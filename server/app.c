/* =========================================================================
 * server/app.c — Application Plane Runtime  (SPEC §5, §11)
 * ========================================================================= */

#include "app.h"
#include "def.h"
#include "own.h"
#include "iface.h"
#include "proto.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define APP_HDR_SIZE       4u
#define APP_MAX_PAYLOAD    4096u
#define MAX_APP_IFACES     64
#define MAX_APP_CLIENTS    256
#define MAX_CYCLIC_FRAMES  128

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

typedef struct {
    char iface_name[IFNAMSIZ];
    int  can_fd;
    int  app_listen_fd;
} app_iface_t;

typedef struct {
    int  fd;
    char iface_name[IFNAMSIZ];
} app_client_t;

/*
 * Per-frame state: used for both cyclic (timer_fd >= 0) and event-driven
 * (timer_fd == -1) frames so that multiple signals accumulate correctly.
 */
typedef struct {
    char     frame_name[PROTO_MAX_NAME + 1];
    char     iface_name[IFNAMSIZ];
    int      timer_fd;
    int      can_sock_fd;
    uint8_t  data[64];
    uint32_t can_id;
    uint8_t  id_type;
    uint8_t  dlc;
    uint8_t  data_len;
} app_cyclic_t;

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static app_iface_t   g_ifaces[MAX_APP_IFACES];
static app_client_t  g_clients[MAX_APP_CLIENTS];
static app_cyclic_t  g_cyclic[MAX_CYCLIC_FRAMES];
static server_t     *g_server;

/* -------------------------------------------------------------------------
 * CAN FD DLC table
 * ---------------------------------------------------------------------- */

static uint8_t dlc_to_len(uint8_t dlc, int is_fd)
{
    static const uint8_t fd_map[16] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    if (!is_fd || dlc <= 8)
        return dlc;
    return fd_map[dlc < 16 ? dlc : 15];
}

/* Reverse: actual data length → encoded CAN FD DLC code */
static uint8_t len_to_dlc(uint8_t len)
{
    if (len <= 8)  return len;
    if (len <= 12) return 9;
    if (len <= 16) return 10;
    if (len <= 20) return 11;
    if (len <= 24) return 12;
    if (len <= 32) return 13;
    if (len <= 48) return 14;
    return 15;
}

/* -------------------------------------------------------------------------
 * Bit packing helpers
 * ---------------------------------------------------------------------- */

static void pack_bits_le(uint8_t *data, uint64_t raw,
                         uint8_t start_bit, uint8_t bit_length)
{
    for (int i = 0; i < (int)bit_length; i++) {
        int bit_pos  = (int)start_bit + i;
        int byte_idx = bit_pos / 8;
        int bit_in   = bit_pos % 8;
        if ((raw >> i) & 1u)
            data[byte_idx] |=  (uint8_t)(1u << bit_in);
        else
            data[byte_idx] &= ~(uint8_t)(1u << bit_in);
    }
}

static void pack_bits_be(uint8_t *data, uint64_t raw,
                         uint8_t start_bit, uint8_t bit_length)
{
    int bit_pos = (int)start_bit;
    for (int i = (int)bit_length - 1; i >= 0; i--) {
        int byte_idx = bit_pos / 8;
        int bit_in   = bit_pos % 8;
        if ((raw >> i) & 1u)
            data[byte_idx] |=  (uint8_t)(1u << bit_in);
        else
            data[byte_idx] &= ~(uint8_t)(1u << bit_in);
        if (bit_in == 0)
            bit_pos += 15;
        else
            bit_pos--;
    }
}

/* -------------------------------------------------------------------------
 * App-plane wire helpers
 * ---------------------------------------------------------------------- */

static int app_send(int fd, uint16_t msg_type, const void *payload, uint16_t len)
{
    uint8_t hdr[APP_HDR_SIZE];
    hdr[0] = (uint8_t)((msg_type >> 8) & 0xFFu);
    hdr[1] = (uint8_t)(msg_type        & 0xFFu);
    hdr[2] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[3] = (uint8_t)(len        & 0xFFu);
    if (write(fd, hdr, APP_HDR_SIZE) != (ssize_t)APP_HDR_SIZE)
        return -1;
    if (len > 0 && payload) {
        ssize_t w = write(fd, payload, len);
        if (w < 0 || (uint16_t)w != len)
            return -1;
    }
    return 0;
}

static int app_send_err(int fd, uint16_t err_code)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)((err_code >> 8) & 0xFFu);
    buf[1] = (uint8_t)(err_code        & 0xFFu);
    return app_send(fd, APP_MSG_APP_ERR, buf, 2);
}

/* Encode a double as 8-byte big-endian IEEE 754 */
static void write_be_double(uint8_t *p, double d)
{
    uint64_t raw;
    memcpy(&raw, &d, sizeof(raw));
    p[0] = (uint8_t)((raw >> 56) & 0xFF);
    p[1] = (uint8_t)((raw >> 48) & 0xFF);
    p[2] = (uint8_t)((raw >> 40) & 0xFF);
    p[3] = (uint8_t)((raw >> 32) & 0xFF);
    p[4] = (uint8_t)((raw >> 24) & 0xFF);
    p[5] = (uint8_t)((raw >> 16) & 0xFF);
    p[6] = (uint8_t)((raw >>  8) & 0xFF);
    p[7] = (uint8_t)( raw        & 0xFF);
}

static double read_be_double(const uint8_t *p)
{
    uint64_t raw = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48)
                 | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
                 | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
                 | ((uint64_t)p[6] <<  8) |  (uint64_t)p[7];
    double d;
    memcpy(&d, &raw, sizeof(d));
    return d;
}

/* -------------------------------------------------------------------------
 * Lookup helpers
 * ---------------------------------------------------------------------- */

static app_iface_t *find_iface_by_listen_fd(int fd)
{
    for (int i = 0; i < MAX_APP_IFACES; i++) {
        if (g_ifaces[i].app_listen_fd == fd)
            return &g_ifaces[i];
    }
    return NULL;
}

static app_iface_t *find_iface_by_can_fd(int fd)
{
    for (int i = 0; i < MAX_APP_IFACES; i++) {
        if (g_ifaces[i].can_fd == fd)
            return &g_ifaces[i];
    }
    return NULL;
}

static app_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < MAX_APP_CLIENTS; i++) {
        if (g_clients[i].fd == fd)
            return &g_clients[i];
    }
    return NULL;
}

static const char *find_iface_for_client(int fd)
{
    app_client_t *c = find_client_by_fd(fd);
    return c ? c->iface_name : NULL;
}

static app_cyclic_t *find_cyclic_by_name_iface(const char *frame_name,
                                                const char *iface_name)
{
    for (int i = 0; i < MAX_CYCLIC_FRAMES; i++) {
        if (g_cyclic[i].can_sock_fd >= 0 &&
            strcmp(g_cyclic[i].frame_name, frame_name) == 0 &&
            strcmp(g_cyclic[i].iface_name, iface_name) == 0)
            return &g_cyclic[i];
    }
    return NULL;
}

static app_cyclic_t *find_cyclic_by_timer_fd(int fd)
{
    for (int i = 0; i < MAX_CYCLIC_FRAMES; i++) {
        if (g_cyclic[i].can_sock_fd >= 0 && g_cyclic[i].timer_fd == fd)
            return &g_cyclic[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * CAN frame transmission helper
 * ---------------------------------------------------------------------- */

static int transmit_can_frame(int can_sock, uint32_t can_id, uint8_t id_type,
                               uint8_t dlc, const uint8_t *data)
{
    uint32_t wire_id = can_id | (id_type == 0x02u ? CAN_EFF_FLAG : 0u);

    if (dlc > 8) {
        struct canfd_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id = wire_id;
        f.len    = dlc_to_len(dlc, 1);
        f.flags  = 0;
        memcpy(f.data, data, f.len);
        if (write(can_sock, &f, CANFD_MTU) != (ssize_t)CANFD_MTU)
            return -1;
    } else {
        struct can_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id  = wire_id;
        f.can_dlc = dlc;
        memcpy(f.data, data, dlc);
        if (write(can_sock, &f, CAN_MTU) != (ssize_t)CAN_MTU)
            return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * app_accept_client
 * ---------------------------------------------------------------------- */

static void app_accept_client(int app_listen_fd)
{
    app_iface_t *iface = find_iface_by_listen_fd(app_listen_fd);
    if (!iface)
        return;

    int cfd = accept4(app_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("app: accept4");
        return;
    }

    /* Find a free slot */
    app_client_t *slot = NULL;
    for (int i = 0; i < MAX_APP_CLIENTS; i++) {
        if (g_clients[i].fd < 0) {
            slot = &g_clients[i];
            break;
        }
    }
    if (!slot) {
        fprintf(stderr, "app: no free client slots\n");
        close(cfd);
        return;
    }

    if (server_add_fd(g_server, cfd, EPOLLIN | EPOLLRDHUP) < 0) {
        close(cfd);
        return;
    }

    slot->fd = cfd;
    strncpy(slot->iface_name, iface->iface_name, IFNAMSIZ - 1);
    slot->iface_name[IFNAMSIZ - 1] = '\0';

    printf("ash-server: app client connected on interface %s (fd=%d)\n",
           iface->iface_name, cfd);
}

/* -------------------------------------------------------------------------
 * app_client_disconnect
 * ---------------------------------------------------------------------- */

static void app_client_disconnect(int fd)
{
    server_del_fd(g_server, fd);

    app_client_t *c = find_client_by_fd(fd);
    if (c) {
        c->fd = -1;
        c->iface_name[0] = '\0';
    }

    close(fd);
    printf("ash-server: app client disconnected (fd=%d)\n", fd);
}

/* -------------------------------------------------------------------------
 * handle_sig_write
 * ---------------------------------------------------------------------- */

static int handle_sig_write(int client_fd, const uint8_t *payload, uint16_t plen,
                             const char *iface_name)
{
    /* Minimum: name_len(1) + name(≥1) + value(8) = 10 */
    if (plen < 10u) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_FOUND);
        return 0;
    }

    uint8_t name_len = payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        (uint16_t)(1u + name_len + 8u) > plen) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_FOUND);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, payload + 1, name_len);
    name[name_len] = '\0';

    double value = read_be_double(payload + 1 + name_len);

    /* Resolve signal → PDU → frame */
    def_sig_info_t info;
    int rc = def_resolve_signal(name, &info);
    if (rc == -1) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_FOUND);
        return 0;
    }
    if (rc == -2) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_MAPPED);
        return 0;
    }

    /* Check ownership: the session that owns the interface must own the signal */
    uint32_t session_id = iface_get_session_id(iface_name);
    if (!own_session_owns_signal(session_id, name)) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_OWNED);
        return 0;
    }

    /* Physical → raw conversion */
    uint64_t raw_bits = 0;

    if (info.data_type == 0x03u) {
        /* float: store physical as 32-bit IEEE 754 */
        float f = (float)value;
        uint32_t raw32;
        memcpy(&raw32, &f, sizeof(raw32));
        raw_bits = (uint64_t)raw32;
    } else if (info.data_type == 0x02u) {
        /* sint */
        double raw_d = (value - info.offset_val) / info.scale;
        int64_t maxv  = (int64_t)1 << (info.bit_length - 1u);
        int64_t minv  = -maxv;
        maxv -= 1;
        if (raw_d > (double)maxv) raw_d = (double)maxv;
        if (raw_d < (double)minv) raw_d = (double)minv;
        int64_t sraw = (int64_t)raw_d;
        /* two's complement mask */
        if (info.bit_length < 64u)
            raw_bits = (uint64_t)sraw & (((uint64_t)1u << info.bit_length) - 1u);
        else
            raw_bits = (uint64_t)sraw;
    } else {
        /* uint */
        double raw_d = (value - info.offset_val) / info.scale;
        uint64_t maxv = (info.bit_length < 64u)
                        ? (((uint64_t)1u << info.bit_length) - 1u)
                        : UINT64_MAX;
        if (raw_d < 0.0)      raw_d = 0.0;
        if (raw_d > (double)maxv) raw_d = (double)maxv;
        raw_bits = (uint64_t)raw_d;
    }

    /* Determine actual data length for the frame */
    uint8_t data_len = dlc_to_len(info.frame_dlc, info.frame_dlc > 8 ? 1 : 0);

    /* Find or create per-frame state entry */
    app_cyclic_t *cy = find_cyclic_by_name_iface(info.frame_name, iface_name);
    if (!cy) {
        /* Allocate a new slot */
        app_cyclic_t *slot = NULL;
        for (int i = 0; i < MAX_CYCLIC_FRAMES; i++) {
            if (g_cyclic[i].can_sock_fd < 0) {
                slot = &g_cyclic[i];
                break;
            }
        }
        if (!slot) {
            fprintf(stderr, "app: no free cyclic frame slots\n");
            return 0;
        }

        memset(slot->data, 0, sizeof(slot->data));
        strncpy(slot->frame_name, info.frame_name, PROTO_MAX_NAME);
        slot->frame_name[PROTO_MAX_NAME] = '\0';
        strncpy(slot->iface_name, iface_name, IFNAMSIZ - 1);
        slot->iface_name[IFNAMSIZ - 1] = '\0';
        slot->can_id      = info.frame_can_id;
        slot->id_type     = info.frame_id_type;
        slot->dlc         = info.frame_dlc;
        slot->data_len    = data_len;
        slot->timer_fd    = -1;
        slot->can_sock_fd = iface_get_can_fd(iface_name);
        cy = slot;
    }

    /* Pack signal bits into frame data buffer */
    uint8_t abs_start = (uint8_t)((int)info.pdu_byte_offset * 8 + (int)info.start_bit);
    if (info.byte_order == 0x01u)
        pack_bits_le(cy->data, raw_bits, abs_start, info.bit_length);
    else
        pack_bits_be(cy->data, raw_bits, abs_start, info.bit_length);

    /* Update cached signal value */
    def_update_signal_value(name, value);

    if (info.frame_tx_period > 0) {
        /* Cyclic: arm timerfd if not already running */
        if (cy->timer_fd < 0) {
            int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (tfd < 0) {
                perror("app: timerfd_create");
                return 0;
            }

            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            uint32_t period_ms = info.frame_tx_period;
            its.it_value.tv_sec    = (time_t)(period_ms / 1000u);
            its.it_value.tv_nsec   = (long)((period_ms % 1000u) * 1000000u);
            its.it_interval.tv_sec  = its.it_value.tv_sec;
            its.it_interval.tv_nsec = its.it_value.tv_nsec;

            if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
                perror("app: timerfd_settime");
                close(tfd);
                return 0;
            }

            if (server_add_fd(g_server, tfd, EPOLLIN) < 0) {
                close(tfd);
                return 0;
            }

            cy->timer_fd = tfd;
        }
    } else {
        /* Event-driven: transmit immediately */
        transmit_can_frame(cy->can_sock_fd, cy->can_id, cy->id_type,
                           cy->dlc, cy->data);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * handle_sig_read
 * ---------------------------------------------------------------------- */

static int handle_sig_read(int client_fd, const uint8_t *payload, uint16_t plen)
{
    /* Minimum: name_len(1) + name(≥1) = 2 */
    if (plen < 2u) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_FOUND);
        return 0;
    }

    uint8_t name_len = payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        (uint16_t)(1u + name_len) > plen) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_FOUND);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, payload + 1, name_len);
    name[name_len] = '\0';

    /* def_get_signal_value returns 0.0 even for unknown signals, so check
     * existence separately */
    def_sig_info_t info;
    if (def_resolve_signal(name, &info) == -1) {
        app_send_err(client_fd, APP_ERR_SIG_NOT_FOUND);
        return 0;
    }

    double value = def_get_signal_value(name);

    /* SIG_READ_RESP: name_len(1) + name(N) + value(8B double BE) */
    uint8_t buf[1 + PROTO_MAX_NAME + 8];
    buf[0] = name_len;
    memcpy(buf + 1, name, name_len);
    write_be_double(buf + 1 + name_len, value);

    app_send(client_fd, APP_MSG_SIG_READ_RESP, buf, (uint16_t)(1u + name_len + 8u));
    return 0;
}

/* -------------------------------------------------------------------------
 * handle_frame_tx
 * ---------------------------------------------------------------------- */

static int handle_frame_tx(int client_fd, const uint8_t *payload, uint16_t plen,
                            const char *iface_name)
{
    /* Minimum: can_id(4) + dlc(1) + flags(1) = 6 */
    if (plen < 6u) {
        app_send_err(client_fd, APP_ERR_DLC_INVALID);
        return 0;
    }

    uint32_t can_id_wire = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16)
                         | ((uint32_t)payload[2] <<  8) |  (uint32_t)payload[3];
    uint8_t dlc   = payload[4];
    uint8_t flags = payload[5];

    int is_fd = (flags & 0x01u) ? 1 : 0;

    /* Validate DLC */
    if (!is_fd && dlc > 8) {
        app_send_err(client_fd, APP_ERR_DLC_INVALID);
        return 0;
    }
    if (dlc > 15) {
        app_send_err(client_fd, APP_ERR_DLC_INVALID);
        return 0;
    }

    uint8_t data_len = dlc_to_len(dlc, is_fd);

    if ((uint16_t)(6u + data_len) > plen) {
        app_send_err(client_fd, APP_ERR_DLC_INVALID);
        return 0;
    }

    const uint8_t *data = payload + 6;

    /* Extract CAN ID and type */
    uint32_t can_id;
    uint8_t  id_type;
    if (can_id_wire & 0x80000000u) {
        can_id  = can_id_wire & CAN_EFF_MASK;
        id_type = 0x02u;
    } else {
        can_id  = can_id_wire & CAN_SFF_MASK;
        id_type = 0x01u;
    }

    int can_sock = iface_get_can_fd(iface_name);
    if (can_sock < 0) {
        app_send_err(client_fd, APP_ERR_FRAME_NOT_FOUND);
        return 0;
    }

    uint32_t wire_id = can_id | (id_type == 0x02u ? CAN_EFF_FLAG : 0u);

    if (dlc > 8 || is_fd) {
        struct canfd_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id = wire_id;
        f.len    = data_len;
        f.flags  = (flags & 0x02u) ? CANFD_BRS : 0;
        memcpy(f.data, data, data_len);
        if (write(can_sock, &f, CANFD_MTU) != (ssize_t)CANFD_MTU)
            perror("app: FRAME_TX canfd write");
    } else {
        struct can_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id  = wire_id;
        f.can_dlc = dlc;
        memcpy(f.data, data, dlc);
        if (write(can_sock, &f, CAN_MTU) != (ssize_t)CAN_MTU)
            perror("app: FRAME_TX can write");
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * app_handle_client  (EPOLLIN on a client fd)
 * ---------------------------------------------------------------------- */

static int app_handle_client(int fd)
{
    /* Read 4-byte header */
    uint8_t hdr[APP_HDR_SIZE];
    if (proto_read_exact(fd, hdr, APP_HDR_SIZE) < 0)
        return -1;

    uint16_t msg_type   = ((uint16_t)hdr[0] << 8) | hdr[1];
    uint16_t payload_len = ((uint16_t)hdr[2] << 8) | hdr[3];

    if (payload_len > APP_MAX_PAYLOAD) {
        /* Drain and ignore oversized messages */
        return -1;
    }

    uint8_t  payload_buf[APP_MAX_PAYLOAD];
    uint8_t *payload = payload_buf;

    if (payload_len > 0) {
        if (proto_read_exact(fd, payload, payload_len) < 0)
            return -1;
    }

    const char *iface_name = find_iface_for_client(fd);
    if (!iface_name)
        return -1;

    switch (msg_type) {
    case APP_MSG_SIG_WRITE:
        handle_sig_write(fd, payload, payload_len, iface_name);
        break;
    case APP_MSG_SIG_READ:
        handle_sig_read(fd, payload, payload_len);
        break;
    case APP_MSG_FRAME_TX:
        handle_frame_tx(fd, payload, payload_len, iface_name);
        break;
    default:
        /* Unknown message type: ignore */
        break;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * app_handle_can_frame  (EPOLLIN on a CAN socket fd)
 * ---------------------------------------------------------------------- */

static void app_handle_can_frame(int can_fd_param)
{
    app_iface_t *iface = find_iface_by_can_fd(can_fd_param);
    if (!iface)
        return;

    struct canfd_frame f;
    memset(&f, 0, sizeof(f));

    ssize_t n = read(can_fd_param, &f, sizeof(f));
    if (n < 0)
        return;

    uint8_t  data_len;
    uint8_t  flags_byte = 0;
    uint8_t  dlc;

    if (n == (ssize_t)CAN_MTU) {
        /* CAN 2.0 frame */
        struct can_frame *cf = (struct can_frame *)(void *)&f;
        dlc      = cf->can_dlc;
        data_len = cf->can_dlc;
        flags_byte = 0;
    } else if (n == (ssize_t)CANFD_MTU) {
        /* CAN FD frame: f.len is data length; encode back to DLC code */
        data_len = f.len;
        dlc      = len_to_dlc(f.len);
        flags_byte = 0x01u;
        if (f.flags & CANFD_BRS)
            flags_byte |= 0x02u;
    } else {
        return;
    }

    uint8_t id_type;
    uint32_t can_id;
    uint32_t can_id_wire = f.can_id;

    if (can_id_wire & CAN_EFF_FLAG) {
        can_id  = can_id_wire & CAN_EFF_MASK;
        id_type = 0x02u;
    } else {
        can_id  = can_id_wire & CAN_SFF_MASK;
        id_type = 0x01u;
    }

    /* Decode signals */
    def_decoded_sig_t decoded[64];
    int nsigs = def_decode_frame_signals(can_id, id_type,
                                         f.data, data_len,
                                         decoded, 64);

    /* Build FRAME_RX payload:
     * can_id(4B BE, bit31=EFF) + dlc(1B) + flags(1B) + data */
    uint8_t frame_rx_buf[4 + 1 + 1 + 64];
    uint32_t wire_id_out = can_id | (id_type == 0x02u ? 0x80000000u : 0u);
    frame_rx_buf[0] = (uint8_t)((wire_id_out >> 24) & 0xFF);
    frame_rx_buf[1] = (uint8_t)((wire_id_out >> 16) & 0xFF);
    frame_rx_buf[2] = (uint8_t)((wire_id_out >>  8) & 0xFF);
    frame_rx_buf[3] = (uint8_t)( wire_id_out        & 0xFF);
    frame_rx_buf[4] = dlc;
    frame_rx_buf[5] = flags_byte;
    memcpy(&frame_rx_buf[6], f.data, data_len);
    uint16_t frame_rx_len = (uint16_t)(6u + data_len);

    /* Push to all app clients on this interface */
    for (int i = 0; i < MAX_APP_CLIENTS; i++) {
        if (g_clients[i].fd < 0)
            continue;
        if (strcmp(g_clients[i].iface_name, iface->iface_name) != 0)
            continue;

        int cfd = g_clients[i].fd;

        /* Send SIG_RX for each decoded signal */
        for (int s = 0; s < nsigs; s++) {
            uint8_t sig_buf[1 + PROTO_MAX_NAME + 8];
            uint8_t nlen = (uint8_t)strlen(decoded[s].sig_name);
            sig_buf[0] = nlen;
            memcpy(sig_buf + 1, decoded[s].sig_name, nlen);
            write_be_double(sig_buf + 1 + nlen, decoded[s].value);
            app_send(cfd, APP_MSG_SIG_RX, sig_buf, (uint16_t)(1u + nlen + 8u));
        }

        /* Send FRAME_RX */
        app_send(cfd, APP_MSG_FRAME_RX, frame_rx_buf, frame_rx_len);
    }
}

/* -------------------------------------------------------------------------
 * app_handle_cyclic_timer  (EPOLLIN on a timerfd)
 * ---------------------------------------------------------------------- */

static void app_handle_cyclic_timer(int timer_fd)
{
    /* Drain the timerfd */
    uint64_t exp;
    (void)read(timer_fd, &exp, sizeof(exp));

    app_cyclic_t *cy = find_cyclic_by_timer_fd(timer_fd);
    if (!cy)
        return;

    transmit_can_frame(cy->can_sock_fd, cy->can_id, cy->id_type,
                       cy->dlc, cy->data);
}

/* -------------------------------------------------------------------------
 * app_iface_attached / app_iface_detach
 * ---------------------------------------------------------------------- */

void app_iface_attached(const char *iface_name, int can_fd, int app_listen_fd)
{
    /* Find a free iface slot */
    app_iface_t *slot = NULL;
    for (int i = 0; i < MAX_APP_IFACES; i++) {
        if (g_ifaces[i].can_fd < 0 && g_ifaces[i].app_listen_fd < 0) {
            slot = &g_ifaces[i];
            break;
        }
    }
    if (!slot) {
        fprintf(stderr, "app: no free iface slots\n");
        return;
    }

    strncpy(slot->iface_name, iface_name, IFNAMSIZ - 1);
    slot->iface_name[IFNAMSIZ - 1] = '\0';
    slot->can_fd        = can_fd;
    slot->app_listen_fd = app_listen_fd;

    server_add_fd(g_server, can_fd,        EPOLLIN);
    server_add_fd(g_server, app_listen_fd, EPOLLIN);

    printf("ash-server: app plane active on %s\n", iface_name);
}

void app_iface_detach(const char *iface_name, int can_fd, int app_listen_fd)
{
    /* Remove from epoll */
    if (can_fd >= 0)
        server_del_fd(g_server, can_fd);
    if (app_listen_fd >= 0)
        server_del_fd(g_server, app_listen_fd);

    /* Disconnect all app clients on this interface */
    for (int i = 0; i < MAX_APP_CLIENTS; i++) {
        if (g_clients[i].fd >= 0 &&
            strcmp(g_clients[i].iface_name, iface_name) == 0) {
            server_del_fd(g_server, g_clients[i].fd);
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
            g_clients[i].iface_name[0] = '\0';
        }
    }

    /* Stop and remove all cyclic frame entries for this interface */
    for (int i = 0; i < MAX_CYCLIC_FRAMES; i++) {
        if (g_cyclic[i].can_sock_fd >= 0 &&
            strcmp(g_cyclic[i].iface_name, iface_name) == 0) {
            if (g_cyclic[i].timer_fd >= 0) {
                /* Disarm */
                struct itimerspec its;
                memset(&its, 0, sizeof(its));
                timerfd_settime(g_cyclic[i].timer_fd, 0, &its, NULL);
                server_del_fd(g_server, g_cyclic[i].timer_fd);
                close(g_cyclic[i].timer_fd);
                g_cyclic[i].timer_fd = -1;
            }
            g_cyclic[i].can_sock_fd = -1;
            g_cyclic[i].iface_name[0] = '\0';
        }
    }

    /* Remove from g_ifaces table */
    for (int i = 0; i < MAX_APP_IFACES; i++) {
        if (g_ifaces[i].can_fd == can_fd ||
            (iface_name && strcmp(g_ifaces[i].iface_name, iface_name) == 0)) {
            g_ifaces[i].can_fd        = -1;
            g_ifaces[i].app_listen_fd = -1;
            g_ifaces[i].iface_name[0] = '\0';
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * app_handle_event  — called from server_run() event loop
 * ---------------------------------------------------------------------- */

int app_handle_event(int fd, uint32_t events)
{
    /* App-plane listen fd? */
    if (find_iface_by_listen_fd(fd)) {
        if (events & EPOLLIN)
            app_accept_client(fd);
        return 1;
    }

    /* CAN socket fd? */
    if (find_iface_by_can_fd(fd)) {
        if (events & EPOLLIN)
            app_handle_can_frame(fd);
        return 1;
    }

    /* Cyclic timerfd? */
    if (find_cyclic_by_timer_fd(fd)) {
        if (events & EPOLLIN)
            app_handle_cyclic_timer(fd);
        return 1;
    }

    /* App client fd? */
    if (find_client_by_fd(fd)) {
        if (events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
            app_client_disconnect(fd);
        } else if (events & EPOLLIN) {
            if (app_handle_client(fd) < 0)
                app_client_disconnect(fd);
        }
        return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * app_init / app_destroy
 * ---------------------------------------------------------------------- */

void app_init(server_t *s)
{
    memset(g_ifaces,  0, sizeof(g_ifaces));
    memset(g_clients, 0, sizeof(g_clients));
    memset(g_cyclic,  0, sizeof(g_cyclic));

    for (int i = 0; i < MAX_APP_IFACES; i++) {
        g_ifaces[i].can_fd        = -1;
        g_ifaces[i].app_listen_fd = -1;
    }
    for (int i = 0; i < MAX_APP_CLIENTS; i++)
        g_clients[i].fd = -1;
    for (int i = 0; i < MAX_CYCLIC_FRAMES; i++) {
        g_cyclic[i].timer_fd    = -1;
        g_cyclic[i].can_sock_fd = -1;
    }

    g_server       = s;
}

void app_destroy(void)
{
    for (int i = 0; i < MAX_APP_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) {
            server_del_fd(g_server, g_clients[i].fd);
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
        }
    }
    for (int i = 0; i < MAX_CYCLIC_FRAMES; i++) {
        if (g_cyclic[i].timer_fd >= 0) {
            server_del_fd(g_server, g_cyclic[i].timer_fd);
            close(g_cyclic[i].timer_fd);
            g_cyclic[i].timer_fd = -1;
        }
    }
    g_server       = NULL;
}
