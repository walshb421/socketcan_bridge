#include "proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Exact-read helper  (subtask #19)
 * ---------------------------------------------------------------------- */

int proto_read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p   = buf;
    size_t   left = len;

    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;   /* EOF or error */
        }
        p    += n;
        left -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Frame reader  (subtask #19)
 * ---------------------------------------------------------------------- */

int proto_read_frame(int fd, proto_frame_t *out)
{
    uint8_t raw[PROTO_HEADER_SIZE];

    if (proto_read_exact(fd, raw, PROTO_HEADER_SIZE) < 0)
        return -1;

    /* Deserialise big-endian fields */
    out->hdr.version     = (uint16_t)((raw[0] << 8) | raw[1]);
    out->hdr.msg_type    = (uint16_t)((raw[2] << 8) | raw[3]);
    out->hdr.payload_len = ((uint32_t)raw[4] << 24) |
                           ((uint32_t)raw[5] << 16) |
                           ((uint32_t)raw[6] <<  8) |
                            (uint32_t)raw[7];
    out->payload = NULL;

    if (out->hdr.payload_len > 0) {
        out->payload = malloc(out->hdr.payload_len);
        if (!out->payload) {
            perror("proto_read_frame: malloc");
            return -1;
        }
        if (proto_read_exact(fd, out->payload, out->hdr.payload_len) < 0) {
            free(out->payload);
            out->payload = NULL;
            return -1;
        }
    }

    return 0;
}

void proto_frame_free(proto_frame_t *f)
{
    if (f) {
        free(f->payload);
        f->payload = NULL;
    }
}

/* -------------------------------------------------------------------------
 * Header validation  (subtask #20)
 * ---------------------------------------------------------------------- */

int proto_validate_header(const cfg_header_t *hdr, int *close_conn)
{
    *close_conn = 0;

    if (hdr->version != PROTO_VERSION) {
        *close_conn = 1;
        return ERR_PROTOCOL_VERSION;
    }

    if (hdr->payload_len > PROTO_MAX_PAYLOAD) {
        *close_conn = 1;
        return ERR_PAYLOAD_TOO_LARGE;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Response framing  (subtask #21)
 * ---------------------------------------------------------------------- */

int proto_send_ack(int fd, uint16_t msg_type,
                   const void *payload, uint32_t payload_len)
{
    uint8_t hdr[PROTO_HEADER_SIZE];

    hdr[0] = (PROTO_VERSION >> 8) & 0xFF;
    hdr[1] =  PROTO_VERSION       & 0xFF;
    hdr[2] = (msg_type >> 8) & 0xFF;
    hdr[3] =  msg_type       & 0xFF;
    hdr[4] = (payload_len >> 24) & 0xFF;
    hdr[5] = (payload_len >> 16) & 0xFF;
    hdr[6] = (payload_len >>  8) & 0xFF;
    hdr[7] =  payload_len        & 0xFF;

    if (write(fd, hdr, PROTO_HEADER_SIZE) != PROTO_HEADER_SIZE)
        return -1;

    if (payload_len > 0 && payload) {
        ssize_t written = write(fd, payload, payload_len);
        if (written < 0 || (uint32_t)written != payload_len)
            return -1;
    }

    return 0;
}

int proto_send_err(int fd, uint16_t err_code, const char *msg)
{
    size_t   msg_len = msg ? strlen(msg) : 0;
    uint32_t plen    = 2u + (uint32_t)msg_len;
    uint8_t  hdr[PROTO_HEADER_SIZE];
    uint8_t  code[2];

    hdr[0] = (PROTO_VERSION >> 8) & 0xFF;
    hdr[1] =  PROTO_VERSION       & 0xFF;
    hdr[2] = (MSG_ERR >> 8) & 0xFF;
    hdr[3] =  MSG_ERR       & 0xFF;
    hdr[4] = (plen >> 24) & 0xFF;
    hdr[5] = (plen >> 16) & 0xFF;
    hdr[6] = (plen >>  8) & 0xFF;
    hdr[7] =  plen        & 0xFF;

    code[0] = (err_code >> 8) & 0xFF;
    code[1] =  err_code       & 0xFF;

    if (write(fd, hdr,  PROTO_HEADER_SIZE) != PROTO_HEADER_SIZE)
        return -1;
    if (write(fd, code, 2) != 2)
        return -1;
    if (msg_len > 0) {
        ssize_t w = write(fd, msg, msg_len);
        if (w < 0 || (size_t)w != msg_len)
            return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Dispatch table  (subtask #22)
 * ---------------------------------------------------------------------- */

#define HANDLER_TABLE_SIZE 64

typedef struct {
    uint16_t       msg_type;
    proto_handler_t handler;
} handler_entry_t;

static handler_entry_t  handler_table[HANDLER_TABLE_SIZE];
static int              handler_count = 0;

int proto_register_handler(uint16_t msg_type, proto_handler_t handler)
{
    if (handler_count >= HANDLER_TABLE_SIZE)
        return -1;

    for (int i = 0; i < handler_count; i++) {
        if (handler_table[i].msg_type == msg_type)
            return -1;   /* already registered */
    }

    handler_table[handler_count].msg_type = msg_type;
    handler_table[handler_count].handler  = handler;
    handler_count++;
    return 0;
}

int proto_dispatch(int fd, const proto_frame_t *frame)
{
    for (int i = 0; i < handler_count; i++) {
        if (handler_table[i].msg_type == frame->hdr.msg_type)
            return handler_table[i].handler(fd, frame);
    }

    /* No handler found */
    proto_send_err(fd, ERR_UNKNOWN_MESSAGE_TYPE, "unknown message type");
    return 0;
}
