#include "server.h"
#include "proto.h"
#include "session.h"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
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
            /* Create per-session timerfd for keep-alive enforcement */
            int tfd = timerfd_create(CLOCK_MONOTONIC,
                                     TFD_NONBLOCK | TFD_CLOEXEC);
            if (tfd < 0) {
                perror("timerfd_create");
                close(fd);
                return;
            }

            s->sessions[i].fd         = fd;
            s->sessions[i].timer_fd   = tfd;
            s->sessions[i].session_id = 0;
            memset(s->sessions[i].client_name, 0,
                   sizeof(s->sessions[i].client_name));

            /* Arm immediately so unauthenticated connections also time out */
            session_arm_timer(&s->sessions[i]);

            if (server_add_fd(s, tfd, EPOLLIN) < 0) {
                close(tfd);
                close(fd);
                s->sessions[i].fd       = -1;
                s->sessions[i].timer_fd = -1;
                return;
            }

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
            int tfd = s->sessions[i].timer_fd;
            if (tfd >= 0) {
                server_del_fd(s, tfd);
                close(tfd);
                s->sessions[i].timer_fd = -1;
            }
            s->sessions[i].fd         = -1;
            s->sessions[i].session_id = 0;
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

    for (int i = 0; i < MAX_SESSIONS; i++) {
        s->sessions[i].fd         = -1;
        s->sessions[i].timer_fd   = -1;
        s->sessions[i].session_id = 0;
    }

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

    /* --- Session module --- */
    session_set_server(s);
    session_register_handlers();

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
                struct signalfd_siginfo ssi;
                (void)read(s->signal_fd, &ssi, sizeof(ssi));
                printf("ash-server received signal %u, shutting down\n",
                       ssi.ssi_signo);
                return;

            } else if (fd == s->listen_fd) {
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
                /* Check if this fd is a keep-alive timerfd */
                session_t *tsess = session_find_by_timer_fd(s, fd);
                if (tsess) {
                    uint64_t exp;
                    (void)read(fd, &exp, sizeof(exp));  /* drain timerfd */
                    int client_fd = tsess->fd;
                    fprintf(stderr,
                            "ash-server: session %u timed out (fd=%d), closing\n",
                            tsess->session_id, client_fd);
                    session_cleanup(s, tsess);
                    server_del_fd(s, client_fd);
                    session_remove(s, client_fd);   /* closes timer_fd too */
                    close(client_fd);
                    continue;
                }

                /* Client fd: error / hangup / peer closed */
                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    session_t *sess = session_find_by_fd(s, fd);
                    if (sess)
                        session_cleanup(s, sess);
                    server_del_fd(s, fd);
                    session_remove(s, fd);
                    close(fd);
                    continue;
                }

                /* Client fd: data available */
                if (events[i].events & EPOLLIN) {
                    proto_frame_t frame;
                    int close_session = 0;

                    if (proto_read_frame(fd, &frame) < 0) {
                        session_t *sess = session_find_by_fd(s, fd);
                        if (sess)
                            session_cleanup(s, sess);
                        server_del_fd(s, fd);
                        session_remove(s, fd);
                        close(fd);
                        continue;
                    }

                    int err_code = proto_validate_header(&frame.hdr,
                                                         &close_session);
                    if (err_code != 0) {
                        proto_send_err(fd, (uint16_t)err_code, NULL);
                        proto_frame_free(&frame);
                        if (close_session) {
                            session_t *sess = session_find_by_fd(s, fd);
                            if (sess)
                                session_cleanup(s, sess);
                            server_del_fd(s, fd);
                            session_remove(s, fd);
                            close(fd);
                        }
                        continue;
                    }

                    /* Reset keep-alive timer on any message from an
                     * established session (SESSION_INIT arms it itself) */
                    session_t *sess = session_find_by_fd(s, fd);
                    if (sess && sess->session_id != 0)
                        session_arm_timer(sess);

                    /* Session guard: check state vs message type */
                    if (session_guard(fd, frame.hdr.msg_type) != 0) {
                        proto_frame_free(&frame);
                        continue;
                    }

                    int rc = proto_dispatch(fd, &frame);
                    proto_frame_free(&frame);

                    if (rc < 0) {
                        session_t *csess = session_find_by_fd(s, fd);
                        if (csess)
                            session_cleanup(s, csess);
                        server_del_fd(s, fd);
                        session_remove(s, fd);
                        close(fd);
                    }
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
    for (int i = 0; i < MAX_SESSIONS; i++) {
        int fd = s->sessions[i].fd;
        if (fd < 0)
            continue;
        session_cleanup(s, &s->sessions[i]);
        (void)proto_send_ack(fd, MSG_NOTIFY_SERVER_CLOSE, NULL, 0);
        server_del_fd(s, fd);
        int tfd = s->sessions[i].timer_fd;
        if (tfd >= 0) {
            server_del_fd(s, tfd);
            close(tfd);
            s->sessions[i].timer_fd = -1;
        }
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
