#include "server.h"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Static helpers
 * ---------------------------------------------------------------------- */

static void session_add(server_t *s, int fd)
{
    if (s->nsessions >= MAX_SESSIONS) {
        fprintf(stderr, "session_add: no free session slots\n");
        close(fd);
        return;
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sessions[i].fd < 0) {
            s->sessions[i].fd = fd;
            s->nsessions++;
            return;
        }
    }
    fprintf(stderr, "session_add: no free session slots\n");
    close(fd);
}

static void session_remove(server_t *s, int fd)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sessions[i].fd == fd) {
            s->sessions[i].fd = -1;
            s->nsessions--;
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * epoll helpers
 * ---------------------------------------------------------------------- */

int server_add_fd(server_t *s, int fd, uint32_t events)
{
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl(ADD)");
        return -1;
    }
    return 0;
}

int server_del_fd(server_t *s, int fd)
{
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("epoll_ctl(DEL)");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * server_init
 * ---------------------------------------------------------------------- */

int server_init(server_t *s, uint16_t port, const char *storage_dir)
{
    memset(s, 0, sizeof(*s));
    s->epoll_fd   = -1;
    s->listen_fd  = -1;
    s->signal_fd  = -1;
    s->port       = port;
    s->storage_dir = storage_dir;

    for (int i = 0; i < MAX_SESSIONS; i++)
        s->sessions[i].fd = -1;

    /* --- epoll instance --- */
    s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s->epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /* --- signalfd for SIGTERM + SIGINT --- */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask");
        return -1;
    }

    s->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (s->signal_fd < 0) {
        perror("signalfd");
        return -1;
    }

    if (server_add_fd(s, s->signal_fd, EPOLLIN) < 0)
        return -1;

    /* --- TCP listener --- */
    s->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }

    if (listen(s->listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        return -1;
    }

    if (server_add_fd(s, s->listen_fd, EPOLLIN) < 0)
        return -1;

    printf("ash-server listening on port %u\n", (unsigned)port);
    return 0;
}

/* -------------------------------------------------------------------------
 * server_run
 * ---------------------------------------------------------------------- */

void server_run(server_t *s)
{
    struct epoll_event events[64];

    for (;;) {
        int n = epoll_wait(s->epoll_fd, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == s->signal_fd) {
                /* Drain the signalfd */
                struct signalfd_siginfo ssi;
                (void)read(s->signal_fd, &ssi, sizeof(ssi));
                printf("ash-server received signal %u, shutting down\n",
                       ssi.ssi_signo);
                return;

            } else if (fd == s->listen_fd) {
                /* Accept loop */
                for (;;) {
                    int cfd = accept4(s->listen_fd, NULL, NULL,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept4");
                        break;
                    }
                    if (server_add_fd(s, cfd, EPOLLIN | EPOLLRDHUP) < 0) {
                        close(cfd);
                        continue;
                    }
                    session_add(s, cfd);
                }

            } else {
                /* Client fd: error / hangup / peer closed */
                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    server_del_fd(s, fd);
                    session_remove(s, fd);
                    close(fd);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * server_destroy
 * ---------------------------------------------------------------------- */

void server_destroy(server_t *s)
{
    /* Notify and close all connected clients */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        int fd = s->sessions[i].fd;
        if (fd < 0)
            continue;
        (void)write(fd, NOTIFY_SERVER_CLOSING,
                    sizeof(NOTIFY_SERVER_CLOSING) - 1);
        server_del_fd(s, fd);
        close(fd);
        s->sessions[i].fd = -1;
    }

    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    if (s->signal_fd >= 0) {
        close(s->signal_fd);
        s->signal_fd = -1;
    }
    if (s->epoll_fd >= 0) {
        close(s->epoll_fd);
        s->epoll_fd = -1;
    }

    printf("ash-server stopped\n");
}
