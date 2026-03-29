#ifndef ASH_SERVER_DEF_H
#define ASH_SERVER_DEF_H

/* -------------------------------------------------------------------------
 * Signal/PDU/Frame definition store  (SPEC §6, §8.5)
 *
 * Global, session-agnostic in-memory namespace.  Definitions survive client
 * disconnects.  All three definition types share a single name namespace:
 * a name may not be used simultaneously for different types.
 * ---------------------------------------------------------------------- */

#include "ash/proto.h"
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

void def_init(void);
void def_destroy(void);
void def_register_handlers(void);

/* Returns 1 if a signal with the given name is defined, 0 otherwise. */
int def_signal_exists(const char *name);

/* -------------------------------------------------------------------------
 * Application-plane signal resolution  (used by app.c)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t  data_type;
    uint8_t  byte_order;
    uint8_t  bit_length;
    double   scale;
    double   offset_val;
    double   min_val;
    double   max_val;
    uint8_t  start_bit;
    uint8_t  pdu_byte_offset;
    char     frame_name[PROTO_MAX_NAME + 1];
    uint32_t frame_can_id;
    uint8_t  frame_id_type;
    uint8_t  frame_dlc;
    uint16_t frame_tx_period;
} def_sig_info_t;

/*
 * Resolve a signal by name and fill out all fields needed for packing.
 * Returns  0 if fully resolved (signal + PDU + frame all found).
 * Returns -1 if signal not found.
 * Returns -2 if signal found but not mapped to any PDU/frame.
 */
int    def_resolve_signal(const char *sig_name, def_sig_info_t *out);

/* Update the cached current value of a signal (used after write). */
void   def_update_signal_value(const char *sig_name, double value);

/* Retrieve the cached current value of a signal (0.0 if not found). */
double def_get_signal_value(const char *sig_name);

typedef struct {
    char   sig_name[PROTO_MAX_NAME + 1];
    double value;
} def_decoded_sig_t;

/*
 * Decode all signals in a received CAN frame.
 * Looks up the frame by (can_id, id_type), then unpacks each signal
 * and updates its cached current value.
 * Returns the number of signals decoded (may be 0), or -1 on error.
 */
int def_decode_frame_signals(uint32_t can_id, uint8_t id_type,
                              const uint8_t *data, uint8_t data_len,
                              def_decoded_sig_t *out, int max_out);

/* -------------------------------------------------------------------------
 * Serialization / deserialization  (used by cfg.c for persistence)
 * ---------------------------------------------------------------------- */

/* Return codes for def_validate_entry / def_apply_entry */
#define DEF_APPLY_OK       0
#define DEF_APPLY_CONFLICT 1
#define DEF_APPLY_INVALID  2

/*
 * Serialize all in-memory definitions into buf[0..bufsize-1].
 * Each entry is: type(1B) + payload_len(2B BE) + payload(N B).
 * Sets *count_out to the number of entries written.
 * Returns bytes written on success, or -1 if buf is too small.
 */
ssize_t def_serialize_entries(uint8_t *buf, size_t bufsize, uint16_t *count_out);

/*
 * Validate one entry (conflict-check only) without applying it.
 * entry_type: DEF_TYPE_SIGNAL/PDU/FRAME (0x01/0x02/0x03)
 * Returns DEF_APPLY_OK, DEF_APPLY_CONFLICT, or DEF_APPLY_INVALID.
 */
int def_validate_entry(uint8_t entry_type, const uint8_t *payload, uint32_t len);

/*
 * Apply one entry to the in-memory store (insert or overwrite).
 * Silently ignores malformed payloads or unresolvable references.
 */
void def_apply_entry(uint8_t entry_type, const uint8_t *payload, uint32_t len);

#endif /* ASH_SERVER_DEF_H */
