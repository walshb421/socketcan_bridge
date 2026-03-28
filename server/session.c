#include "session.h"
#include "proto.h"
#include "iface.h"
#include "own.h"

#include "ash/proto.h"

#include <sys/timerfd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static server_t *g_server = NULL;

static uint32_t next_session_id = 1;  /* 0 is reserved for "no session" */

/* -------------------------------------------------------------------------
 * Global server pointer
 * ---------------------------------------------------------------------- */

void session_set_server(server_t *s)
{
    g_server = s;
}

/* -------------------------------------------------------------------------
 * Session lookup helpers
 * ---------------------------------------------------------------------- */

session_t *session_find_by_fd(server_t *s, int fd)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sessions[i].fd == fd)
            return &s->sessions[i];
    }
    return NULL;
}

session_t *session_find_by_timer_fd(server_t *s, int tfd)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sessions[i].fd >= 0 && s->sessions[i].timer_fd == tfd)
            return &s->sessions[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Keep-alive timer
 * ---------------------------------------------------------------------- */

void session_arm_timer(session_t *sess)
{
    struct itimerspec its;
    its.it_value.tv_sec  = KEEPALIVE_TIMEOUT_SEC;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 0;
    if (timerfd_settime(sess->timer_fd, 0, &its, NULL) < 0)
        perror("timerfd_settime");
}

/* -------------------------------------------------------------------------
 * Resource cleanup
 * ---------------------------------------------------------------------- */

void session_cleanup(server_t *s, session_t *sess)
{
    if (sess->session_id != 0) {
        own_session_cleanup(sess->session_id);
        iface_session_cleanup(s, sess->session_id);
    }
    sess->session_id = 0;
    memset(sess->client_name, 0, sizeof(sess->client_name));
}

/* -------------------------------------------------------------------------
 * Pre-dispatch session guard
 * ---------------------------------------------------------------------- */

int session_guard(int fd, uint16_t msg_type)
{
    session_t *sess = session_find_by_fd(g_server, fd);
    if (!sess)
        return -1;

    if (msg_type == MSG_SESSION_INIT) {
        if (sess->session_id != 0) {
            proto_send_err(fd, ERR_ALREADY_IN_SESSION, NULL);
            return -1;
        }
    } else {
        if (sess->session_id == 0) {
            proto_send_err(fd, ERR_NOT_IN_SESSION, NULL);
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Message handlers
 * ---------------------------------------------------------------------- */

/*
 * SESSION_INIT  (SPEC §4.1)
 *
 * Payload: 1-byte name_len + name_len bytes of client name (UTF-8).
 * Response: SESSION_INIT_ACK with 4-byte big-endian session ID.
 */
static int handle_session_init(int fd, const proto_frame_t *frame)
{
    session_t *sess = session_find_by_fd(g_server, fd);
    if (!sess)
        return -1;

    /* Parse client name */
    uint8_t name_len = 0;
    if (frame->hdr.payload_len >= 1)
        name_len = frame->payload[0];

    if (name_len > PROTO_MAX_NAME) {
        proto_send_err(fd, ERR_NAME_TOO_LONG, NULL);
        return 0;
    }

    if (frame->hdr.payload_len < (uint32_t)(1u + name_len)) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Assign session */
    sess->session_id = next_session_id++;
    if (next_session_id == 0)
        next_session_id = 1;   /* skip reserved 0 on wraparound */

    memcpy(sess->client_name, &frame->payload[1], name_len);
    sess->client_name[name_len] = '\0';

    /* Arm the keep-alive timer */
    session_arm_timer(sess);

    printf("ash-server: session %u established (client=\"%s\", fd=%d)\n",
           sess->session_id, sess->client_name, fd);

    /* Send ACK: 4-byte big-endian session ID */
    uint8_t ack[4];
    ack[0] = (sess->session_id >> 24) & 0xFF;
    ack[1] = (sess->session_id >> 16) & 0xFF;
    ack[2] = (sess->session_id >>  8) & 0xFF;
    ack[3] =  sess->session_id        & 0xFF;
    proto_send_ack(fd, MSG_SESSION_INIT_ACK, ack, 4);

    return 0;
}

/*
 * SESSION_KEEPALIVE  (SPEC §4.2)
 *
 * No payload, no response.  The keep-alive timer is reset by server_run()
 * in the EPOLLIN path before dispatch, so this handler is a no-op.
 */
static int handle_session_keepalive(int fd, const proto_frame_t *frame)
{
    (void)fd;
    (void)frame;
    return 0;
}

/*
 * SESSION_CLOSE  (SPEC §4.3)
 *
 * No payload.  Release all session resources, send SESSION_CLOSE_ACK,
 * then signal the server to close the TCP connection (return -1).
 */
static int handle_session_close(int fd, const proto_frame_t *frame)
{
    (void)frame;

    session_t *sess = session_find_by_fd(g_server, fd);
    if (!sess)
        return -1;

    printf("ash-server: session %u closing (client=\"%s\", fd=%d)\n",
           sess->session_id, sess->client_name, fd);

    session_cleanup(g_server, sess);
    proto_send_ack(fd, MSG_SESSION_CLOSE_ACK, NULL, 0);

    return -1;  /* instruct server_run() to close the connection */
}

/* -------------------------------------------------------------------------
 * Handler registration
 * ---------------------------------------------------------------------- */

void session_register_handlers(void)
{
    proto_register_handler(MSG_SESSION_INIT,      handle_session_init);
    proto_register_handler(MSG_SESSION_KEEPALIVE, handle_session_keepalive);
    proto_register_handler(MSG_SESSION_CLOSE,     handle_session_close);
}
