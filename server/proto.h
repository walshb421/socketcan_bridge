#ifndef ASH_SERVER_PROTO_H
#define ASH_SERVER_PROTO_H

#include "ash/proto.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Opaque frame buffer — owns a heap-allocated payload array.
 * ---------------------------------------------------------------------- */

typedef struct {
    cfg_header_t hdr;
    uint8_t     *payload;   /* NULL when hdr.payload_len == 0 */
} proto_frame_t;

/* -------------------------------------------------------------------------
 * Message handler callback type.
 *
 * Return 0 to keep the session open, -1 to close it.
 * ---------------------------------------------------------------------- */

typedef int (*proto_handler_t)(int fd, const proto_frame_t *frame);

/* -------------------------------------------------------------------------
 * Exact-read helper  (subtask #19)
 *
 * Reads exactly `len` bytes from `fd` into `buf`.
 * Returns 0 on success, -1 on error (EOF counts as error here since we
 * expect a complete frame).
 * ---------------------------------------------------------------------- */

int proto_read_exact(int fd, void *buf, size_t len);

/* -------------------------------------------------------------------------
 * Frame reader  (subtask #19)
 *
 * Reads one complete frame from `fd` into *out.  The caller must call
 * proto_frame_free() when done.
 *
 * Returns:
 *   0   – frame read successfully
 *  -1   – I/O error or peer closed
 * ---------------------------------------------------------------------- */

int  proto_read_frame(int fd, proto_frame_t *out);
void proto_frame_free(proto_frame_t *f);

/* -------------------------------------------------------------------------
 * Header validation  (subtask #20)
 *
 * Validates protocol version and payload length.
 *
 * Returns 0 if the header is acceptable.
 * Returns a non-zero wire error code (ERR_*) if not.
 *
 * close_conn is set to 1 when the connection must be closed after sending
 * the error response (per SPEC §3.3).
 * ---------------------------------------------------------------------- */

int proto_validate_header(const cfg_header_t *hdr, int *close_conn);

/* -------------------------------------------------------------------------
 * Response framing  (subtask #21)
 *
 * proto_send_ack – sends a framed response with msg_type and optional
 *                  payload.  payload may be NULL if payload_len is 0.
 *
 * proto_send_err – sends a framed ERR message.
 *                  `msg` may be NULL (zero-length message string).
 *
 * Both return 0 on success, -1 on write failure.
 * ---------------------------------------------------------------------- */

int proto_send_ack(int fd, uint16_t msg_type,
                   const void *payload, uint32_t payload_len);

int proto_send_err(int fd, uint16_t err_code, const char *msg);

/* -------------------------------------------------------------------------
 * Dispatch table  (subtask #22)
 * ---------------------------------------------------------------------- */

/* Register a handler for the given message type.
 * Returns 0 on success, -1 if the table is full or the type is already
 * registered. */
int proto_register_handler(uint16_t msg_type, proto_handler_t handler);

/* Dispatch frame to the registered handler.
 * Returns the handler's return value, or sends ERR_UNKNOWN_MESSAGE_TYPE
 * and returns 0 if no handler is found. */
int proto_dispatch(int fd, const proto_frame_t *frame);

#endif /* ASH_SERVER_PROTO_H */
