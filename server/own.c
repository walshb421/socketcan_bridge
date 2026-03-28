#include "own.h"
#include "proto.h"
#include "def.h"

#include "ash/proto.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Store limit
 * ---------------------------------------------------------------------- */

#define MAX_OWN_ENTRIES  512   /* one per signal maximum */

/* -------------------------------------------------------------------------
 * Per-signal ownership record
 * ---------------------------------------------------------------------- */

typedef struct {
    char     sig_name[PROTO_MAX_NAME + 1];
    uint32_t session_id;    /* 0 = unowned */
    int      client_fd;     /* fd of owning session (for NOTIFY_OWN_REVOKED) */
    uint8_t  locked;        /* 1 = locked, 0 = unlocked */
    uint8_t  on_disconnect; /* 0x01=stop 0x02=last 0x03=default */
} own_entry_t;

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static own_entry_t g_own[MAX_OWN_ENTRIES];
static int         g_own_count;
static server_t   *g_server;

/* -------------------------------------------------------------------------
 * Lookup helpers
 * ---------------------------------------------------------------------- */

static own_entry_t *own_find(const char *sig_name)
{
    for (int i = 0; i < g_own_count; i++) {
        if (strcmp(g_own[i].sig_name, sig_name) == 0)
            return &g_own[i];
    }
    return NULL;
}

/* Find the fd of a session by its session_id. Returns -1 if not found. */
static int find_session_fd(uint32_t session_id)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_server->sessions[i].fd >= 0 &&
            g_server->sessions[i].session_id == session_id)
            return g_server->sessions[i].fd;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Response helpers
 * ---------------------------------------------------------------------- */

static int send_own_ack(int fd, const char *name)
{
    size_t  nlen = strlen(name);
    uint8_t buf[PROTO_MAX_NAME + 1];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, name, nlen);
    return proto_send_ack(fd, MSG_OWN_ACK, buf, (uint32_t)(nlen + 1));
}

static void send_own_revoked(int fd, const char *name)
{
    size_t  nlen = strlen(name);
    uint8_t buf[PROTO_MAX_NAME + 1];
    buf[0] = (uint8_t)nlen;
    memcpy(buf + 1, name, nlen);
    proto_send_ack(fd, MSG_NOTIFY_OWN_REVOKED, buf, (uint32_t)(nlen + 1));
}

/* -------------------------------------------------------------------------
 * Parse helpers: all OWN messages share name_len + name prefix
 * ---------------------------------------------------------------------- */

/*
 * Parse a name from the frame payload.  Fills `name_out` (NUL-terminated)
 * and advances `*pp` / `*remaining`.
 * Returns 0 on success, -1 if the payload is malformed.
 */
static int parse_name(const uint8_t **pp, uint32_t *remaining,
                      char name_out[PROTO_MAX_NAME + 1])
{
    if (*remaining < 1u)
        return -1;

    uint8_t nlen = **pp; (*pp)++; (*remaining)--;

    if (nlen == 0 || nlen > PROTO_MAX_NAME || *remaining < (uint32_t)nlen)
        return -1;

    memcpy(name_out, *pp, nlen);
    name_out[nlen] = '\0';
    *pp        += nlen;
    *remaining -= nlen;
    return 0;
}

/* -------------------------------------------------------------------------
 * OWN_ACQUIRE handler  (SPEC §8.1)
 *
 * Payload:
 *   1B  name_len
 *   NB  name
 *   1B  on_disconnect (0x01=stop, 0x02=last, 0x03=default)
 * ---------------------------------------------------------------------- */

static int handle_own_acquire(int fd, const proto_frame_t *frame)
{
    /* Minimum: name_len(1) + name(≥1) + on_disconnect(1) = 3 */
    if (frame->hdr.payload_len < 3u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    char name[PROTO_MAX_NAME + 1];
    if (parse_name(&p, &remaining, name) < 0) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    if (remaining < 1u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }
    uint8_t on_disc = *p++; remaining--;

    if (on_disc < 0x01u || on_disc > 0x03u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Signal must be defined */
    if (!def_signal_exists(name)) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Find or allocate ownership record */
    own_entry_t *e = own_find(name);

    if (e && e->session_id != 0) {
        /* Determine requesting session */
        uint32_t req_session = 0;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_server->sessions[i].fd == fd) {
                req_session = g_server->sessions[i].session_id;
                break;
            }
        }

        if (e->session_id == req_session) {
            /* Re-acquire own signal: update on_disconnect, ACK */
            e->on_disconnect = on_disc;
            return send_own_ack(fd, name);
        }

        /* Owned by another session */
        if (e->locked) {
            proto_send_err(fd, ERR_OWN_NOT_AVAILABLE, NULL);
            return 0;
        }

        /* Transfer: notify previous owner, reassign */
        int prev_fd = find_session_fd(e->session_id);
        if (prev_fd >= 0)
            send_own_revoked(prev_fd, name);

        e->session_id    = req_session;
        e->client_fd     = fd;
        e->locked        = 0;
        e->on_disconnect = on_disc;
        return send_own_ack(fd, name);
    }

    /* Not yet owned (or entry exists with session_id == 0) */
    if (!e) {
        if (g_own_count >= MAX_OWN_ENTRIES) {
            proto_send_err(fd, ERR_OWN_NOT_AVAILABLE, NULL);
            return 0;
        }
        e = &g_own[g_own_count++];
    }

    /* Find session_id of the requesting fd */
    uint32_t req_session = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_server->sessions[i].fd == fd) {
            req_session = g_server->sessions[i].session_id;
            break;
        }
    }

    memcpy(e->sig_name, name, strlen(name) + 1);
    e->session_id    = req_session;
    e->client_fd     = fd;
    e->locked        = 0;
    e->on_disconnect = on_disc;

    return send_own_ack(fd, name);
}

/* -------------------------------------------------------------------------
 * OWN_RELEASE handler  (SPEC §8.2)
 *
 * Payload:
 *   1B  name_len
 *   NB  name
 * ---------------------------------------------------------------------- */

static int handle_own_release(int fd, const proto_frame_t *frame)
{
    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    char name[PROTO_MAX_NAME + 1];
    if (parse_name(&p, &remaining, name) < 0) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    own_entry_t *e = own_find(name);

    /* Determine requesting session_id */
    uint32_t req_session = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_server->sessions[i].fd == fd) {
            req_session = g_server->sessions[i].session_id;
            break;
        }
    }

    if (!e || e->session_id != req_session) {
        proto_send_err(fd, ERR_OWN_NOT_HELD, NULL);
        return 0;
    }

    e->session_id = 0;
    e->client_fd  = -1;
    e->locked     = 0;

    return send_own_ack(fd, name);
}

/* -------------------------------------------------------------------------
 * OWN_LOCK handler  (SPEC §8.3)
 *
 * Same payload as OWN_RELEASE.  Caller must own the signal.
 * ---------------------------------------------------------------------- */

static int handle_own_lock(int fd, const proto_frame_t *frame)
{
    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    char name[PROTO_MAX_NAME + 1];
    if (parse_name(&p, &remaining, name) < 0) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    own_entry_t *e = own_find(name);

    uint32_t req_session = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_server->sessions[i].fd == fd) {
            req_session = g_server->sessions[i].session_id;
            break;
        }
    }

    if (!e || e->session_id != req_session) {
        proto_send_err(fd, ERR_OWN_NOT_HELD, NULL);
        return 0;
    }

    e->locked = 1;

    return send_own_ack(fd, name);
}

/* -------------------------------------------------------------------------
 * OWN_UNLOCK handler  (SPEC §8.3)
 *
 * Same payload as OWN_RELEASE.  Caller must own the signal.
 * ---------------------------------------------------------------------- */

static int handle_own_unlock(int fd, const proto_frame_t *frame)
{
    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    char name[PROTO_MAX_NAME + 1];
    if (parse_name(&p, &remaining, name) < 0) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    own_entry_t *e = own_find(name);

    uint32_t req_session = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_server->sessions[i].fd == fd) {
            req_session = g_server->sessions[i].session_id;
            break;
        }
    }

    if (!e || e->session_id != req_session) {
        proto_send_err(fd, ERR_OWN_NOT_HELD, NULL);
        return 0;
    }

    e->locked = 0;

    return send_own_ack(fd, name);
}

/* -------------------------------------------------------------------------
 * Session cleanup  (called from session_cleanup)
 * ---------------------------------------------------------------------- */

void own_session_cleanup(uint32_t session_id)
{
    for (int i = 0; i < g_own_count; i++) {
        if (g_own[i].session_id == session_id) {
            /*
             * on_disconnect policy note: cyclic frame management (stop /
             * last / default) will be enforced in the application-plane
             * runtime (SPEC §10).  For now we simply release ownership so
             * the signal becomes available for re-acquisition.
             */
            g_own[i].session_id = 0;
            g_own[i].client_fd  = -1;
            g_own[i].locked     = 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */

void own_init(server_t *s)
{
    g_server    = s;
    g_own_count = 0;
}

void own_destroy(void)
{
    g_own_count = 0;
    g_server    = NULL;
}

void own_register_handlers(void)
{
    proto_register_handler(MSG_OWN_ACQUIRE, handle_own_acquire);
    proto_register_handler(MSG_OWN_RELEASE, handle_own_release);
    proto_register_handler(MSG_OWN_LOCK,    handle_own_lock);
    proto_register_handler(MSG_OWN_UNLOCK,  handle_own_unlock);
}
