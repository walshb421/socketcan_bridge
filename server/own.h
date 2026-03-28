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

#endif /* ASH_SERVER_OWN_H */
