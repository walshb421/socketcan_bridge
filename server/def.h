#ifndef ASH_SERVER_DEF_H
#define ASH_SERVER_DEF_H

/* -------------------------------------------------------------------------
 * Signal/PDU/Frame definition store  (SPEC §6, §8.5)
 *
 * Global, session-agnostic in-memory namespace.  Definitions survive client
 * disconnects.  All three definition types share a single name namespace:
 * a name may not be used simultaneously for different types.
 * ---------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

void def_init(void);
void def_destroy(void);
void def_register_handlers(void);

/* Returns 1 if a signal with the given name is defined, 0 otherwise. */
int def_signal_exists(const char *name);

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
