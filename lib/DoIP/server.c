/**
 * @file server.c
 * @brief DoIP server implementation (ISO 13400-2)
 *
 * Manages a TCP listening socket and a UDP socket.
 * Incoming messages are validated and dispatched to registered handler
 * callbacks.  Each callback must return a response whose payload type is
 * validated against the descriptor for the incoming message type before
 * transmission.
 *
 * No main() — integrate via doip_server_poll() in your event loop.
 */

#include "doip.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* =========================================================================
 * Shared helper declarations
 * (Definitions live in client.c; extern-linkage pulls them in at link time)
 * ========================================================================= */

/* These are defined in client.c with external linkage */
extern void           doip_encode_header(uint8_t *, doip_payload_type_t, uint32_t);
extern doip_status_t  doip_decode_header(const uint8_t *, doip_header_t *);
extern doip_status_t  doip_validate_payload_length(doip_payload_type_t, uint32_t);
extern const doip_payload_descriptor_t *doip_get_payload_descriptor(doip_payload_type_t);
extern doip_status_t  doip_message_create(doip_payload_type_t, const void *, uint32_t,
                                           doip_message_t **);
extern void           doip_message_free(doip_message_t *);

/* =========================================================================
 * Internal I/O helpers (local to this TU)
 * ========================================================================= */

/** Send exactly @p len bytes from @p buf over @p fd. */
static doip_status_t
srv_send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return DOIP_ERR_SEND;
        sent += (size_t)n;
    }
    return DOIP_OK;
}

/** Receive exactly @p len bytes into @p buf from @p fd (no timeout). */
static doip_status_t
srv_recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, buf + recvd, len - recvd, 0);
        if (n <= 0) return DOIP_ERR_RECV;
        recvd += (size_t)n;
    }
    return DOIP_OK;
}

/** Send a complete DoIP message (header+payload) over TCP fd. */
static doip_status_t
srv_send_tcp(int fd, doip_payload_type_t type,
             const void *payload, uint32_t len)
{
    uint8_t hdr[DOIP_HEADER_LENGTH];
    doip_encode_header(hdr, type, len);

    doip_status_t st = srv_send_all(fd, hdr, DOIP_HEADER_LENGTH);
    if (st != DOIP_OK) return st;
    if (len > 0 && payload)
        st = srv_send_all(fd, (const uint8_t *)payload, len);
    return st;
}

/** Send a complete DoIP message as a UDP datagram. */
static doip_status_t
srv_send_udp(int fd, doip_payload_type_t type,
             const void *payload, uint32_t len,
             const struct sockaddr *dest, socklen_t dest_len)
{
    size_t total = DOIP_HEADER_LENGTH + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return DOIP_ERR_NO_MEMORY;

    doip_encode_header(buf, type, len);
    if (len > 0 && payload)
        memcpy(buf + DOIP_HEADER_LENGTH, payload, len);

    ssize_t n = sendto(fd, buf, total, 0, dest, dest_len);
    free(buf);
    return (n == (ssize_t)total) ? DOIP_OK : DOIP_ERR_SEND;
}

/** Send a generic NACK over TCP. */
static void
srv_send_nack_tcp(int fd, doip_nack_code_t code)
{
    doip_generic_nack_t nack = { .nack_code = (uint8_t)code };
    srv_send_tcp(fd, DOIP_PT_GENERIC_NACK, &nack, sizeof(nack));
}

/** Send a generic NACK as a UDP datagram. */
static void
srv_send_nack_udp(int fd,
                  doip_nack_code_t code,
                  const struct sockaddr *dest,
                  socklen_t dest_len)
{
    doip_generic_nack_t nack = { .nack_code = (uint8_t)code };
    srv_send_udp(fd, DOIP_PT_GENERIC_NACK, &nack, sizeof(nack), dest, dest_len);
}

/**
 * Read one complete DoIP message from a TCP fd.
 * Allocates *out; caller frees with doip_message_free().
 */
static doip_status_t
srv_recv_tcp(int fd, doip_message_t **out)
{
    uint8_t hdr_buf[DOIP_HEADER_LENGTH];
    doip_status_t st = srv_recv_all(fd, hdr_buf, DOIP_HEADER_LENGTH);
    if (st != DOIP_OK) return st;

    doip_header_t hdr;
    st = doip_decode_header(hdr_buf, &hdr);
    if (st != DOIP_OK) return st;

    if (hdr.payload_length > (64u * 1024 * 1024)) return DOIP_ERR_PAYLOAD_LENGTH;

    uint8_t *payload = NULL;
    if (hdr.payload_length > 0) {
        payload = (uint8_t *)malloc(hdr.payload_length);
        if (!payload) return DOIP_ERR_NO_MEMORY;

        st = srv_recv_all(fd, payload, hdr.payload_length);
        if (st != DOIP_OK) { free(payload); return st; }
    }

    st = doip_validate_payload_length((doip_payload_type_t)hdr.payload_type,
                                       hdr.payload_length);
    if (st != DOIP_OK) { free(payload); return st; }

    doip_message_t *msg = (doip_message_t *)malloc(sizeof(*msg));
    if (!msg) { free(payload); return DOIP_ERR_NO_MEMORY; }

    msg->type           = (doip_payload_type_t)hdr.payload_type;
    msg->payload_length = hdr.payload_length;
    msg->payload        = payload;
    *out = msg;
    return DOIP_OK;
}

/**
 * Read one complete DoIP message from a UDP fd.
 * Populates sender address; allocates *out.
 */
static doip_status_t
srv_recv_udp(int fd,
             struct sockaddr_storage *sender,
             socklen_t               *sender_len,
             doip_message_t         **out)
{
    uint8_t tmp[65536];
    *sender_len = sizeof(*sender);

    ssize_t n = recvfrom(fd, tmp, sizeof(tmp), 0,
                          (struct sockaddr *)sender, sender_len);
    if (n < 0) return DOIP_ERR_RECV;
    if ((size_t)n < DOIP_HEADER_LENGTH) return DOIP_ERR_HEADER;

    doip_header_t hdr;
    doip_status_t st = doip_decode_header(tmp, &hdr);
    if (st != DOIP_OK) return st;

    if ((uint32_t)n < DOIP_HEADER_LENGTH + hdr.payload_length)
        return DOIP_ERR_PAYLOAD_LENGTH;

    st = doip_validate_payload_length((doip_payload_type_t)hdr.payload_type,
                                       hdr.payload_length);
    if (st != DOIP_OK) return st;

    uint8_t *payload = NULL;
    if (hdr.payload_length > 0) {
        payload = (uint8_t *)malloc(hdr.payload_length);
        if (!payload) return DOIP_ERR_NO_MEMORY;
        memcpy(payload, tmp + DOIP_HEADER_LENGTH, hdr.payload_length);
    }

    doip_message_t *msg = (doip_message_t *)malloc(sizeof(*msg));
    if (!msg) { free(payload); return DOIP_ERR_NO_MEMORY; }

    msg->type           = (doip_payload_type_t)hdr.payload_type;
    msg->payload_length = hdr.payload_length;
    msg->payload        = payload;
    *out = msg;
    return DOIP_OK;
}

/* =========================================================================
 * Server context definition
 * ========================================================================= */

/** One registered handler entry */
typedef struct {
    bool                active;
    doip_payload_type_t payload_type;
    doip_handler_fn     handler;
    void               *user_data;
} doip_handler_entry_t;

/** Active TCP client connection */
typedef struct {
    int      fd;
    uint16_t source_address;   /**< Tester SA after routing activation */
    bool     routing_active;
} doip_tcp_conn_t;

struct doip_server {
    int                   tcp_listen_fd;
    int                   udp_fd;
    uint16_t              logical_address;

    doip_tcp_conn_t       connections[DOIP_MAX_TCP_CONNECTIONS];
    int                   num_connections;

    doip_handler_entry_t  handlers[DOIP_MAX_CALLBACKS];
    int                   num_handlers;
};

/* =========================================================================
 * Server lifecycle
 * ========================================================================= */

doip_status_t
doip_server_create(uint16_t logical_address, doip_server_t **out)
{
    if (!out) return DOIP_ERR_INVALID_ARG;

    doip_server_t *s = (doip_server_t *)calloc(1, sizeof(*s));
    if (!s) return DOIP_ERR_NO_MEMORY;

    s->tcp_listen_fd  = -1;
    s->udp_fd         = -1;
    s->logical_address = logical_address;
    s->num_connections = 0;
    s->num_handlers    = 0;

    for (int i = 0; i < DOIP_MAX_TCP_CONNECTIONS; ++i)
        s->connections[i].fd = -1;

    *out = s;
    return DOIP_OK;
}

void doip_server_destroy(doip_server_t *server)
{
    if (!server) return;

    if (server->tcp_listen_fd >= 0) {
        close(server->tcp_listen_fd);
        server->tcp_listen_fd = -1;
    }
    if (server->udp_fd >= 0) {
        close(server->udp_fd);
        server->udp_fd = -1;
    }
    for (int i = 0; i < DOIP_MAX_TCP_CONNECTIONS; ++i) {
        if (server->connections[i].fd >= 0) {
            close(server->connections[i].fd);
            server->connections[i].fd = -1;
        }
    }
    free(server);
}

int doip_server_get_tcp_fd(const doip_server_t *server)
{
    return server ? server->tcp_listen_fd : -1;
}

int doip_server_get_udp_fd(const doip_server_t *server)
{
    return server ? server->udp_fd : -1;
}

/* =========================================================================
 * Server socket initialisation
 * ========================================================================= */

doip_status_t
doip_server_init_tcp(doip_server_t *server,
                      const char    *bind_ip,
                      uint16_t       port)
{
    if (!server) return DOIP_ERR_INVALID_ARG;

    if (server->tcp_listen_fd >= 0) {
        close(server->tcp_listen_fd);
        server->tcp_listen_fd = -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return DOIP_ERR_SOCKET;

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (bind_ip && bind_ip[0] != '\0') {
        if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) <= 0) {
            close(fd);
            return DOIP_ERR_INVALID_ARG;
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return DOIP_ERR_BIND;
    }

    if (listen(fd, DOIP_TCP_BACKLOG) < 0) {
        close(fd);
        return DOIP_ERR_SOCKET;
    }

    server->tcp_listen_fd = fd;
    return DOIP_OK;
}

doip_status_t
doip_server_init_udp(doip_server_t *server,
                      const char    *bind_ip,
                      uint16_t       port)
{
    if (!server) return DOIP_ERR_INVALID_ARG;

    if (server->udp_fd >= 0) {
        close(server->udp_fd);
        server->udp_fd = -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return DOIP_ERR_SOCKET;

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (bind_ip && bind_ip[0] != '\0') {
        if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) <= 0) {
            close(fd);
            return DOIP_ERR_INVALID_ARG;
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return DOIP_ERR_BIND;
    }

    server->udp_fd = fd;
    return DOIP_OK;
}

/* =========================================================================
 * Callback registration
 * ========================================================================= */

doip_status_t
doip_server_register_handler(doip_server_t      *server,
                               doip_payload_type_t payload_type,
                               doip_handler_fn     handler,
                               void               *user_data)
{
    if (!server || !handler) return DOIP_ERR_INVALID_ARG;

    /* Check for duplicate */
    for (int i = 0; i < server->num_handlers; ++i) {
        if (server->handlers[i].active &&
            server->handlers[i].payload_type == payload_type) {
            return DOIP_ERR_CALLBACK_EXISTS;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < DOIP_MAX_CALLBACKS; ++i) {
        if (!server->handlers[i].active) {
            server->handlers[i].active       = true;
            server->handlers[i].payload_type  = payload_type;
            server->handlers[i].handler       = handler;
            server->handlers[i].user_data     = user_data;
            if (i >= server->num_handlers) server->num_handlers = i + 1;
            return DOIP_OK;
        }
    }
    return DOIP_ERR_CALLBACK_FULL;
}

doip_status_t
doip_server_deregister_handler(doip_server_t      *server,
                                 doip_payload_type_t payload_type)
{
    if (!server) return DOIP_ERR_INVALID_ARG;

    for (int i = 0; i < DOIP_MAX_CALLBACKS; ++i) {
        if (server->handlers[i].active &&
            server->handlers[i].payload_type == payload_type) {
            memset(&server->handlers[i], 0, sizeof(server->handlers[i]));
            return DOIP_OK;
        }
    }
    return DOIP_ERR_INVALID_ARG;
}

/* =========================================================================
 * Internal dispatch helpers
 * ========================================================================= */

/** Find the handler entry for a given payload type (or NULL). */
static doip_handler_entry_t *
find_handler(doip_server_t *server, doip_payload_type_t type)
{
    for (int i = 0; i < DOIP_MAX_CALLBACKS; ++i) {
        if (server->handlers[i].active &&
            server->handlers[i].payload_type == type) {
            return &server->handlers[i];
        }
    }
    return NULL;
}

/**
 * Validate that a response message returned by a handler is correct for the
 * given incoming request type.
 *
 * Rules:
 *  - If the request descriptor has expected_response == 0 → any non-NULL
 *    response is acceptable (passthrough).
 *  - Otherwise the response type must match expected_response.
 *  - The response payload length must pass doip_validate_payload_length().
 */
static doip_status_t
validate_response(doip_payload_type_t      request_type,
                  const doip_message_t    *response)
{
    if (!response) return DOIP_ERR_INVALID_ARG;

    const doip_payload_descriptor_t *req_desc =
        doip_get_payload_descriptor(request_type);

    if (req_desc && req_desc->expected_response != 0x0000) {
        if (response->type != req_desc->expected_response) {
            fprintf(stderr,
                "doip_server: handler for type 0x%04X returned response "
                "type 0x%04X, expected 0x%04X\n",
                (unsigned)request_type,
                (unsigned)response->type,
                (unsigned)req_desc->expected_response);
            return DOIP_ERR_CALLBACK_RESPONSE;
        }
    }

    return doip_validate_payload_length(response->type, response->payload_length);
}

/**
 * Dispatch a received message to its handler and transmit the response
 * over TCP.  Handles NACK sending on all error paths.
 *
 * @param server     Server context.
 * @param conn       Active TCP connection descriptor.
 * @param msg        Received message (ownership stays with caller).
 */
static void
dispatch_tcp(doip_server_t *server, doip_tcp_conn_t *conn,
             doip_message_t *msg)
{
    doip_handler_entry_t *entry = find_handler(server, msg->type);

    if (!entry) {
        /* No handler: send generic NACK */
        fprintf(stderr, "doip_server: no handler for payload type 0x%04X\n",
                (unsigned)msg->type);
        srv_send_nack_tcp(conn->fd, DOIP_NACK_UNKNOWN_PAYLOAD_TYPE);
        return;
    }

    doip_message_t *response = NULL;
    doip_status_t   st = entry->handler(server, conn->fd, msg,
                                         &response, entry->user_data);

    if (st != DOIP_OK || !response) {
        fprintf(stderr,
            "doip_server: handler for type 0x%04X returned error %d\n",
            (unsigned)msg->type, st);
        srv_send_nack_tcp(conn->fd, DOIP_NACK_OUT_OF_MEMORY);
        doip_message_free(response);
        return;
    }

    /* Validate the response type and length */
    st = validate_response(msg->type, response);
    if (st != DOIP_OK) {
        srv_send_nack_tcp(conn->fd, DOIP_NACK_INVALID_PAYLOAD_LENGTH);
        doip_message_free(response);
        return;
    }

    srv_send_tcp(conn->fd, response->type,
                  response->payload, response->payload_length);
    doip_message_free(response);
}

/**
 * Dispatch a received UDP message to its handler and transmit the response
 * as a UDP datagram back to the sender.
 */
static void
dispatch_udp(doip_server_t          *server,
             doip_message_t         *msg,
             const struct sockaddr  *sender,
             socklen_t               sender_len)
{
    doip_handler_entry_t *entry = find_handler(server, msg->type);

    if (!entry) {
        srv_send_nack_udp(server->udp_fd,
                           DOIP_NACK_UNKNOWN_PAYLOAD_TYPE,
                           sender, sender_len);
        return;
    }

    doip_message_t *response = NULL;
    doip_status_t   st = entry->handler(server, -1, msg,
                                         &response, entry->user_data);

    if (st != DOIP_OK || !response) {
        srv_send_nack_udp(server->udp_fd,
                           DOIP_NACK_OUT_OF_MEMORY,
                           sender, sender_len);
        doip_message_free(response);
        return;
    }

    st = validate_response(msg->type, response);
    if (st != DOIP_OK) {
        srv_send_nack_udp(server->udp_fd,
                           DOIP_NACK_INVALID_PAYLOAD_LENGTH,
                           sender, sender_len);
        doip_message_free(response);
        return;
    }

    srv_send_udp(server->udp_fd,
                  response->type,
                  response->payload,
                  response->payload_length,
                  sender, sender_len);
    doip_message_free(response);
}

/* =========================================================================
 * Built-in protocol handlers (routing activation, alive check)
 * These are handled internally before user callbacks so the application
 * does not need to implement them unless it needs custom behaviour.
 * ========================================================================= */

/**
 * Handle a Routing Activation Request.
 * Sends a Routing Activation Response directly; returns true if handled.
 */
static bool
handle_routing_activation(doip_server_t *server, doip_tcp_conn_t *conn,
                           const doip_message_t *msg)
{
    if (msg->payload_length < sizeof(doip_routing_activation_request_t))
        return false;

    const doip_routing_activation_request_t *req =
        (const doip_routing_activation_request_t *)msg->payload;

    doip_routing_activation_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.tester_logical_address = req->source_address;   /* already NBO */
    resp.entity_logical_address = htons(server->logical_address);
    resp.response_code          = DOIP_ROUTING_SUCCESS;

    conn->source_address  = ntohs(req->source_address);
    conn->routing_active  = true;

    srv_send_tcp(conn->fd,
                  DOIP_PT_ROUTING_ACTIVATION_RESPONSE,
                  &resp, sizeof(resp));
    return true;
}

/**
 * Handle an Alive Check Request — respond with an Alive Check Response.
 * Returns true if handled.
 */
static bool
handle_alive_check(doip_server_t *server, doip_tcp_conn_t *conn,
                   const doip_message_t *msg)
{
    (void)server;
    (void)msg;
    /* Response has zero-length payload */
    srv_send_tcp(conn->fd, DOIP_PT_ALIVE_CHECK_RESPONSE, NULL, 0);
    return true;
}

/* =========================================================================
 * Connection management helpers
 * ========================================================================= */

/** Accept a new TCP connection and store it. Returns false if slots full. */
static bool
accept_connection(doip_server_t *server)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int cfd = accept(server->tcp_listen_fd,
                      (struct sockaddr *)&client_addr, &client_len);
    if (cfd < 0) return false;

    for (int i = 0; i < DOIP_MAX_TCP_CONNECTIONS; ++i) {
        if (server->connections[i].fd < 0) {
            server->connections[i].fd             = cfd;
            server->connections[i].source_address = 0;
            server->connections[i].routing_active = false;
            server->num_connections++;
            return true;
        }
    }

    /* No free slot */
    close(cfd);
    return false;
}

/** Remove a TCP connection by index, closing its fd. */
static void
remove_connection(doip_server_t *server, int idx)
{
    if (server->connections[idx].fd >= 0) {
        close(server->connections[idx].fd);
        server->connections[idx].fd             = -1;
        server->connections[idx].source_address = 0;
        server->connections[idx].routing_active = false;
        server->num_connections--;
    }
}

/* =========================================================================
 * Server poll (main event loop entry point)
 * ========================================================================= */

doip_status_t
doip_server_poll(doip_server_t *server, uint32_t timeout_ms)
{
    if (!server) return DOIP_ERR_INVALID_ARG;
    if (server->tcp_listen_fd < 0 && server->udp_fd < 0)
        return DOIP_ERR_NOT_INITIALIZED;

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;

    if (server->tcp_listen_fd >= 0) {
        FD_SET(server->tcp_listen_fd, &rfds);
        if (server->tcp_listen_fd > maxfd) maxfd = server->tcp_listen_fd;
    }

    if (server->udp_fd >= 0) {
        FD_SET(server->udp_fd, &rfds);
        if (server->udp_fd > maxfd) maxfd = server->udp_fd;
    }

    for (int i = 0; i < DOIP_MAX_TCP_CONNECTIONS; ++i) {
        int fd = server->connections[i].fd;
        if (fd >= 0) {
            FD_SET(fd, &rfds);
            if (fd > maxfd) maxfd = fd;
        }
    }

    if (maxfd < 0) return DOIP_OK;   /* nothing to wait on */

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) return DOIP_OK;
        return DOIP_ERR_RECV;
    }
    if (ret == 0) return DOIP_OK;   /* timeout — no events */

    /* --- New TCP connection --- */
    if (server->tcp_listen_fd >= 0 &&
        FD_ISSET(server->tcp_listen_fd, &rfds)) {
        accept_connection(server);  /* best-effort; errors silently drop */
    }

    /* --- UDP message --- */
    if (server->udp_fd >= 0 && FD_ISSET(server->udp_fd, &rfds)) {
        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof(sender);
        doip_message_t *msg  = NULL;

        doip_status_t st = srv_recv_udp(server->udp_fd,
                                         &sender, &sender_len, &msg);
        if (st == DOIP_OK && msg) {
            dispatch_udp(server, msg,
                          (struct sockaddr *)&sender, sender_len);
            doip_message_free(msg);
        } else {
            srv_send_nack_udp(server->udp_fd,
                               DOIP_NACK_INCORRECT_PATTERN,
                               (struct sockaddr *)&sender, sender_len);
        }
    }

    /* --- Data on existing TCP connections --- */
    for (int i = 0; i < DOIP_MAX_TCP_CONNECTIONS; ++i) {
        doip_tcp_conn_t *conn = &server->connections[i];
        if (conn->fd < 0 || !FD_ISSET(conn->fd, &rfds)) continue;

        doip_message_t *msg = NULL;
        doip_status_t st = srv_recv_tcp(conn->fd, &msg);

        if (st != DOIP_OK) {
            /* Peer closed or receive error — tear down the connection */
            remove_connection(server, i);
            continue;
        }

        /* Route built-in protocol messages before user callbacks */
        bool handled = false;
        switch (msg->type) {
        case DOIP_PT_ROUTING_ACTIVATION_REQUEST:
            handled = handle_routing_activation(server, conn, msg);
            break;
        case DOIP_PT_ALIVE_CHECK_REQUEST:
            handled = handle_alive_check(server, conn, msg);
            break;
        default:
            break;
        }

        if (!handled) {
            /* Check for user-registered handler */
            doip_handler_entry_t *entry = find_handler(server, msg->type);
            if (entry) {
                dispatch_tcp(server, conn, msg);
            } else {
                srv_send_nack_tcp(conn->fd, DOIP_NACK_UNKNOWN_PAYLOAD_TYPE);
            }
        }

        doip_message_free(msg);
    }

    return DOIP_OK;
}
