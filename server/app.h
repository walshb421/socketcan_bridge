#ifndef ASH_SERVER_APP_H
#define ASH_SERVER_APP_H

/* =========================================================================
 * server/app.h — Application Plane Runtime  (SPEC §5)
 * ========================================================================= */

#include "server.h"
#include <stdint.h>

/* Application-plane message types (Client → Server) */
#define APP_MSG_SIG_WRITE       0x0001u
#define APP_MSG_SIG_READ        0x0002u
#define APP_MSG_FRAME_TX        0x0010u

/* Application-plane message types (Server → Client) */
#define APP_MSG_SIG_READ_RESP   0x8002u
#define APP_MSG_SIG_RX          0x8003u
#define APP_MSG_FRAME_RX        0x8010u
#define APP_MSG_APP_ERR         0xFFFFu

/* Application-plane error codes */
#define APP_ERR_SIG_NOT_FOUND   0x0001u
#define APP_ERR_SIG_NOT_OWNED   0x0002u
#define APP_ERR_SIG_NOT_MAPPED  0x0003u
#define APP_ERR_FRAME_NOT_FOUND 0x0004u
#define APP_ERR_DLC_INVALID     0x0005u

void app_init(server_t *s);
void app_destroy(void);

/* Called by iface.c when an interface is attached */
void app_iface_attached(const char *iface_name, int can_fd, int app_listen_fd);

/* Called by iface.c before closing an interface's fds */
void app_iface_detach(const char *iface_name, int can_fd, int app_listen_fd);

/* Dispatch an epoll event; returns 1 if handled by app module, 0 otherwise */
int app_handle_event(int fd, uint32_t events);

#endif /* ASH_SERVER_APP_H */
