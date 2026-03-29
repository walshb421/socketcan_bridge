#ifndef ASH_H
#define ASH_H

/* ash — CAN bridge client library  (SPEC §12.1) */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Opaque context
 * ---------------------------------------------------------------------- */

typedef struct ash_ctx ash_ctx_t;

/* -------------------------------------------------------------------------
 * Interface info  (returned by ash_iface_list)
 * ---------------------------------------------------------------------- */

#define ASH_IFACE_STATE_AVAILABLE 0x01u  /* not attached by any session     */
#define ASH_IFACE_STATE_MINE      0x02u  /* attached by this session        */
#define ASH_IFACE_STATE_OTHER     0x03u  /* attached by a different session */

typedef struct {
    char    name[64];
    uint8_t state;
} ash_iface_info_t;

/* -------------------------------------------------------------------------
 * Interface modes  (for ash_iface_attach)
 * ---------------------------------------------------------------------- */

#define ASH_MODE_CAN20A 0x01u  /* CAN 2.0A — standard IDs only        */
#define ASH_MODE_CAN20B 0x02u  /* CAN 2.0B — standard and extended IDs */
#define ASH_MODE_CANFD  0x03u  /* CAN FD                               */

/* -------------------------------------------------------------------------
 * Signal definition  (for ash_define_signal)
 * ---------------------------------------------------------------------- */

#define ASH_DATA_TYPE_UINT  0x01u  /* unsigned integer  */
#define ASH_DATA_TYPE_SINT  0x02u  /* signed integer    */
#define ASH_DATA_TYPE_FLOAT 0x03u  /* IEEE 754 float    */

#define ASH_BYTE_ORDER_LE   0x01u  /* little-endian (Intel)   */
#define ASH_BYTE_ORDER_BE   0x02u  /* big-endian (Motorola)   */

typedef struct {
    char    name[64];
    uint8_t data_type;
    uint8_t byte_order;
    uint8_t bit_length;
    double  scale;
    double  offset;
    double  min;
    double  max;
} ash_signal_def_t;

/* -------------------------------------------------------------------------
 * PDU definition  (for ash_define_pdu)
 * ---------------------------------------------------------------------- */

typedef struct {
    char    signal_name[64];
    uint8_t start_bit;
} ash_pdu_signal_map_t;

typedef struct {
    char                 name[64];
    uint8_t              length;        /* byte length of the PDU */
    uint8_t              signal_count;  /* 1–32 */
    ash_pdu_signal_map_t signals[32];
} ash_pdu_def_t;

/* -------------------------------------------------------------------------
 * Frame definition  (for ash_define_frame)
 * ---------------------------------------------------------------------- */

#define ASH_ID_TYPE_STD 0x01u  /* standard 11-bit CAN ID  */
#define ASH_ID_TYPE_EXT 0x02u  /* extended 29-bit CAN ID  */

typedef struct {
    char    pdu_name[64];
    uint8_t byte_offset;
} ash_frame_pdu_map_t;

typedef struct {
    char                name[64];
    uint32_t            can_id;
    uint8_t             id_type;       /* ASH_ID_TYPE_STD or ASH_ID_TYPE_EXT */
    uint8_t             dlc;
    uint16_t            tx_period_ms;  /* 0 = event-driven */
    uint8_t             pdu_count;     /* 1–8 */
    ash_frame_pdu_map_t pdus[8];
} ash_frame_def_t;

/* -------------------------------------------------------------------------
 * Definition types  (for ash_delete_def)
 * ---------------------------------------------------------------------- */

#define ASH_DEF_SIGNAL 0x01u
#define ASH_DEF_PDU    0x02u
#define ASH_DEF_FRAME  0x03u

/* -------------------------------------------------------------------------
 * On-disconnect policies  (for ash_acquire)
 * ---------------------------------------------------------------------- */

#define ASH_ON_DISCONNECT_STOP    0x01u  /* stop transmitting the frame      */
#define ASH_ON_DISCONNECT_LAST    0x02u  /* continue at last value           */
#define ASH_ON_DISCONNECT_DEFAULT 0x03u  /* revert to default (raw=0/offset) */

/* -------------------------------------------------------------------------
 * Events  (returned by ash_poll)
 * ---------------------------------------------------------------------- */

typedef enum {
    ASH_EVENT_SIG_RX              = 1,
    ASH_EVENT_FRAME_RX            = 2,
    ASH_EVENT_NOTIFY_OWN_REVOKED  = 3,
    ASH_EVENT_NOTIFY_IFACE_DOWN   = 4,
    ASH_EVENT_NOTIFY_SERVER_CLOSE = 5,
    ASH_EVENT_APP_ERR             = 6,
} ash_event_type_t;

typedef struct {
    ash_event_type_t type;
    char             iface[16];  /* interface this event arrived on (if applicable) */
    union {
        struct {
            char   signal[64];
            double value;
        } sig_rx;
        struct {
            uint32_t can_id;  /* bit 31 set = extended frame */
            uint8_t  dlc;
            uint8_t  flags;
            uint8_t  data[64];
        } frame_rx;
        struct {
            char signal[64];
        } own_revoked;
        struct {
            char iface[16];
        } iface_down;
        struct {
            uint16_t code;
        } app_err;
    } u;
} ash_event_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/* Connection */
ash_ctx_t *ash_connect(const char *host, uint16_t port, const char *client_name);
void       ash_disconnect(ash_ctx_t *ctx);
int        ash_keepalive(ash_ctx_t *ctx);

/* Interface management */
int ash_iface_list(ash_ctx_t *ctx, ash_iface_info_t *out, size_t max_count);
int ash_iface_attach(ash_ctx_t *ctx, const char *iface, uint8_t mode, uint32_t bitrate);
int ash_iface_detach(ash_ctx_t *ctx, const char *iface);
int ash_vcan_create(ash_ctx_t *ctx, const char *name);
int ash_vcan_destroy(ash_ctx_t *ctx, const char *name);

/* Definitions */
int ash_define_signal(ash_ctx_t *ctx, const ash_signal_def_t *def);
int ash_define_pdu(ash_ctx_t *ctx, const ash_pdu_def_t *def);
int ash_define_frame(ash_ctx_t *ctx, const ash_frame_def_t *def);
int ash_delete_def(ash_ctx_t *ctx, const char *name, uint8_t def_type);

/* Ownership */
int ash_acquire(ash_ctx_t *ctx, const char *signal, uint8_t on_disconnect);
int ash_release(ash_ctx_t *ctx, const char *signal);
int ash_lock(ash_ctx_t *ctx, const char *signal);
int ash_unlock(ash_ctx_t *ctx, const char *signal);

/* Runtime */
int ash_write(ash_ctx_t *ctx, const char *signal, double value);
int ash_read(ash_ctx_t *ctx, const char *signal, double *value_out);
int ash_frame_tx(ash_ctx_t *ctx, const char *iface, uint32_t can_id,
                 uint8_t dlc, uint8_t flags, const uint8_t *data);
int ash_poll(ash_ctx_t *ctx, ash_event_t *event, int timeout_ms);

/* Persistence */
int ash_cfg_save(ash_ctx_t *ctx, const char *name);
int ash_cfg_load(ash_ctx_t *ctx, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ASH_H */
