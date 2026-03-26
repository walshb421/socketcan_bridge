#ifndef ASH_SERVER_H
#define ASH_SERVER_H

#include <stdint.h>
#include <sys/epoll.h>

#define MAX_SESSIONS          1024
#define DEFAULT_PORT          4000

typedef struct {
    int fd;
} session_t;

typedef struct {
    int         epoll_fd;
    int         listen_fd;
    int         signal_fd;
    const char *storage_dir;
    uint16_t    port;
    session_t   sessions[MAX_SESSIONS];
    int         nsessions;
} server_t;

int  server_init(server_t *s, uint16_t port, const char *storage_dir);
void server_run(server_t *s);
void server_destroy(server_t *s);
int  server_add_fd(server_t *s, int fd, uint32_t events);
int  server_del_fd(server_t *s, int fd);

#endif /* ASH_SERVER_H */
