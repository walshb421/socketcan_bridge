#ifndef ASH_SERVER_IFACE_H
#define ASH_SERVER_IFACE_H

#include "server.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Interface management module (SPEC §7)
 *
 * Handles CAN interface discovery, attachment, detachment, and virtual
 * interface creation/destruction via Linux SocketCAN and netlink.
 * ---------------------------------------------------------------------- */

/* Set the server pointer (called before iface_init). */
void iface_set_server(server_t *s);

/*
 * Open the RTMGRP_LINK monitoring netlink socket and register it with the
 * server's epoll instance.  Also registers message handlers.
 *
 * Returns 0 on success, -1 on error.
 */
int iface_init(server_t *s);

/*
 * Close all attached CAN sockets and the monitoring netlink socket.
 * Called from server_destroy().
 */
void iface_destroy(void);

/*
 * Register IFACE_* message handlers with the dispatch table.
 * Called from iface_init().
 */
void iface_register_handlers(void);

/*
 * Process a pending netlink event on the monitoring socket.
 * Called from server_run() when the netlink fd is readable.
 */
void iface_handle_netlink(void);

/*
 * Release all interfaces owned by the given session.
 * Called from session_cleanup() before zeroing session state.
 */
void iface_session_cleanup(server_t *s, uint32_t session_id);

/*
 * Return the session_id that has attached the named interface,
 * or 0 if no session owns it.
 */
uint32_t iface_get_session_id(const char *iface_name);

/*
 * Return the SocketCAN fd for the named interface, or -1 if not attached.
 */
int iface_get_can_fd(const char *iface_name);

#endif /* ASH_SERVER_IFACE_H */
