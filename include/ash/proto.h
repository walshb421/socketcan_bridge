#ifndef ASH_PROTO_H
#define ASH_PROTO_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Wire constants  (SPEC §3.2, §3.4)
 * ---------------------------------------------------------------------- */

#define PROTO_VERSION       0x0001u
#define PROTO_HEADER_SIZE   8u
#define PROTO_MAX_PAYLOAD   65535u
#define PROTO_MAX_NAME      64u

/* -------------------------------------------------------------------------
 * Fixed 8-byte frame header  (SPEC §3.2)
 *
 *  0               1               2               3
 *  0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |       Protocol Version        |          Message Type         |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                         Payload Length                        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * All fields are big-endian.
 * ---------------------------------------------------------------------- */

typedef struct {
    uint16_t version;
    uint16_t msg_type;
    uint32_t payload_len;
} cfg_header_t;

/* -------------------------------------------------------------------------
 * Message type constants — Client → Server  (SPEC §3.4)
 * ---------------------------------------------------------------------- */

/* Session management */
#define MSG_SESSION_INIT        0x0001u
#define MSG_SESSION_KEEPALIVE   0x0002u
#define MSG_SESSION_CLOSE       0x0003u

/* Interface management */
#define MSG_IFACE_LIST          0x0010u
#define MSG_IFACE_ATTACH        0x0011u
#define MSG_IFACE_DETACH        0x0012u
#define MSG_IFACE_VCAN_CREATE   0x0013u
#define MSG_IFACE_VCAN_DESTROY  0x0014u

/* Definition management */
#define MSG_DEF_SIGNAL          0x0020u
#define MSG_DEF_PDU             0x0021u
#define MSG_DEF_FRAME           0x0022u
#define MSG_DEF_DELETE          0x0023u

/* Ownership management */
#define MSG_OWN_ACQUIRE         0x0030u
#define MSG_OWN_RELEASE         0x0031u
#define MSG_OWN_LOCK            0x0032u
#define MSG_OWN_UNLOCK          0x0033u

/* Persistence */
#define MSG_CFG_SAVE            0x0040u
#define MSG_CFG_LOAD            0x0041u

/* -------------------------------------------------------------------------
 * Message type constants — Server → Client  (SPEC §3.4)
 * ---------------------------------------------------------------------- */

#define MSG_SESSION_INIT_ACK    0x8001u
#define MSG_SESSION_CLOSE_ACK   0x8003u
#define MSG_IFACE_LIST_RESP     0x8010u
#define MSG_IFACE_ATTACH_ACK    0x8011u
#define MSG_IFACE_DETACH_ACK    0x8012u
#define MSG_IFACE_VCAN_ACK      0x8013u
#define MSG_DEF_ACK             0x8020u
#define MSG_OWN_ACK             0x8030u
#define MSG_CFG_ACK             0x8040u
#define MSG_NOTIFY_OWN_REVOKED  0x9001u
#define MSG_NOTIFY_IFACE_DOWN   0x9002u
#define MSG_NOTIFY_SERVER_CLOSE 0x9003u
#define MSG_ERR                 0xFFFFu

/* -------------------------------------------------------------------------
 * Error codes  (SPEC §3.5)
 * ---------------------------------------------------------------------- */

#define ERR_PROTOCOL_VERSION        0x0001u
#define ERR_UNKNOWN_MESSAGE_TYPE    0x0002u
#define ERR_PAYLOAD_TOO_LARGE       0x0003u
#define ERR_NOT_IN_SESSION          0x0004u
#define ERR_ALREADY_IN_SESSION      0x0005u
#define ERR_NAME_TOO_LONG           0x0006u
#define ERR_IFACE_NOT_FOUND         0x0010u
#define ERR_IFACE_ALREADY_ATTACHED  0x0011u
#define ERR_IFACE_ATTACH_FAILED     0x0012u
#define ERR_DEF_INVALID             0x0020u
#define ERR_DEF_CONFLICT            0x0021u
#define ERR_DEF_IN_USE              0x0022u
#define ERR_OWN_NOT_AVAILABLE       0x0030u
#define ERR_OWN_NOT_HELD            0x0031u
#define ERR_CFG_IO                  0x0040u
#define ERR_CFG_CHECKSUM            0x0041u
#define ERR_CFG_CONFLICT            0x0042u

#endif /* ASH_PROTO_H */
