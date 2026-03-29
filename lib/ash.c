/* =========================================================================
 * lib/ash.c — Reference C client library  (SPEC §12.1)
 * ========================================================================= */

#include "ash/ash.h"
#include "ash/proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal constants
 * ---------------------------------------------------------------------- */

#define ASH_MAX_IFACES   16
#define ASH_IFNAMSIZ     16
#define APP_HDR_SIZE      4u
#define APP_MAX_PAYLOAD  4096u

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

typedef struct {
    char name[ASH_IFNAMSIZ];
    int  app_fd;
} ash_iface_conn_t;

struct ash_ctx {
    int              cfg_fd;
    uint32_t         session_id;
    char             host[256];
    ash_iface_conn_t ifaces[ASH_MAX_IFACES];
    int              iface_count;
};

/* -------------------------------------------------------------------------
 * Exact-read helper
 * ---------------------------------------------------------------------- */

static int read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p    = buf;
    size_t   left = len;

    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p    += n;
        left -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Exact-write helper
 * ---------------------------------------------------------------------- */

static int write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p    = buf;
    size_t         left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p    += n;
        left -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * IEEE 754 double encoding helpers (big-endian)
 * ---------------------------------------------------------------------- */

static void encode_be_double(uint8_t *p, double d)
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

static double decode_be_double(const uint8_t *p)
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
 * Config-plane send
 * ---------------------------------------------------------------------- */

static int cfg_send(int fd, uint16_t msg_type,
                    const void *payload, uint32_t plen)
{
    /*
     * Combine header and payload into a single write so they land in one
     * TCP segment.  The server uses non-blocking accepted sockets and its
     * proto_read_exact does not handle EAGAIN, so splitting into two writes
     * risks a race where the header arrives but the payload has not yet
     * when the server's EPOLLIN fires.
     */
    uint8_t  buf[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD];

    buf[0] = (uint8_t)((PROTO_VERSION >> 8) & 0xFF);
    buf[1] = (uint8_t)( PROTO_VERSION       & 0xFF);
    buf[2] = (uint8_t)((msg_type >> 8) & 0xFF);
    buf[3] = (uint8_t)( msg_type       & 0xFF);
    buf[4] = (uint8_t)((plen >> 24) & 0xFF);
    buf[5] = (uint8_t)((plen >> 16) & 0xFF);
    buf[6] = (uint8_t)((plen >>  8) & 0xFF);
    buf[7] = (uint8_t)( plen        & 0xFF);

    if (plen > 0 && payload)
        memcpy(buf + PROTO_HEADER_SIZE, payload, plen);

    return write_exact(fd, buf, PROTO_HEADER_SIZE + plen);
}

/* -------------------------------------------------------------------------
 * Config-plane receive
 *
 * Reads one config-plane frame.  Allocates *payload_out with malloc (caller
 * must free).  Returns 0 on success, -1 on I/O error.
 * ---------------------------------------------------------------------- */

static int cfg_recv(int fd, uint16_t *msg_type_out,
                    uint8_t **payload_out, uint32_t *plen_out)
{
    uint8_t raw[PROTO_HEADER_SIZE];

    if (read_exact(fd, raw, PROTO_HEADER_SIZE) < 0)
        return -1;

    uint16_t msg_type  = (uint16_t)((raw[2] << 8) | raw[3]);
    uint32_t plen      = ((uint32_t)raw[4] << 24) | ((uint32_t)raw[5] << 16)
                       | ((uint32_t)raw[6] <<  8) |  (uint32_t)raw[7];

    uint8_t *payload = NULL;
    if (plen > 0) {
        payload = malloc(plen);
        if (!payload)
            return -1;
        if (read_exact(fd, payload, plen) < 0) {
            free(payload);
            return -1;
        }
    }

    *msg_type_out = msg_type;
    *payload_out  = payload;
    *plen_out     = plen;
    return 0;
}

/* -------------------------------------------------------------------------
 * cfg_request: send + receive one config-plane exchange.
 *
 * On a protocol ERR response the error code is extracted and returned as a
 * negative errno.  On success, caller receives the ACK msg_type and payload
 * (must free *resp_payload_out).
 * ---------------------------------------------------------------------- */

static int cfg_request(ash_ctx_t *ctx,
                       uint16_t msg_type,
                       const void *payload, uint32_t plen,
                       uint16_t *resp_type_out,
                       uint8_t **resp_payload_out, uint32_t *resp_plen_out)
{
    if (cfg_send(ctx->cfg_fd, msg_type, payload, plen) < 0)
        return -EIO;

    uint16_t rtype;
    uint8_t *rpayload = NULL;
    uint32_t rplen    = 0;

    if (cfg_recv(ctx->cfg_fd, &rtype, &rpayload, &rplen) < 0)
        return -EIO;

    if (rtype == MSG_ERR) {
        /* Extract 2-byte error code if present */
        uint16_t code = (rpayload && rplen >= 2u)
                        ? (uint16_t)((rpayload[0] << 8) | rpayload[1])
                        : 0;
        free(rpayload);
        switch (code) {
        case ERR_IFACE_NOT_FOUND:      return -ENODEV;
        case ERR_IFACE_ALREADY_ATTACHED: return -EBUSY;
        case ERR_IFACE_ATTACH_FAILED:  return -EIO;
        case ERR_PERMISSION_DENIED:    return -EACCES;
        case ERR_DEF_INVALID:          return -EINVAL;
        case ERR_DEF_CONFLICT:         return -EEXIST;
        case ERR_DEF_IN_USE:           return -EBUSY;
        case ERR_OWN_NOT_AVAILABLE:    return -EBUSY;
        case ERR_OWN_NOT_HELD:         return -EPERM;
        case ERR_CFG_IO:               return -EIO;
        case ERR_CFG_CHECKSUM:         return -EILSEQ;
        case ERR_CFG_CONFLICT:         return -EEXIST;
        default:                       return -EPROTO;
        }
    }

    if (resp_type_out)    *resp_type_out    = rtype;
    if (resp_payload_out) *resp_payload_out = rpayload;
    if (resp_plen_out)    *resp_plen_out    = rplen;

    if (!resp_payload_out)
        free(rpayload);

    return 0;
}

/* -------------------------------------------------------------------------
 * App-plane send
 * ---------------------------------------------------------------------- */

static int app_send(int fd, uint16_t msg_type,
                    const void *payload, uint16_t plen)
{
    /* Same single-write strategy as cfg_send — server app sockets are
     * also non-blocking and app_handle_client does not handle EAGAIN. */
    uint8_t buf[APP_HDR_SIZE + APP_MAX_PAYLOAD];
    buf[0] = (uint8_t)((msg_type >> 8) & 0xFF);
    buf[1] = (uint8_t)( msg_type       & 0xFF);
    buf[2] = (uint8_t)((plen >> 8) & 0xFF);
    buf[3] = (uint8_t)( plen       & 0xFF);

    if (plen > 0 && payload)
        memcpy(buf + APP_HDR_SIZE, payload, plen);

    return write_exact(fd, buf, APP_HDR_SIZE + plen);
}

/* -------------------------------------------------------------------------
 * App-plane receive a single frame.  Caller must free *payload_out.
 * ---------------------------------------------------------------------- */

static int app_recv(int fd, uint16_t *msg_type_out,
                    uint8_t **payload_out, uint16_t *plen_out)
{
    uint8_t hdr[APP_HDR_SIZE];
    if (read_exact(fd, hdr, APP_HDR_SIZE) < 0)
        return -1;

    uint16_t msg_type = (uint16_t)((hdr[0] << 8) | hdr[1]);
    uint16_t plen     = (uint16_t)((hdr[2] << 8) | hdr[3]);

    if (plen > APP_MAX_PAYLOAD)
        return -1;

    uint8_t *payload = NULL;
    if (plen > 0) {
        payload = malloc(plen);
        if (!payload)
            return -1;
        if (read_exact(fd, payload, plen) < 0) {
            free(payload);
            return -1;
        }
    }

    *msg_type_out = msg_type;
    *payload_out  = payload;
    *plen_out     = plen;
    return 0;
}

/* -------------------------------------------------------------------------
 * TCP connect helper
 * ---------------------------------------------------------------------- */

static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *res, *rp;
    char portstr[8];
    int  fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* -------------------------------------------------------------------------
 * Internal iface lookup helpers
 * ---------------------------------------------------------------------- */

static ash_iface_conn_t *find_iface_by_name(ash_ctx_t *ctx, const char *name)
{
    for (int i = 0; i < ctx->iface_count; i++) {
        if (strcmp(ctx->ifaces[i].name, name) == 0)
            return &ctx->ifaces[i];
    }
    return NULL;
}

/* Returns the first attached iface's app_fd, or -1 if none. */
static int first_app_fd(ash_ctx_t *ctx)
{
    for (int i = 0; i < ctx->iface_count; i++) {
        if (ctx->ifaces[i].app_fd >= 0)
            return ctx->ifaces[i].app_fd;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Connection
 * ---------------------------------------------------------------------- */

ash_ctx_t *ash_connect(const char *host, uint16_t port, const char *client_name)
{
    if (!host || !client_name)
        return NULL;

    size_t name_len = strlen(client_name);
    if (name_len > PROTO_MAX_NAME)
        return NULL;

    int fd = tcp_connect(host, port);
    if (fd < 0)
        return NULL;

    /* Build SESSION_INIT payload: name_len(1) + name(N) */
    uint8_t payload[1 + PROTO_MAX_NAME];
    payload[0] = (uint8_t)name_len;
    if (name_len > 0)
        memcpy(payload + 1, client_name, name_len);

    if (cfg_send(fd, MSG_SESSION_INIT, payload, (uint32_t)(1u + name_len)) < 0) {
        close(fd);
        return NULL;
    }

    uint16_t rtype;
    uint8_t *rpayload = NULL;
    uint32_t rplen    = 0;

    if (cfg_recv(fd, &rtype, &rpayload, &rplen) < 0 ||
        rtype != MSG_SESSION_INIT_ACK || rplen < 4u) {
        free(rpayload);
        close(fd);
        return NULL;
    }

    uint32_t session_id = ((uint32_t)rpayload[0] << 24) |
                          ((uint32_t)rpayload[1] << 16) |
                          ((uint32_t)rpayload[2] <<  8) |
                           (uint32_t)rpayload[3];
    free(rpayload);

    ash_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        close(fd);
        return NULL;
    }

    ctx->cfg_fd     = fd;
    ctx->session_id = session_id;
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->host[sizeof(ctx->host) - 1] = '\0';

    for (int i = 0; i < ASH_MAX_IFACES; i++)
        ctx->ifaces[i].app_fd = -1;

    return ctx;
}

void ash_disconnect(ash_ctx_t *ctx)
{
    if (!ctx)
        return;

    /* Send SESSION_CLOSE (no payload) */
    cfg_send(ctx->cfg_fd, MSG_SESSION_CLOSE, NULL, 0);

    /* Read SESSION_CLOSE_ACK (best-effort) */
    uint16_t rtype;
    uint8_t *rpayload = NULL;
    uint32_t rplen    = 0;
    cfg_recv(ctx->cfg_fd, &rtype, &rpayload, &rplen);
    free(rpayload);

    /* Close all app-plane connections */
    for (int i = 0; i < ctx->iface_count; i++) {
        if (ctx->ifaces[i].app_fd >= 0) {
            close(ctx->ifaces[i].app_fd);
            ctx->ifaces[i].app_fd = -1;
        }
    }

    close(ctx->cfg_fd);
    free(ctx);
}

int ash_keepalive(ash_ctx_t *ctx)
{
    if (!ctx)
        return -EINVAL;
    /* SESSION_KEEPALIVE: no payload, no ACK */
    if (cfg_send(ctx->cfg_fd, MSG_SESSION_KEEPALIVE, NULL, 0) < 0)
        return -EIO;
    return 0;
}

/* -------------------------------------------------------------------------
 * Interface management
 * ---------------------------------------------------------------------- */

int ash_iface_list(ash_ctx_t *ctx, ash_iface_info_t *out, size_t max_count)
{
    if (!ctx || !out)
        return -EINVAL;

    uint16_t rtype;
    uint8_t *rpayload = NULL;
    uint32_t rplen    = 0;

    int rc = cfg_request(ctx, MSG_IFACE_LIST, NULL, 0,
                         &rtype, &rpayload, &rplen);
    if (rc < 0)
        return rc;

    if (rtype != MSG_IFACE_LIST_RESP || rplen < 1u) {
        free(rpayload);
        return -EPROTO;
    }

    uint8_t count = rpayload[0];
    const uint8_t *p = rpayload + 1;
    uint32_t remaining = rplen - 1u;
    int filled = 0;

    for (int i = 0; i < (int)count && remaining >= 2u; i++) {
        uint8_t nlen = *p++;
        remaining--;

        if (nlen == 0 || nlen > 63u || remaining < (uint32_t)(nlen + 1u))
            break;

        if ((size_t)filled < max_count) {
            memcpy(out[filled].name, p, nlen);
            out[filled].name[nlen] = '\0';
            out[filled].state      = p[nlen];
            filled++;
        }

        p         += nlen + 1u;
        remaining -= (uint32_t)(nlen + 1u);
    }

    free(rpayload);
    return filled;
}

int ash_iface_attach(ash_ctx_t *ctx, const char *iface,
                     uint8_t mode, uint32_t bitrate)
{
    if (!ctx || !iface)
        return -EINVAL;

    size_t nlen = strlen(iface);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    if (ctx->iface_count >= ASH_MAX_IFACES)
        return -ENOMEM;

    /* Payload: name_len(1) + name(N) + mode(1) + bitrate(4) + filter_count(1) */
    uint8_t buf[1 + PROTO_MAX_NAME + 1 + 4 + 1];
    size_t  off = 0;

    buf[off++] = (uint8_t)nlen;
    memcpy(buf + off, iface, nlen); off += nlen;
    buf[off++] = mode;
    buf[off++] = (uint8_t)((bitrate >> 24) & 0xFF);
    buf[off++] = (uint8_t)((bitrate >> 16) & 0xFF);
    buf[off++] = (uint8_t)((bitrate >>  8) & 0xFF);
    buf[off++] = (uint8_t)( bitrate        & 0xFF);
    buf[off++] = 0;  /* filter_count = 0 (promiscuous) */

    uint16_t rtype;
    uint8_t *rpayload = NULL;
    uint32_t rplen    = 0;

    int rc = cfg_request(ctx, MSG_IFACE_ATTACH, buf, (uint32_t)off,
                         &rtype, &rpayload, &rplen);
    if (rc < 0)
        return rc;

    if (rtype != MSG_IFACE_ATTACH_ACK || rplen < 2u) {
        free(rpayload);
        return -EPROTO;
    }

    uint16_t app_port = (uint16_t)((rpayload[0] << 8) | rpayload[1]);
    free(rpayload);

    /* Connect to the application plane port */
    int app_fd = tcp_connect(ctx->host, app_port);
    if (app_fd < 0)
        return -EIO;

    /* Register in context */
    ash_iface_conn_t *slot = &ctx->ifaces[ctx->iface_count++];
    strncpy(slot->name, iface, ASH_IFNAMSIZ - 1);
    slot->name[ASH_IFNAMSIZ - 1] = '\0';
    slot->app_fd = app_fd;

    return 0;
}

int ash_iface_detach(ash_ctx_t *ctx, const char *iface)
{
    if (!ctx || !iface)
        return -EINVAL;

    size_t nlen = strlen(iface);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    uint8_t buf[1 + PROTO_MAX_NAME];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, iface, nlen);

    int rc = cfg_request(ctx, MSG_IFACE_DETACH, buf, (uint32_t)(1u + nlen),
                         NULL, NULL, NULL);
    if (rc < 0)
        return rc;

    /* Close and remove the app-plane connection */
    ash_iface_conn_t *conn = find_iface_by_name(ctx, iface);
    if (conn) {
        if (conn->app_fd >= 0) {
            close(conn->app_fd);
            conn->app_fd = -1;
        }
        /* Compact the ifaces array */
        int idx = (int)(conn - ctx->ifaces);
        for (int i = idx; i < ctx->iface_count - 1; i++)
            ctx->ifaces[i] = ctx->ifaces[i + 1];
        ctx->iface_count--;
    }

    return 0;
}

static int vcan_op(ash_ctx_t *ctx, const char *name, uint16_t msg_type)
{
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    uint8_t buf[1 + PROTO_MAX_NAME];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, name, nlen);

    return cfg_request(ctx, msg_type, buf, (uint32_t)(1u + nlen),
                       NULL, NULL, NULL);
}

int ash_vcan_create(ash_ctx_t *ctx, const char *name)
{
    if (!ctx || !name)
        return -EINVAL;
    return vcan_op(ctx, name, MSG_IFACE_VCAN_CREATE);
}

int ash_vcan_destroy(ash_ctx_t *ctx, const char *name)
{
    if (!ctx || !name)
        return -EINVAL;
    return vcan_op(ctx, name, MSG_IFACE_VCAN_DESTROY);
}

/* -------------------------------------------------------------------------
 * Definitions
 * ---------------------------------------------------------------------- */

int ash_define_signal(ash_ctx_t *ctx, const ash_signal_def_t *def)
{
    if (!ctx || !def)
        return -EINVAL;

    size_t nlen = strnlen(def->name, sizeof(def->name));
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /*
     * Payload: name_len(1) + name(N) + data_type(1) + byte_order(1)
     *        + bit_length(1) + scale(8) + offset(8) + min(8) + max(8)
     */
    uint8_t buf[1 + PROTO_MAX_NAME + 3 + 32];
    size_t  off = 0;

    buf[off++] = (uint8_t)nlen;
    memcpy(buf + off, def->name, nlen); off += nlen;
    buf[off++] = def->data_type;
    buf[off++] = def->byte_order;
    buf[off++] = def->bit_length;
    encode_be_double(buf + off, def->scale);  off += 8;
    encode_be_double(buf + off, def->offset); off += 8;
    encode_be_double(buf + off, def->min);    off += 8;
    encode_be_double(buf + off, def->max);    off += 8;

    return cfg_request(ctx, MSG_DEF_SIGNAL, buf, (uint32_t)off,
                       NULL, NULL, NULL);
}

int ash_define_pdu(ash_ctx_t *ctx, const ash_pdu_def_t *def)
{
    if (!ctx || !def)
        return -EINVAL;

    size_t nlen = strnlen(def->name, sizeof(def->name));
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /*
     * Payload: name_len(1) + name(N) + pdu_length(1) + signal_count(1)
     *        + [sig_name_len(1) + sig_name(N) + start_bit(1)] * signal_count
     */
    /* Max buffer: header(1+64+1+1) + 32 * (1+64+1) = 67 + 32*66 = 2179 */
    uint8_t buf[2200];
    size_t  off = 0;

    buf[off++] = (uint8_t)nlen;
    memcpy(buf + off, def->name, nlen); off += nlen;
    buf[off++] = def->length;
    buf[off++] = def->signal_count;

    for (int i = 0; i < (int)def->signal_count; i++) {
        size_t snlen = strnlen(def->signals[i].signal_name,
                               sizeof(def->signals[i].signal_name));
        if (snlen == 0 || snlen > PROTO_MAX_NAME)
            return -EINVAL;
        buf[off++] = (uint8_t)snlen;
        memcpy(buf + off, def->signals[i].signal_name, snlen); off += snlen;
        buf[off++] = def->signals[i].start_bit;
    }

    return cfg_request(ctx, MSG_DEF_PDU, buf, (uint32_t)off,
                       NULL, NULL, NULL);
}

int ash_define_frame(ash_ctx_t *ctx, const ash_frame_def_t *def)
{
    if (!ctx || !def)
        return -EINVAL;

    size_t nlen = strnlen(def->name, sizeof(def->name));
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /*
     * Payload: name_len(1) + name(N) + can_id(4) + id_type(1) + dlc(1)
     *        + tx_period(2) + pdu_count(1)
     *        + [pdu_name_len(1) + pdu_name(N) + byte_offset(1)] * pdu_count
     */
    uint8_t buf[1 + PROTO_MAX_NAME + 4 + 1 + 1 + 2 + 1 + 8 * (1 + PROTO_MAX_NAME + 1)];
    size_t  off = 0;

    buf[off++] = (uint8_t)nlen;
    memcpy(buf + off, def->name, nlen); off += nlen;

    uint32_t can_id = def->can_id;
    buf[off++] = (uint8_t)((can_id >> 24) & 0xFF);
    buf[off++] = (uint8_t)((can_id >> 16) & 0xFF);
    buf[off++] = (uint8_t)((can_id >>  8) & 0xFF);
    buf[off++] = (uint8_t)( can_id        & 0xFF);

    buf[off++] = def->id_type;
    buf[off++] = def->dlc;

    uint16_t period = def->tx_period_ms;
    buf[off++] = (uint8_t)((period >> 8) & 0xFF);
    buf[off++] = (uint8_t)( period       & 0xFF);

    buf[off++] = def->pdu_count;

    for (int i = 0; i < (int)def->pdu_count; i++) {
        size_t pnlen = strnlen(def->pdus[i].pdu_name,
                               sizeof(def->pdus[i].pdu_name));
        if (pnlen == 0 || pnlen > PROTO_MAX_NAME)
            return -EINVAL;
        buf[off++] = (uint8_t)pnlen;
        memcpy(buf + off, def->pdus[i].pdu_name, pnlen); off += pnlen;
        buf[off++] = def->pdus[i].byte_offset;
    }

    return cfg_request(ctx, MSG_DEF_FRAME, buf, (uint32_t)off,
                       NULL, NULL, NULL);
}

int ash_delete_def(ash_ctx_t *ctx, const char *name, uint8_t def_type)
{
    if (!ctx || !name)
        return -EINVAL;

    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /* Payload: name_len(1) + name(N) + def_type(1) */
    uint8_t buf[1 + PROTO_MAX_NAME + 1];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, name, nlen);
    buf[1 + nlen] = def_type;

    return cfg_request(ctx, MSG_DEF_DELETE, buf, (uint32_t)(1u + nlen + 1u),
                       NULL, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Ownership
 * ---------------------------------------------------------------------- */

int ash_acquire(ash_ctx_t *ctx, const char *signal, uint8_t on_disconnect)
{
    if (!ctx || !signal)
        return -EINVAL;

    size_t nlen = strlen(signal);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /* Payload: name_len(1) + name(N) + on_disconnect(1) */
    uint8_t buf[1 + PROTO_MAX_NAME + 1];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, signal, nlen);
    buf[1 + nlen] = on_disconnect;

    return cfg_request(ctx, MSG_OWN_ACQUIRE, buf, (uint32_t)(1u + nlen + 1u),
                       NULL, NULL, NULL);
}

static int own_name_op(ash_ctx_t *ctx, const char *signal, uint16_t msg_type)
{
    size_t nlen = strlen(signal);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    uint8_t buf[1 + PROTO_MAX_NAME];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, signal, nlen);

    return cfg_request(ctx, msg_type, buf, (uint32_t)(1u + nlen),
                       NULL, NULL, NULL);
}

int ash_release(ash_ctx_t *ctx, const char *signal)
{
    if (!ctx || !signal)
        return -EINVAL;
    return own_name_op(ctx, signal, MSG_OWN_RELEASE);
}

int ash_lock(ash_ctx_t *ctx, const char *signal)
{
    if (!ctx || !signal)
        return -EINVAL;
    return own_name_op(ctx, signal, MSG_OWN_LOCK);
}

int ash_unlock(ash_ctx_t *ctx, const char *signal)
{
    if (!ctx || !signal)
        return -EINVAL;
    return own_name_op(ctx, signal, MSG_OWN_UNLOCK);
}

/* -------------------------------------------------------------------------
 * Runtime
 * ---------------------------------------------------------------------- */

int ash_write(ash_ctx_t *ctx, const char *signal, double value)
{
    if (!ctx || !signal)
        return -EINVAL;

    int app_fd = first_app_fd(ctx);
    if (app_fd < 0)
        return -ENODEV;

    size_t nlen = strlen(signal);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /* Payload: name_len(1) + name(N) + value(8 BE double) */
    uint8_t buf[1 + PROTO_MAX_NAME + 8];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, signal, nlen);
    encode_be_double(buf + 1 + nlen, value);

    if (app_send(app_fd, 0x0001u /* APP_MSG_SIG_WRITE */,
                 buf, (uint16_t)(1u + nlen + 8u)) < 0)
        return -EIO;

    return 0;
}

int ash_read(ash_ctx_t *ctx, const char *signal, double *value_out)
{
    if (!ctx || !signal || !value_out)
        return -EINVAL;

    int app_fd = first_app_fd(ctx);
    if (app_fd < 0)
        return -ENODEV;

    size_t nlen = strlen(signal);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    /* Send SIG_READ: name_len(1) + name(N) */
    uint8_t req[1 + PROTO_MAX_NAME];
    req[0] = (uint8_t)nlen;
    memcpy(req + 1, signal, nlen);

    if (app_send(app_fd, 0x0002u /* APP_MSG_SIG_READ */,
                 req, (uint16_t)(1u + nlen)) < 0)
        return -EIO;

    /* Drain frames until we get SIG_READ_RESP or APP_ERR */
    for (;;) {
        uint16_t rtype;
        uint8_t *payload = NULL;
        uint16_t plen    = 0;

        if (app_recv(app_fd, &rtype, &payload, &plen) < 0)
            return -EIO;

        if (rtype == 0x8002u /* APP_MSG_SIG_READ_RESP */) {
            /* name_len(1) + name(N) + value(8) */
            if (plen >= (uint16_t)(1u + nlen + 8u)) {
                *value_out = decode_be_double(payload + 1 + nlen);
                free(payload);
                return 0;
            }
            free(payload);
            return -EPROTO;
        }

        if (rtype == 0xFFFFu /* APP_MSG_APP_ERR */) {
            uint16_t code = (payload && plen >= 2u)
                            ? (uint16_t)((payload[0] << 8) | payload[1])
                            : 0;
            free(payload);
            switch (code) {
            case 0x0001: return -ENOENT;  /* ERR_SIG_NOT_FOUND */
            default:     return -EPROTO;
            }
        }

        /* Other message (SIG_RX, FRAME_RX): discard and loop */
        free(payload);
    }
}

int ash_frame_tx(ash_ctx_t *ctx, const char *iface,
                 uint32_t can_id, uint8_t dlc, uint8_t flags,
                 const uint8_t *data)
{
    if (!ctx || !iface)
        return -EINVAL;

    ash_iface_conn_t *conn = find_iface_by_name(ctx, iface);
    if (!conn || conn->app_fd < 0)
        return -ENODEV;

    /* Determine data length from DLC */
    static const uint8_t fd_map[16] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    int      is_fd    = (flags & 0x01u) ? 1 : 0;
    uint8_t  data_len = (is_fd && dlc > 8) ? fd_map[dlc < 16 ? dlc : 15] : dlc;

    /* Payload: can_id(4) + dlc(1) + flags(1) + data(N) */
    uint8_t buf[6 + 64];
    buf[0] = (uint8_t)((can_id >> 24) & 0xFF);
    buf[1] = (uint8_t)((can_id >> 16) & 0xFF);
    buf[2] = (uint8_t)((can_id >>  8) & 0xFF);
    buf[3] = (uint8_t)( can_id        & 0xFF);
    buf[4] = dlc;
    buf[5] = flags;

    if (data_len > 0 && data)
        memcpy(buf + 6, data, data_len);

    if (app_send(conn->app_fd, 0x0010u /* APP_MSG_FRAME_TX */,
                 buf, (uint16_t)(6u + data_len)) < 0)
        return -EIO;

    return 0;
}

int ash_poll(ash_ctx_t *ctx, ash_event_t *event, int timeout_ms)
{
    if (!ctx || !event)
        return -EINVAL;

    /* Build poll set: cfg_fd + all app fds */
    struct pollfd fds[1 + ASH_MAX_IFACES];
    int nfds = 0;

    fds[nfds].fd      = ctx->cfg_fd;
    fds[nfds].events  = POLLIN;
    fds[nfds].revents = 0;
    nfds++;

    for (int i = 0; i < ctx->iface_count; i++) {
        if (ctx->ifaces[i].app_fd >= 0) {
            fds[nfds].fd      = ctx->ifaces[i].app_fd;
            fds[nfds].events  = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
    }

    int ret = poll(fds, (nfds_t)nfds, timeout_ms);
    if (ret < 0)
        return -errno;
    if (ret == 0)
        return 0;  /* timeout */

    /* Check config fd first (for notifications) */
    if (fds[0].revents & POLLIN) {
        uint16_t rtype;
        uint8_t *payload = NULL;
        uint32_t plen    = 0;

        if (cfg_recv(ctx->cfg_fd, &rtype, &payload, &plen) < 0)
            return -EIO;

        memset(event, 0, sizeof(*event));

        if (rtype == MSG_NOTIFY_OWN_REVOKED) {
            event->type = ASH_EVENT_NOTIFY_OWN_REVOKED;
            if (payload && plen >= 2u) {
                uint8_t nlen = payload[0];
                if (nlen > 0 && nlen <= 63u && plen >= (uint32_t)(1u + nlen)) {
                    memcpy(event->u.own_revoked.signal, payload + 1, nlen);
                    event->u.own_revoked.signal[nlen] = '\0';
                }
            }
            free(payload);
            return 1;
        }

        if (rtype == MSG_NOTIFY_IFACE_DOWN) {
            event->type = ASH_EVENT_NOTIFY_IFACE_DOWN;
            if (payload && plen >= 2u) {
                uint8_t nlen = payload[0];
                if (nlen > 0 && nlen <= 15u && plen >= (uint32_t)(1u + nlen)) {
                    memcpy(event->u.iface_down.iface, payload + 1, nlen);
                    event->u.iface_down.iface[nlen] = '\0';
                }
            }
            free(payload);
            return 1;
        }

        if (rtype == MSG_NOTIFY_SERVER_CLOSE) {
            event->type = ASH_EVENT_NOTIFY_SERVER_CLOSE;
            free(payload);
            return 1;
        }

        /* Unexpected config-plane message: discard */
        free(payload);
    }

    /* Check app fds */
    for (int i = 1; i < nfds; i++) {
        if (!(fds[i].revents & POLLIN))
            continue;

        int app_fd = fds[i].fd;

        /* Find which iface this fd belongs to */
        const char *iface_name = NULL;
        for (int j = 0; j < ctx->iface_count; j++) {
            if (ctx->ifaces[j].app_fd == app_fd) {
                iface_name = ctx->ifaces[j].name;
                break;
            }
        }

        uint16_t rtype;
        uint8_t *payload = NULL;
        uint16_t plen    = 0;

        if (app_recv(app_fd, &rtype, &payload, &plen) < 0)
            return -EIO;

        memset(event, 0, sizeof(*event));
        if (iface_name)
            strncpy(event->iface, iface_name, sizeof(event->iface) - 1);

        if (rtype == 0x8003u /* APP_MSG_SIG_RX */) {
            /* name_len(1) + name(N) + value(8) */
            event->type = ASH_EVENT_SIG_RX;
            if (payload && plen >= 10u) {
                uint8_t nlen = payload[0];
                if (nlen > 0 && nlen <= 63u &&
                    plen >= (uint16_t)(1u + nlen + 8u)) {
                    memcpy(event->u.sig_rx.signal, payload + 1, nlen);
                    event->u.sig_rx.signal[nlen] = '\0';
                    event->u.sig_rx.value = decode_be_double(payload + 1 + nlen);
                }
            }
            free(payload);
            return 1;
        }

        if (rtype == 0x8010u /* APP_MSG_FRAME_RX */) {
            /* can_id(4) + dlc(1) + flags(1) + data(N) */
            event->type = ASH_EVENT_FRAME_RX;
            if (payload && plen >= 6u) {
                event->u.frame_rx.can_id =
                    ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                    ((uint32_t)payload[2] <<  8) |  (uint32_t)payload[3];
                event->u.frame_rx.dlc   = payload[4];
                event->u.frame_rx.flags = payload[5];
                uint8_t dlen = (uint8_t)(plen - 6u);
                if (dlen > 64u) dlen = 64u;
                if (dlen > 0)
                    memcpy(event->u.frame_rx.data, payload + 6, dlen);
            }
            free(payload);
            return 1;
        }

        if (rtype == 0xFFFFu /* APP_MSG_APP_ERR */) {
            event->type = ASH_EVENT_APP_ERR;
            if (payload && plen >= 2u)
                event->u.app_err.code =
                    (uint16_t)((payload[0] << 8) | payload[1]);
            free(payload);
            return 1;
        }

        /* Unknown app message: discard */
        free(payload);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Persistence
 * ---------------------------------------------------------------------- */

static int cfg_name_op(ash_ctx_t *ctx, const char *name, uint16_t msg_type)
{
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > PROTO_MAX_NAME)
        return -EINVAL;

    uint8_t buf[1 + PROTO_MAX_NAME];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, name, nlen);

    return cfg_request(ctx, msg_type, buf, (uint32_t)(1u + nlen),
                       NULL, NULL, NULL);
}

int ash_cfg_save(ash_ctx_t *ctx, const char *name)
{
    if (!ctx || !name)
        return -EINVAL;
    return cfg_name_op(ctx, name, MSG_CFG_SAVE);
}

int ash_cfg_load(ash_ctx_t *ctx, const char *name)
{
    if (!ctx || !name)
        return -EINVAL;
    return cfg_name_op(ctx, name, MSG_CFG_LOAD);
}
