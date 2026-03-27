#ifndef ASH_SESSION_H
#define ASH_SESSION_H

#include "server.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Global server pointer — must be set before any session function is called.
 * ---------------------------------------------------------------------- */

void session_set_server(server_t *s);

/* -------------------------------------------------------------------------
 * Session lookup helpers
 * ---------------------------------------------------------------------- */

session_t *session_find_by_fd(server_t *s, int fd);
session_t *session_find_by_timer_fd(server_t *s, int tfd);

/* -------------------------------------------------------------------------
 * Keep-alive timer
 *
 * Arms (or rearms) the per-session timerfd to fire after
 * KEEPALIVE_TIMEOUT_SEC seconds.  Call after any message is received from
 * an established session.
 * ---------------------------------------------------------------------- */

void session_arm_timer(session_t *sess);

/* -------------------------------------------------------------------------
 * Resource cleanup
 *
 * Releases all resources held by *sess (owned signals, attached interfaces).
 * Clears session state but does NOT close the TCP fd or remove it from
 * epoll — that is the caller's responsibility.
 * ---------------------------------------------------------------------- */

void session_cleanup(server_t *s, session_t *sess);

/* -------------------------------------------------------------------------
 * Pre-dispatch session guard  (SPEC §4)
 *
 * Checks that the incoming message type is consistent with the current
 * session state on *fd*:
 *
 *   - SESSION_INIT on an already-established session → ERR_ALREADY_IN_SESSION
 *   - Any other message before session is established → ERR_NOT_IN_SESSION
 *
 * Returns  0 if dispatch should proceed.
 * Returns -1 if the guard sent an error and dispatch must be skipped
 *            (the connection stays open in both cases).
 * ---------------------------------------------------------------------- */

int session_guard(int fd, uint16_t msg_type);

/* -------------------------------------------------------------------------
 * Handler registration
 *
 * Registers handlers for SESSION_INIT, SESSION_KEEPALIVE, SESSION_CLOSE.
 * Must be called after session_set_server().
 * ---------------------------------------------------------------------- */

void session_register_handlers(void);

#endif /* ASH_SESSION_H */
