/**
 * @file client.c
 * @brief DoIP client implementation (ISO 13400-2)
 *
 * Implements TCP diagnostic messaging and UDP vehicle discovery.
 * No main() — intended for integration into a larger application.
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
 * Shared helpers (also used by server.c — kept static to avoid ODR issues;
 * both TUs include the shared helpers via macros/inline below)
 * ========================================================================= */

/* ----------- Byte-order helpers ----------------------------------------- */

uint16_t doip_hton16(uint16_t v) { return htons(v); }
uint16_t doip_ntoh16(uint16_t v) { return ntohs(v); }
uint32_t doip_hton32(uint32_t v) { return htonl(v); }
uint32_t doip_ntoh32(uint32_t v) { return ntohl(v); }

/* ----------- Payload descriptor table ------------------------------------ */

static const doip_payload_descriptor_t g_payload_descriptors[] = {
    { DOIP_PT_GENERIC_NACK,
      "Generic NACK",
      sizeof(doip_generic_nack_t),
      sizeof(doip_generic_nack_t),
      0x0000 },

    { DOIP_PT_VEHICLE_ID_REQUEST,
      "Vehicle ID Request",
      0, 0,
      DOIP_PT_VEHICLE_ANNOUNCEMENT },

    { DOIP_PT_VEHICLE_ID_REQUEST_WITH_EID,
      "Vehicle ID Request (EID)",
      sizeof(doip_vehicle_id_request_eid_t),
      sizeof(doip_vehicle_id_request_eid_t),
      DOIP_PT_VEHICLE_ANNOUNCEMENT },

    { DOIP_PT_VEHICLE_ID_REQUEST_WITH_VIN,
      "Vehicle ID Request (VIN)",
      sizeof(doip_vehicle_id_request_vin_t),
      sizeof(doip_vehicle_id_request_vin_t),
      DOIP_PT_VEHICLE_ANNOUNCEMENT },

    { DOIP_PT_VEHICLE_ANNOUNCEMENT,
      "Vehicle Announcement",
      sizeof(doip_vehicle_announcement_t),
      sizeof(doip_vehicle_announcement_t),
      0x0000 },

    { DOIP_PT_ROUTING_ACTIVATION_REQUEST,
      "Routing Activation Request",
      sizeof(doip_routing_activation_request_t),
      sizeof(doip_routing_activation_request_t),
      DOIP_PT_ROUTING_ACTIVATION_RESPONSE },

    { DOIP_PT_ROUTING_ACTIVATION_RESPONSE,
      "Routing Activation Response",
      sizeof(doip_routing_activation_response_t),
      sizeof(doip_routing_activation_response_t),
      0x0000 },

    { DOIP_PT_ALIVE_CHECK_REQUEST,
      "Alive Check Request",
      0, 0,
      DOIP_PT_ALIVE_CHECK_RESPONSE },

    { DOIP_PT_ALIVE_CHECK_RESPONSE,
      "Alive Check Response",
      0, 0,
      0x0000 },

    { DOIP_PT_ENTITY_STATUS_REQUEST,
      "Entity Status Request",
      0, 0,
      DOIP_PT_ENTITY_STATUS_RESPONSE },

    { DOIP_PT_ENTITY_STATUS_RESPONSE,
      "Entity Status Response",
      sizeof(doip_entity_status_response_t),
      sizeof(doip_entity_status_response_t),
      0x0000 },

    { DOIP_PT_POWER_MODE_INFO_REQUEST,
      "Power Mode Info Request",
      0, 0,
      DOIP_PT_POWER_MODE_INFO_RESPONSE },

    { DOIP_PT_POWER_MODE_INFO_RESPONSE,
      "Power Mode Info Response",
      sizeof(doip_power_mode_response_t),
      sizeof(doip_power_mode_response_t),
      0x0000 },

    { DOIP_PT_DIAG_MESSAGE,
      "Diagnostic Message",
      sizeof(doip_diag_message_t),   /* SA+TA minimum, data optional */
      UINT32_MAX,
      DOIP_PT_DIAG_MESSAGE_POSITIVE_ACK },

    { DOIP_PT_DIAG_MESSAGE_POSITIVE_ACK,
      "Diagnostic Message Positive ACK",
      sizeof(doip_diag_positive_ack_t),
      UINT32_MAX,
      0x0000 },

    { DOIP_PT_DIAG_MESSAGE_NEGATIVE_ACK,
      "Diagnostic Message Negative ACK",
      sizeof(doip_diag_negative_ack_t),
      UINT32_MAX,
      0x0000 },
};

#define DOIP_NUM_DESCRIPTORS \
    (sizeof(g_payload_descriptors) / sizeof(g_payload_descriptors[0]))

const doip_payload_descriptor_t *
doip_get_payload_descriptor(doip_payload_type_t type)
{
    for (size_t i = 0; i < DOIP_NUM_DESCRIPTORS; ++i) {
        if (g_payload_descriptors[i].type == type) {
            return &g_payload_descriptors[i];
        }
    }
    return NULL;
}

doip_status_t
doip_validate_payload_length(doip_payload_type_t type, uint32_t length)
{
    const doip_payload_descriptor_t *desc = doip_get_payload_descriptor(type);
    if (!desc) {
        /* Unknown type — treated as variable length; let caller decide. */
        return DOIP_OK;
    }
    if (length < desc->min_length) return DOIP_ERR_PAYLOAD_LENGTH;
    if (desc->max_length != UINT32_MAX && length > desc->max_length)
        return DOIP_ERR_PAYLOAD_LENGTH;
    return DOIP_OK;
}

/* ----------- Header encode / decode -------------------------------------- */

void doip_encode_header(uint8_t *buf, doip_payload_type_t payload_type, uint32_t payload_len)
{
    buf[0] = DOIP_PROTOCOL_VERSION;
    buf[1] = DOIP_INVERSE_VERSION;
    buf[2] = (uint8_t)((payload_type >> 8) & 0xFF);
    buf[3] = (uint8_t)( payload_type       & 0xFF);
    buf[4] = (uint8_t)((payload_len >> 24) & 0xFF);
    buf[5] = (uint8_t)((payload_len >> 16) & 0xFF);
    buf[6] = (uint8_t)((payload_len >>  8) & 0xFF);
    buf[7] = (uint8_t)( payload_len        & 0xFF);
}

doip_status_t
doip_decode_header(const uint8_t *buf, doip_header_t *out)
{
    if (buf[0] != DOIP_PROTOCOL_VERSION ||
        buf[1] != (uint8_t)(~DOIP_PROTOCOL_VERSION & 0xFF)) {
        return DOIP_ERR_HEADER;
    }
    out->protocol_version = buf[0];
    out->inverse_version  = buf[1];
    out->payload_type     = (uint16_t)((buf[2] << 8) | buf[3]);
    out->payload_length   = ((uint32_t)buf[4] << 24) |
                            ((uint32_t)buf[5] << 16) |
                            ((uint32_t)buf[6] <<  8) |
                             (uint32_t)buf[7];
    return DOIP_OK;
}

/* ----------- Message lifecycle ------------------------------------------- */

doip_status_t
doip_message_create(doip_payload_type_t type,
                    const void         *payload,
                    uint32_t            length,
                    doip_message_t    **out)
{
    if (!out) return DOIP_ERR_INVALID_ARG;

    doip_message_t *msg = (doip_message_t *)malloc(sizeof(*msg));
    if (!msg) return DOIP_ERR_NO_MEMORY;

    msg->type           = type;
    msg->payload_length = length;
    msg->payload        = NULL;

    if (length > 0) {
        msg->payload = (uint8_t *)malloc(length);
        if (!msg->payload) {
            free(msg);
            return DOIP_ERR_NO_MEMORY;
        }
        if (payload) memcpy(msg->payload, payload, length);
    }

    *out = msg;
    return DOIP_OK;
}

void doip_message_free(doip_message_t *msg)
{
    if (!msg) return;
    free(msg->payload);
    free(msg);
}

/* =========================================================================
 * Internal I/O helpers
 * ========================================================================= */

/**
 * Send exactly @p len bytes from @p buf over @p fd.
 * Returns DOIP_OK or DOIP_ERR_SEND.
 */
static doip_status_t
send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return DOIP_ERR_SEND;
        sent += (size_t)n;
    }
    return DOIP_OK;
}

/**
 * Receive exactly @p len bytes into @p buf from @p fd.
 * Returns DOIP_OK, DOIP_ERR_RECV (closed), or DOIP_ERR_TIMEOUT.
 */
static doip_status_t
recv_all(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t recvd = 0;
    while (recvd < len) {
        if (timeout_ms > 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {
                .tv_sec  = timeout_ms / 1000,
                .tv_usec = (timeout_ms % 1000) * 1000
            };
            int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (ret == 0) return DOIP_ERR_TIMEOUT;
            if (ret < 0) return DOIP_ERR_RECV;
        }
        ssize_t n = recv(fd, buf + recvd, len - recvd, 0);
        if (n <= 0) return DOIP_ERR_RECV;
        recvd += (size_t)n;
    }
    return DOIP_OK;
}

/**
 * Build and send a complete DoIP message (header + payload) over a TCP fd.
 */
static doip_status_t
send_doip_tcp(int fd, doip_payload_type_t type, const void *payload, uint32_t len)
{
    uint8_t hdr[DOIP_HEADER_LENGTH];
    doip_encode_header(hdr, type, len);

    doip_status_t st = send_all(fd, hdr, DOIP_HEADER_LENGTH);
    if (st != DOIP_OK) return st;

    if (len > 0 && payload) {
        st = send_all(fd, (const uint8_t *)payload, len);
    }
    return st;
}

/**
 * Build and send a complete DoIP message as a UDP datagram.
 */
static doip_status_t
send_doip_udp(int fd, doip_payload_type_t type,
              const void *payload, uint32_t len,
              const struct sockaddr *dest, socklen_t dest_len)
{
    size_t total = DOIP_HEADER_LENGTH + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return DOIP_ERR_NO_MEMORY;

    doip_encode_header(buf, type, len);
    if (len > 0 && payload) {
        memcpy(buf + DOIP_HEADER_LENGTH, payload, len);
    }

    ssize_t n = sendto(fd, buf, total, 0, dest, dest_len);
    free(buf);
    return (n == (ssize_t)total) ? DOIP_OK : DOIP_ERR_SEND;
}

/**
 * Receive one complete DoIP message from a TCP fd.
 * Allocates *out; caller must call doip_message_free().
 */
static doip_status_t
recv_doip_tcp(int fd, uint32_t timeout_ms, doip_message_t **out)
{
    uint8_t hdr_buf[DOIP_HEADER_LENGTH];
    doip_status_t st = recv_all(fd, hdr_buf, DOIP_HEADER_LENGTH, timeout_ms);
    if (st != DOIP_OK) return st;

    doip_header_t hdr;
    st = doip_decode_header(hdr_buf, &hdr);
    if (st != DOIP_OK) return st;

    /* Guard against absurdly large allocations */
    if (hdr.payload_length > (64u * 1024 * 1024)) return DOIP_ERR_PAYLOAD_LENGTH;

    uint8_t *payload = NULL;
    if (hdr.payload_length > 0) {
        payload = (uint8_t *)malloc(hdr.payload_length);
        if (!payload) return DOIP_ERR_NO_MEMORY;

        st = recv_all(fd, payload, hdr.payload_length, timeout_ms);
        if (st != DOIP_OK) {
            free(payload);
            return st;
        }
    }

    st = doip_validate_payload_length((doip_payload_type_t)hdr.payload_type,
                                       hdr.payload_length);
    if (st != DOIP_OK) {
        free(payload);
        return st;
    }

    doip_message_t *msg = (doip_message_t *)malloc(sizeof(*msg));
    if (!msg) { free(payload); return DOIP_ERR_NO_MEMORY; }

    msg->type           = (doip_payload_type_t)hdr.payload_type;
    msg->payload_length = hdr.payload_length;
    msg->payload        = payload;
    *out = msg;
    return DOIP_OK;
}

/**
 * Receive one complete DoIP message from a UDP fd.
 * Allocates *out; caller must call doip_message_free().
 */
static doip_status_t
recv_doip_udp(int fd, uint32_t timeout_ms,
              char *src_ip, uint16_t *src_port,
              doip_message_t **out)
{
    if (timeout_ms > 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {
            .tv_sec  = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000
        };
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret == 0) return DOIP_ERR_TIMEOUT;
        if (ret < 0)  return DOIP_ERR_RECV;
    }

    /* Peek at the full datagram size first */
    uint8_t tmp[65536];
    struct sockaddr_storage sender;
    socklen_t sender_len = sizeof(sender);

    ssize_t n = recvfrom(fd, tmp, sizeof(tmp), 0,
                          (struct sockaddr *)&sender, &sender_len);
    if (n < 0) return DOIP_ERR_RECV;
    if ((size_t)n < DOIP_HEADER_LENGTH) return DOIP_ERR_HEADER;

    doip_header_t hdr;
    doip_status_t st = doip_decode_header(tmp, &hdr);
    if (st != DOIP_OK) return st;

    uint32_t expected = DOIP_HEADER_LENGTH + hdr.payload_length;
    if ((uint32_t)n < expected) return DOIP_ERR_PAYLOAD_LENGTH;

    st = doip_validate_payload_length((doip_payload_type_t)hdr.payload_type,
                                       hdr.payload_length);
    if (st != DOIP_OK) return st;

    /* Extract sender info */
    if (sender.ss_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&sender;
        if (src_ip)   inet_ntop(AF_INET, &s4->sin_addr, src_ip, 46);
        if (src_port) *src_port = ntohs(s4->sin_port);
    } else if (sender.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sender;
        if (src_ip)   inet_ntop(AF_INET6, &s6->sin6_addr, src_ip, 46);
        if (src_port) *src_port = ntohs(s6->sin6_port);
    }

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
 * Client context definition
 * ========================================================================= */

struct doip_client {
    int tcp_fd;     /**< Connected TCP diagnostic socket (-1 if closed) */
    int udp_fd;     /**< UDP discovery socket (-1 if closed) */
};

/* =========================================================================
 * Client lifecycle
 * ========================================================================= */

doip_status_t doip_client_create(doip_client_t **out)
{
    if (!out) return DOIP_ERR_INVALID_ARG;

    doip_client_t *c = (doip_client_t *)calloc(1, sizeof(*c));
    if (!c) return DOIP_ERR_NO_MEMORY;

    c->tcp_fd = -1;
    c->udp_fd = -1;
    *out = c;
    return DOIP_OK;
}

void doip_client_destroy(doip_client_t *client)
{
    if (!client) return;
    doip_client_disconnect_tcp(client);
    doip_client_close_udp(client);
    free(client);
}

void doip_client_disconnect_tcp(doip_client_t *client)
{
    if (!client || client->tcp_fd < 0) return;
    close(client->tcp_fd);
    client->tcp_fd = -1;
}

void doip_client_close_udp(doip_client_t *client)
{
    if (!client || client->udp_fd < 0) return;
    close(client->udp_fd);
    client->udp_fd = -1;
}

int doip_client_get_tcp_fd(const doip_client_t *client)
{
    return client ? client->tcp_fd : -1;
}

int doip_client_get_udp_fd(const doip_client_t *client)
{
    return client ? client->udp_fd : -1;
}

/* =========================================================================
 * Client socket initialisation
 * ========================================================================= */

doip_status_t
doip_client_connect_tcp(doip_client_t *client,
                         const char    *host,
                         uint16_t       port)
{
    if (!client || !host) return DOIP_ERR_INVALID_ARG;

    /* Close any existing connection */
    doip_client_disconnect_tcp(client);

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return DOIP_ERR_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return DOIP_ERR_INVALID_ARG;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return DOIP_ERR_CONNECT;
    }

    client->tcp_fd = fd;
    return DOIP_OK;
}

doip_status_t
doip_client_init_udp(doip_client_t *client, uint16_t port)
{
    if (!client) return DOIP_ERR_INVALID_ARG;

    doip_client_close_udp(client);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return DOIP_ERR_SOCKET;

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));

    if (port > 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return DOIP_ERR_BIND;
        }
    }

    client->udp_fd = fd;
    return DOIP_OK;
}

/* =========================================================================
 * Client send API
 * ========================================================================= */

doip_status_t
doip_client_send_tcp(doip_client_t        *client,
                      const doip_message_t *msg)
{
    if (!client || !msg) return DOIP_ERR_INVALID_ARG;
    if (client->tcp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    doip_status_t st = doip_validate_payload_length(msg->type, msg->payload_length);
    if (st != DOIP_OK) return st;

    return send_doip_tcp(client->tcp_fd, msg->type,
                          msg->payload, msg->payload_length);
}

doip_status_t
doip_client_send_udp(doip_client_t        *client,
                      const doip_message_t *msg,
                      const char           *dest_ip,
                      uint16_t              dest_port)
{
    if (!client || !msg || !dest_ip) return DOIP_ERR_INVALID_ARG;
    if (client->udp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    doip_status_t st = doip_validate_payload_length(msg->type, msg->payload_length);
    if (st != DOIP_OK) return st;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) <= 0)
        return DOIP_ERR_INVALID_ARG;

    return send_doip_udp(client->udp_fd, msg->type,
                          msg->payload, msg->payload_length,
                          (struct sockaddr *)&dest, sizeof(dest));
}

doip_status_t
doip_client_send_vehicle_id_request(doip_client_t *client,
                                     const char    *bcast_ip,
                                     uint16_t       port)
{
    if (!client || !bcast_ip) return DOIP_ERR_INVALID_ARG;
    if (client->udp_fd < 0)   return DOIP_ERR_NOT_INITIALIZED;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, bcast_ip, &dest.sin_addr) <= 0)
        return DOIP_ERR_INVALID_ARG;

    /* Vehicle ID Request has zero-length payload */
    return send_doip_udp(client->udp_fd,
                          DOIP_PT_VEHICLE_ID_REQUEST,
                          NULL, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
}

doip_status_t
doip_client_send_routing_activation(doip_client_t *client,
                                     uint16_t       source_address,
                                     uint8_t        activation_type)
{
    if (!client) return DOIP_ERR_INVALID_ARG;
    if (client->tcp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    doip_routing_activation_request_t req;
    memset(&req, 0, sizeof(req));
    req.source_address  = htons(source_address);
    req.activation_type = activation_type;
    /* reserved bytes are zeroed by memset */

    return send_doip_tcp(client->tcp_fd,
                          DOIP_PT_ROUTING_ACTIVATION_REQUEST,
                          &req, sizeof(req));
}

doip_status_t
doip_client_send_diag_message(doip_client_t *client,
                               uint16_t       source_address,
                               uint16_t       target_address,
                               const uint8_t *data,
                               uint32_t       data_length)
{
    if (!client) return DOIP_ERR_INVALID_ARG;
    if (client->tcp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    /* Build: [SA(2)] [TA(2)] [data...] */
    uint32_t total = (uint32_t)sizeof(doip_diag_message_t) + data_length;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return DOIP_ERR_NO_MEMORY;

    doip_diag_message_t *hdr = (doip_diag_message_t *)buf;
    hdr->source_address = htons(source_address);
    hdr->target_address = htons(target_address);

    if (data_length > 0 && data) {
        memcpy(buf + sizeof(doip_diag_message_t), data, data_length);
    }

    doip_status_t st = send_doip_tcp(client->tcp_fd,
                                      DOIP_PT_DIAG_MESSAGE,
                                      buf, total);
    free(buf);
    return st;
}

doip_status_t
doip_client_send_alive_check(doip_client_t *client)
{
    if (!client) return DOIP_ERR_INVALID_ARG;
    if (client->tcp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    return send_doip_tcp(client->tcp_fd,
                          DOIP_PT_ALIVE_CHECK_REQUEST,
                          NULL, 0);
}

/* =========================================================================
 * Client receive API
 * ========================================================================= */

doip_status_t
doip_client_recv_tcp(doip_client_t  *client,
                      uint32_t        timeout_ms,
                      doip_message_t **out)
{
    if (!client || !out) return DOIP_ERR_INVALID_ARG;
    if (client->tcp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    return recv_doip_tcp(client->tcp_fd, timeout_ms, out);
}

doip_status_t
doip_client_recv_udp(doip_client_t  *client,
                      uint32_t        timeout_ms,
                      char           *src_ip,
                      uint16_t       *src_port,
                      doip_message_t **out)
{
    if (!client || !out) return DOIP_ERR_INVALID_ARG;
    if (client->udp_fd < 0) return DOIP_ERR_NOT_INITIALIZED;

    return recv_doip_udp(client->udp_fd, timeout_ms, src_ip, src_port, out);
}
