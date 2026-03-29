#ifndef ASH_SERVER_OWN_H
#define ASH_SERVER_OWN_H

/* -------------------------------------------------------------------------
 * Signal ownership and ACL  (SPEC §8)
 *
 * Session-scoped ownership of signals.  At most one session may own a
 * given signal at a time.  Ownership can be locked to prevent transfer.
 * ---------------------------------------------------------------------- */

#include "server.h"
#include <stdint.h>

void own_init(server_t *s);
void own_destroy(void);
void own_register_handlers(void);

/* Release all signals owned by `session_id` (called from session_cleanup). */
void own_session_cleanup(uint32_t session_id);

/*
 * Returns 1 if `session_id` owns the named signal, 0 otherwise.
 * Used by app.c to authorise SIG_WRITE.
 */
int own_session_owns_signal(uint32_t session_id, const char *sig_name);

/* Callback type for own_foreach_session_signal. */
typedef void (*own_signal_cb_t)(const char *sig_name, uint8_t on_disconnect,
                                void *ctx);

/*
 * Iterate over all signals owned by `session_id`, calling `cb` for each
 * before ownership is released.  Used by app.c to apply on_disconnect
 * policies prior to own_session_cleanup().
 */
void own_foreach_session_signal(uint32_t session_id, own_signal_cb_t cb,
                                void *ctx);

/*
 * Returns 1 if any signal mapped to `frame_name` is currently owned by a
 * session other than `skip_session_id` with on_disconnect != stop (0x01).
 * Used by app.c to decide whether to keep a cyclic timer alive.
 */
int own_frame_has_continuing_signal(const char *frame_name,
                                    uint32_t skip_session_id);

#endif /* ASH_SERVER_OWN_H */
