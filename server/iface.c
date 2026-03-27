/* =========================================================================
 * server/iface.c — CAN Interface Management  (SPEC §7)
 *
 * Implements:
 *   IFACE_LIST        — enumerate available CAN interfaces
 *   IFACE_ATTACH      — open SocketCAN socket, expose app-plane TCP port
 *   IFACE_DETACH      — close SocketCAN socket and app-plane listener
 *   IFACE_VCAN_CREATE — create a virtual CAN interface via NETLINK_ROUTE
 *   IFACE_VCAN_DESTROY— destroy a virtual CAN interface via NETLINK_ROUTE
 *
 * All multi-byte values on the wire are big-endian (SPEC §3.2).
 * ========================================================================= */

#include "iface.h"
#include "proto.h"
#include "session.h"
#include "ash/proto.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

/* ARPHRD_CAN is 280; define it if the system headers don't provide it. */
#ifndef ARPHRD_CAN
#define ARPHRD_CAN 280
#endif

#define MAX_IFACE_TABLE  64
#define NL_BUF_SIZE      8192

/* -------------------------------------------------------------------------
 * Interface attachment table
 * ---------------------------------------------------------------------- */

typedef struct {
    char     name[IFNAMSIZ];
    uint32_t session_id;      /* 0 = not attached */
    int      client_fd;       /* TCP fd of owning client (-1 if none) */
    int      can_fd;          /* SocketCAN socket fd (-1 if not attached) */
    int      app_listen_fd;   /* application-plane TCP listener fd */
    uint16_t app_port;        /* application-plane port */
} iface_entry_t;

static iface_entry_t g_iface_table[MAX_IFACE_TABLE];
static int           g_iface_count;
static server_t     *g_server;
static int           g_netlink_fd = -1;   /* RTMGRP_LINK monitoring socket */
static uint32_t      g_nl_seq     = 1;    /* per-process netlink sequence counter */

/* -------------------------------------------------------------------------
 * Netlink helpers
 * ---------------------------------------------------------------------- */

/* Pointer to the next attribute slot after the current message content. */
#define NLMSG_TAIL(n) \
    ((struct rtattr *)(((char *)(n)) + NLMSG_ALIGN((n)->nlmsg_len)))

/*
 * Append a netlink attribute to message *n.
 * Returns 0 on success, -1 if the buffer would overflow.
 */
static int nl_addattr(struct nlmsghdr *n, size_t maxlen,
                      int type, const void *data, int alen)
{
    size_t len = RTA_LENGTH((size_t)alen);
    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
        return -1;
    struct rtattr *rta = NLMSG_TAIL(n);
    rta->rta_type = (unsigned short)type;
    rta->rta_len  = (unsigned short)len;
    if (alen && data)
        memcpy(RTA_DATA(rta), data, (size_t)alen);
    n->nlmsg_len = (unsigned int)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len));
    return 0;
}

/* Begin a nested attribute; returns a pointer to the nest header. */
static struct rtattr *nl_nest_start(struct nlmsghdr *n, size_t maxlen, int type)
{
    struct rtattr *nest = NLMSG_TAIL(n);
    nl_addattr(n, maxlen, type, NULL, 0);
    return nest;
}

/* Finish a nested attribute started with nl_nest_start(). */
static void nl_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
    nest->rta_len = (unsigned short)((char *)NLMSG_TAIL(n) - (char *)nest);
}

/* Open a bound AF_NETLINK/NETLINK_ROUTE socket for one-shot operations. */
static int nl_open_temp(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Send a netlink message and block until we receive NLMSG_ERROR (ACK/NAK).
 * Returns 0 on success (ACK with error == 0), -1 on failure.
 */
static int nl_talk(int fd, struct nlmsghdr *req, size_t req_len)
{
    if (send(fd, req, req_len, 0) < 0)
        return -1;

    char buf[NL_BUF_SIZE];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
            return -1;
        struct nlmsghdr *h = (struct nlmsghdr *)(void *)buf;
        for (; NLMSG_OK(h, (unsigned int)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(h);
                if (err->error != 0) {
                    errno = -(err->error);
                    return -1;
                }
                return 0;
            }
            if (h->nlmsg_type == NLMSG_DONE)
                return 0;
        }
        break;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * CAN interface enumeration via RTM_GETLINK dump
 * ---------------------------------------------------------------------- */

typedef struct {
    char name[IFNAMSIZ];
} can_iface_info_t;

/*
 * Enumerate all CAN interfaces (ifi_type == ARPHRD_CAN).
 * Fills *out with at most *max* entries and returns the count found,
 * or -1 on error.
 */
static int enumerate_can_ifaces(can_iface_info_t *out, int max)
{
    int fd = nl_open_temp();
    if (fd < 0)
        return -1;

    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifm;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type  = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = g_nl_seq++;
    req.ifm.ifi_family  = AF_UNSPEC;

    if (send(fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        close(fd);
        return -1;
    }

    int  count = 0;
    int  done  = 0;
    char buf[NL_BUF_SIZE];
    while (!done) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
            break;
        struct nlmsghdr *h = (struct nlmsghdr *)(void *)buf;
        for (; NLMSG_OK(h, (unsigned int)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
            if (h->nlmsg_type != RTM_NEWLINK)  continue;

            struct ifinfomsg *ifi = NLMSG_DATA(h);
            if (ifi->ifi_type != ARPHRD_CAN)
                continue;

            int attr_len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
            struct rtattr *attr = IFLA_RTA(ifi);
            for (; RTA_OK(attr, attr_len); attr = RTA_NEXT(attr, attr_len)) {
                if (attr->rta_type == IFLA_IFNAME && count < max) {
                    strncpy(out[count].name, RTA_DATA(attr), IFNAMSIZ - 1);
                    out[count].name[IFNAMSIZ - 1] = '\0';
                    count++;
                    break;
                }
            }
        }
    }
    close(fd);
    return count;
}

/* -------------------------------------------------------------------------
 * Attachment table helpers
 * ---------------------------------------------------------------------- */

static iface_entry_t *iface_table_find(const char *name)
{
    for (int i = 0; i < g_iface_count; i++) {
        if (strcmp(g_iface_table[i].name, name) == 0)
            return &g_iface_table[i];
    }
    return NULL;
}

static iface_entry_t *iface_table_get_or_add(const char *name)
{
    iface_entry_t *e = iface_table_find(name);
    if (e)
        return e;
    if (g_iface_count >= MAX_IFACE_TABLE)
        return NULL;
    e = &g_iface_table[g_iface_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, IFNAMSIZ - 1);
    e->can_fd        = -1;
    e->app_listen_fd = -1;
    e->client_fd     = -1;
    return e;
}

static void iface_entry_detach(iface_entry_t *e)
{
    if (e->can_fd >= 0) {
        close(e->can_fd);
        e->can_fd = -1;
    }
    if (e->app_listen_fd >= 0) {
        close(e->app_listen_fd);
        e->app_listen_fd = -1;
    }
    e->session_id = 0;
    e->client_fd  = -1;
    e->app_port   = 0;
}

/* -------------------------------------------------------------------------
 * Bitrate configuration via netlink  (SPEC §7.2)
 *
 * Brings the interface down, sets IFLA_CAN_BITTIMING, brings it back up.
 * A bitrate of 0 is a no-op (caller should not call this function with 0).
 * ---------------------------------------------------------------------- */

static int iface_set_bitrate(const char *ifname, uint32_t bitrate)
{
    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
        return -1;

    int fd = nl_open_temp();
    if (fd < 0)
        return -1;

    /* Step 1: bring interface DOWN */
    {
        struct {
            struct nlmsghdr  nlh;
            struct ifinfomsg ifi;
        } req;
        memset(&req, 0, sizeof(req));
        req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifi));
        req.nlh.nlmsg_type  = RTM_NEWLINK;
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        req.nlh.nlmsg_seq   = g_nl_seq++;
        req.ifi.ifi_family  = AF_UNSPEC;
        req.ifi.ifi_index   = (int)ifindex;
        req.ifi.ifi_flags   = 0;
        req.ifi.ifi_change  = IFF_UP;
        if (nl_talk(fd, &req.nlh, req.nlh.nlmsg_len) < 0) {
            close(fd);
            return -1;
        }
    }

    /* Step 2: set bittiming */
    {
        char buf[512];
        memset(buf, 0, sizeof(buf));
        struct nlmsghdr  *nlh = (struct nlmsghdr *)(void *)buf;
        struct ifinfomsg *ifi = NLMSG_DATA(nlh);

        nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
        nlh->nlmsg_type  = RTM_NEWLINK;
        nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        nlh->nlmsg_seq   = g_nl_seq++;
        ifi->ifi_family  = AF_UNSPEC;
        ifi->ifi_index   = (int)ifindex;

        struct rtattr *linkinfo = nl_nest_start(nlh, sizeof(buf), IFLA_LINKINFO);
        const char *kind = "can";
        nl_addattr(nlh, sizeof(buf), IFLA_INFO_KIND, kind, (int)strlen(kind) + 1);
        struct rtattr *data = nl_nest_start(nlh, sizeof(buf), IFLA_INFO_DATA);

        struct can_bittiming bt;
        memset(&bt, 0, sizeof(bt));
        bt.bitrate = bitrate;
        nl_addattr(nlh, sizeof(buf), IFLA_CAN_BITTIMING, &bt, (int)sizeof(bt));

        nl_nest_end(nlh, data);
        nl_nest_end(nlh, linkinfo);

        if (nl_talk(fd, nlh, nlh->nlmsg_len) < 0) {
            fprintf(stderr,
                    "ash-server: warning: could not set bitrate %u on %s: %s\n",
                    bitrate, ifname, strerror(errno));
            /* Non-fatal: interface may already be at the requested rate. */
        }
    }

    /* Step 3: bring interface UP */
    {
        struct {
            struct nlmsghdr  nlh;
            struct ifinfomsg ifi;
        } req;
        memset(&req, 0, sizeof(req));
        req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifi));
        req.nlh.nlmsg_type  = RTM_NEWLINK;
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        req.nlh.nlmsg_seq   = g_nl_seq++;
        req.ifi.ifi_family  = AF_UNSPEC;
        req.ifi.ifi_index   = (int)ifindex;
        req.ifi.ifi_flags   = IFF_UP;
        req.ifi.ifi_change  = IFF_UP;
        if (nl_talk(fd, &req.nlh, req.nlh.nlmsg_len) < 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

/* -------------------------------------------------------------------------
 * Virtual CAN management via RTM_NEWLINK / RTM_DELLINK  (SPEC §7.4)
 * ---------------------------------------------------------------------- */

static int vcan_create(const char *name)
{
    int fd = nl_open_temp();
    if (fd < 0)
        return -1;

    /* RTM_NEWLINK: create the vcan interface */
    char buf[512];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr  *nlh = (struct nlmsghdr *)(void *)buf;
    struct ifinfomsg *ifi = NLMSG_DATA(nlh);

    nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(*ifi));
    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nlh->nlmsg_seq   = g_nl_seq++;
    ifi->ifi_family  = AF_UNSPEC;

    nl_addattr(nlh, sizeof(buf), IFLA_IFNAME, name, (int)strlen(name) + 1);
    struct rtattr *linkinfo = nl_nest_start(nlh, sizeof(buf), IFLA_LINKINFO);
    const char *kind = "vcan";
    nl_addattr(nlh, sizeof(buf), IFLA_INFO_KIND, kind, (int)strlen(kind) + 1);
    nl_nest_end(nlh, linkinfo);

    int ret = nl_talk(fd, nlh, nlh->nlmsg_len);
    close(fd);
    if (ret < 0)
        return -1;

    /* Bring the interface up */
    unsigned int ifindex = if_nametoindex(name);
    if (ifindex == 0)
        return -1;

    fd = nl_open_temp();
    if (fd < 0)
        return -1;

    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifi));
    req.nlh.nlmsg_type  = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq   = g_nl_seq++;
    req.ifi.ifi_family  = AF_UNSPEC;
    req.ifi.ifi_index   = (int)ifindex;
    req.ifi.ifi_flags   = IFF_UP;
    req.ifi.ifi_change  = IFF_UP;

    ret = nl_talk(fd, &req.nlh, req.nlh.nlmsg_len);
    close(fd);
    return ret;
}

static int vcan_destroy(const char *name)
{
    unsigned int ifindex = if_nametoindex(name);
    if (ifindex == 0) {
        errno = ENODEV;
        return -1;
    }

    int fd = nl_open_temp();
    if (fd < 0)
        return -1;

    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifi));
    req.nlh.nlmsg_type  = RTM_DELLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq   = g_nl_seq++;
    req.ifi.ifi_family  = AF_UNSPEC;
    req.ifi.ifi_index   = (int)ifindex;

    int ret = nl_talk(fd, &req.nlh, req.nlh.nlmsg_len);
    close(fd);
    return ret;
}

/* -------------------------------------------------------------------------
 * Application-plane TCP listener
 * ---------------------------------------------------------------------- */

static int create_app_listener(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port        = 0;   /* kernel assigns ephemeral port */

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }

    socklen_t slen = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) < 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(sa.sin_port);
    return fd;
}

/* -------------------------------------------------------------------------
 * Message handlers
 * ---------------------------------------------------------------------- */

/*
 * IFACE_LIST  (SPEC §7.1)
 *
 * Request: no payload.
 * Response (MSG_IFACE_LIST_RESP):
 *   1B  interface count
 *   for each:
 *     1B  name_len
 *     NB  name
 *     1B  state  (0x01=available, 0x02=attached-this, 0x03=attached-other)
 */
static int handle_iface_list(int fd, const proto_frame_t *frame)
{
    (void)frame;

    session_t *sess = session_find_by_fd(g_server, fd);
    if (!sess)
        return -1;

    can_iface_info_t ifaces[MAX_IFACE_TABLE];
    int count = enumerate_can_ifaces(ifaces, MAX_IFACE_TABLE);
    if (count < 0)
        count = 0;

    uint8_t buf[8192];
    int     pos = 0;
    buf[pos++] = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        uint8_t nlen = (uint8_t)strlen(ifaces[i].name);
        buf[pos++] = nlen;
        memcpy(&buf[pos], ifaces[i].name, nlen);
        pos += nlen;

        iface_entry_t *e = iface_table_find(ifaces[i].name);
        uint8_t state;
        if (!e || e->session_id == 0)
            state = 0x01;
        else if (e->session_id == sess->session_id)
            state = 0x02;
        else
            state = 0x03;
        buf[pos++] = state;
    }

    return proto_send_ack(fd, MSG_IFACE_LIST_RESP, buf, (uint32_t)pos);
}

/*
 * IFACE_ATTACH  (SPEC §7.2)
 *
 * Payload:
 *   1B  name_len
 *   NB  name
 *   1B  mode  (0x01=CAN2.0A, 0x02=CAN2.0B, 0x03=CAN FD)
 *   4B  bitrate (big-endian, bps; 0 = do not reconfigure)
 *   1B  filter_count
 *   for each filter:
 *     4B  can_id  (big-endian)
 *     4B  mask    (big-endian)
 *
 * Response (MSG_IFACE_ATTACH_ACK):
 *   2B  app_port (big-endian)
 */
static int handle_iface_attach(int fd, const proto_frame_t *frame)
{
    session_t *sess = session_find_by_fd(g_server, fd);
    if (!sess)
        return -1;

    /* Minimum: name_len(1) + name(≥1) + mode(1) + bitrate(4) + filter_count(1) */
    if (frame->hdr.payload_len < 8u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    uint8_t name_len = *p++; remaining--;
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        remaining < (uint32_t)name_len) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, p, name_len);
    name[name_len] = '\0';
    p += name_len; remaining -= name_len;

    if (remaining < 6u) {   /* mode(1) + bitrate(4) + filter_count(1) */
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint8_t  mode    = *p++; remaining--;
    uint32_t bitrate = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                     | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    p += 4; remaining -= 4;
    uint8_t filter_count = *p++; remaining--;

    if (remaining < (uint32_t)filter_count * 8u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Verify the interface exists */
    if (if_nametoindex(name) == 0) {
        proto_send_err(fd, ERR_IFACE_NOT_FOUND, NULL);
        return 0;
    }

    /* Enforce exclusive attachment */
    iface_entry_t *e = iface_table_find(name);
    if (e && e->session_id != 0) {
        proto_send_err(fd, ERR_IFACE_ALREADY_ATTACHED, NULL);
        return 0;
    }

    /* Optionally configure bitrate */
    if (bitrate != 0 && iface_set_bitrate(name, bitrate) < 0)
        fprintf(stderr, "ash-server: bitrate config failed for %s (continuing)\n",
                name);

    /* Open SocketCAN socket */
    int can_sock = socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (can_sock < 0) {
        proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
        return 0;
    }

    /* Enable CAN FD frames when mode requests it */
    if (mode == 0x03) {
        int enable = 1;
        if (setsockopt(can_sock, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                       &enable, sizeof(enable)) < 0) {
            close(can_sock);
            proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
            return 0;
        }
    }

    /* Apply RX filters (zero filter_count leaves the socket promiscuous) */
    if (filter_count > 0) {
        struct can_filter filters[256];   /* max 255 entries (uint8_t count) */
        for (int i = 0; i < (int)filter_count; i++) {
            filters[i].can_id   = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                                 | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
            filters[i].can_mask = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                                 | ((uint32_t)p[6] <<  8) |  (uint32_t)p[7];
            p += 8;
        }
        if (setsockopt(can_sock, SOL_CAN_RAW, CAN_RAW_FILTER,
                       filters,
                       (socklen_t)(filter_count * sizeof(struct can_filter))) < 0) {
            close(can_sock);
            proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
            return 0;
        }
    }

    /* Bind to the named interface */
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = (int)if_nametoindex(name);
    if (bind(can_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(can_sock);
        proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
        return 0;
    }

    /* Create application-plane TCP listener on an ephemeral port */
    uint16_t app_port = 0;
    int app_fd = create_app_listener(&app_port);
    if (app_fd < 0) {
        close(can_sock);
        proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
        return 0;
    }

    /* Record the attachment */
    e = iface_table_get_or_add(name);
    if (!e) {
        close(can_sock);
        close(app_fd);
        proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
        return 0;
    }
    e->session_id    = sess->session_id;
    e->client_fd     = fd;
    e->can_fd        = can_sock;
    e->app_listen_fd = app_fd;
    e->app_port      = app_port;

    printf("ash-server: session %u attached to %s (mode=0x%02x, app port=%u)\n",
           sess->session_id, name, mode, app_port);

    /* IFACE_ATTACH_ACK: 2-byte big-endian port */
    uint8_t ack[2];
    ack[0] = (uint8_t)((app_port >> 8) & 0xFFu);
    ack[1] = (uint8_t)( app_port       & 0xFFu);
    return proto_send_ack(fd, MSG_IFACE_ATTACH_ACK, ack, 2);
}

/*
 * IFACE_DETACH  (SPEC §7.3)
 *
 * Payload:
 *   1B  name_len
 *   NB  name
 */
static int handle_iface_detach(int fd, const proto_frame_t *frame)
{
    session_t *sess = session_find_by_fd(g_server, fd);
    if (!sess)
        return -1;

    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint8_t name_len = frame->payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        frame->hdr.payload_len < (uint32_t)(1u + name_len)) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, &frame->payload[1], name_len);
    name[name_len] = '\0';

    iface_entry_t *e = iface_table_find(name);
    if (!e || e->session_id != sess->session_id) {
        proto_send_err(fd, ERR_IFACE_NOT_FOUND, NULL);
        return 0;
    }

    printf("ash-server: session %u detaching from %s\n",
           sess->session_id, name);

    iface_entry_detach(e);
    return proto_send_ack(fd, MSG_IFACE_DETACH_ACK, NULL, 0);
}

/*
 * IFACE_VCAN_CREATE  (SPEC §7.4)
 *
 * Payload:
 *   1B  name_len
 *   NB  name  (e.g. "vcan0")
 */
static int handle_iface_vcan_create(int fd, const proto_frame_t *frame)
{
    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint8_t name_len = frame->payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        frame->hdr.payload_len < (uint32_t)(1u + name_len)) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, &frame->payload[1], name_len);
    name[name_len] = '\0';

    if (vcan_create(name) < 0) {
        fprintf(stderr, "ash-server: vcan_create(%s) failed: %s\n",
                name, strerror(errno));
        proto_send_err(fd, ERR_IFACE_ATTACH_FAILED, NULL);
        return 0;
    }

    printf("ash-server: created vcan interface %s\n", name);
    return proto_send_ack(fd, MSG_IFACE_VCAN_ACK, NULL, 0);
}

/*
 * IFACE_VCAN_DESTROY  (SPEC §7.4)
 *
 * Payload:
 *   1B  name_len
 *   NB  name
 */
static int handle_iface_vcan_destroy(int fd, const proto_frame_t *frame)
{
    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint8_t name_len = frame->payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        frame->hdr.payload_len < (uint32_t)(1u + name_len)) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, &frame->payload[1], name_len);
    name[name_len] = '\0';

    if (vcan_destroy(name) < 0) {
        proto_send_err(fd, ERR_IFACE_NOT_FOUND, NULL);
        return 0;
    }

    printf("ash-server: destroyed vcan interface %s\n", name);
    return proto_send_ack(fd, MSG_IFACE_VCAN_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * Netlink monitoring — RTMGRP_LINK events
 * ---------------------------------------------------------------------- */

/*
 * Called from server_run() when g_netlink_fd is readable.
 *
 * Handles RTM_DELLINK and RTM_NEWLINK-with-IFF_UP-cleared events for
 * attached CAN interfaces.  Sends MSG_NOTIFY_IFACE_DOWN to the owning
 * session and cleans up the attachment.
 */
void iface_handle_netlink(void)
{
    char buf[NL_BUF_SIZE];
    ssize_t n = recv(g_netlink_fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0)
        return;

    struct nlmsghdr *h = (struct nlmsghdr *)(void *)buf;
    for (; NLMSG_OK(h, (unsigned int)n); h = NLMSG_NEXT(h, n)) {
        if (h->nlmsg_type != RTM_NEWLINK && h->nlmsg_type != RTM_DELLINK)
            continue;

        struct ifinfomsg *ifi = NLMSG_DATA(h);
        if (ifi->ifi_type != ARPHRD_CAN)
            continue;

        /* Extract interface name */
        char ifname[IFNAMSIZ] = {0};
        int attr_len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
        struct rtattr *attr = IFLA_RTA(ifi);
        for (; RTA_OK(attr, attr_len); attr = RTA_NEXT(attr, attr_len)) {
            if (attr->rta_type == IFLA_IFNAME) {
                strncpy(ifname, RTA_DATA(attr), IFNAMSIZ - 1);
                break;
            }
        }
        if (ifname[0] == '\0')
            continue;

        int went_down = (h->nlmsg_type == RTM_DELLINK) ||
                        !(ifi->ifi_flags & IFF_UP);
        if (!went_down)
            continue;

        iface_entry_t *e = iface_table_find(ifname);
        if (!e || e->session_id == 0)
            continue;

        printf("ash-server: interface %s went down, notifying session %u\n",
               ifname, e->session_id);

        /* NOTIFY_IFACE_DOWN payload: name_len(1) + name(N) */
        uint8_t notif[IFNAMSIZ + 1];
        uint8_t nlen = (uint8_t)strlen(ifname);
        notif[0] = nlen;
        memcpy(&notif[1], ifname, nlen);
        proto_send_ack(e->client_fd, MSG_NOTIFY_IFACE_DOWN,
                       notif, (uint32_t)(1u + nlen));

        iface_entry_detach(e);
    }
}

/* -------------------------------------------------------------------------
 * Session cleanup hook
 * ---------------------------------------------------------------------- */

void iface_session_cleanup(server_t *s, uint32_t session_id)
{
    (void)s;
    for (int i = 0; i < g_iface_count; i++) {
        if (g_iface_table[i].session_id == session_id) {
            printf("ash-server: releasing interface %s from session %u\n",
                   g_iface_table[i].name, session_id);
            iface_entry_detach(&g_iface_table[i]);
        }
    }
}

/* -------------------------------------------------------------------------
 * Initialization and teardown
 * ---------------------------------------------------------------------- */

void iface_set_server(server_t *s)
{
    g_server = s;
}

int iface_init(server_t *s)
{
    memset(g_iface_table, 0, sizeof(g_iface_table));
    for (int i = 0; i < MAX_IFACE_TABLE; i++) {
        g_iface_table[i].can_fd        = -1;
        g_iface_table[i].app_listen_fd = -1;
        g_iface_table[i].client_fd     = -1;
    }
    g_iface_count = 0;
    g_server      = s;

    /* Open RTMGRP_LINK monitoring socket */
    g_netlink_fd = socket(AF_NETLINK,
                          SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                          NETLINK_ROUTE);
    if (g_netlink_fd < 0) {
        perror("iface: netlink socket");
        return -1;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK;
    if (bind(g_netlink_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("iface: netlink bind");
        close(g_netlink_fd);
        g_netlink_fd = -1;
        return -1;
    }

    if (server_add_fd(s, g_netlink_fd, EPOLLIN) < 0) {
        close(g_netlink_fd);
        g_netlink_fd = -1;
        return -1;
    }

    s->netlink_fd = g_netlink_fd;

    iface_register_handlers();
    return 0;
}

void iface_destroy(void)
{
    for (int i = 0; i < MAX_IFACE_TABLE; i++) {
        if (g_iface_table[i].session_id != 0)
            iface_entry_detach(&g_iface_table[i]);
    }
    if (g_netlink_fd >= 0) {
        close(g_netlink_fd);
        g_netlink_fd = -1;
    }
}

/* -------------------------------------------------------------------------
 * Handler registration
 * ---------------------------------------------------------------------- */

void iface_register_handlers(void)
{
    proto_register_handler(MSG_IFACE_LIST,         handle_iface_list);
    proto_register_handler(MSG_IFACE_ATTACH,       handle_iface_attach);
    proto_register_handler(MSG_IFACE_DETACH,       handle_iface_detach);
    proto_register_handler(MSG_IFACE_VCAN_CREATE,  handle_iface_vcan_create);
    proto_register_handler(MSG_IFACE_VCAN_DESTROY, handle_iface_vcan_destroy);
}
