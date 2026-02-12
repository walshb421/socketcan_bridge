/**
 * @file doip.h
 * @brief Diagnostics over IP (DoIP) implementation - ISO 13400-2
 *
 * Provides types, constants, and interfaces for a DoIP client and server.
 * Covers UDP (vehicle discovery/announcement) and TCP (diagnostic messaging).
 *
 * Port assignments (ISO 13400-2, Table 13):
 *   UDP: 13400  (vehicle announcement / entity status)
 *   TCP: 13400  (diagnostic communication)
 *   UDP: 13400  (broadcast/multicast discovery)
 */

#ifndef DOIP_H
#define DOIP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define DOIP_PORT                   13400
#define DOIP_TLS_PORT               3496

#define DOIP_PROTOCOL_VERSION       0x02   /* ISO 13400-2:2012 */
#define DOIP_INVERSE_VERSION        0xFD   /* ~DOIP_PROTOCOL_VERSION */

#define DOIP_HEADER_LENGTH          8      /* bytes: ver(1)+inv(1)+type(2)+len(4) */
#define DOIP_MAX_PAYLOAD_LENGTH     0xFFFFFFFF

/* Maximum number of registered server callbacks */
#define DOIP_MAX_CALLBACKS          32

/* TCP connection limits */
#define DOIP_MAX_TCP_CONNECTIONS    64
#define DOIP_TCP_BACKLOG            8

/* Timeouts (milliseconds) */
#define DOIP_T_TCP_GENERAL_INACTIVITY_MS  5000
#define DOIP_T_TCP_INITIAL_INACTIVITY_MS  2000
#define DOIP_T_TCP_ALIVE_CHECK_MS         500

/* =========================================================================
 * Payload Type Identifiers (ISO 13400-2, Table 17)
 * ========================================================================= */

typedef enum {
    /* Generic / Network management */
    DOIP_PT_GENERIC_NACK                        = 0x0000,

    /* Vehicle discovery (UDP) */
    DOIP_PT_VEHICLE_ID_REQUEST                  = 0x0001,
    DOIP_PT_VEHICLE_ID_REQUEST_WITH_EID         = 0x0002,
    DOIP_PT_VEHICLE_ID_REQUEST_WITH_VIN         = 0x0003,
    DOIP_PT_VEHICLE_ANNOUNCEMENT                = 0x0004,
    DOIP_PT_ROUTING_ACTIVATION_REQUEST          = 0x0005,
    DOIP_PT_ROUTING_ACTIVATION_RESPONSE         = 0x0006,
    DOIP_PT_ALIVE_CHECK_REQUEST                 = 0x0007,
    DOIP_PT_ALIVE_CHECK_RESPONSE                = 0x0008,

    /* Entity / status (UDP) */
    DOIP_PT_ENTITY_STATUS_REQUEST               = 0x4001,
    DOIP_PT_ENTITY_STATUS_RESPONSE              = 0x4002,
    DOIP_PT_POWER_MODE_INFO_REQUEST             = 0x4003,
    DOIP_PT_POWER_MODE_INFO_RESPONSE            = 0x4004,

    /* Diagnostic messaging (TCP) */
    DOIP_PT_DIAG_MESSAGE                        = 0x8001,
    DOIP_PT_DIAG_MESSAGE_POSITIVE_ACK           = 0x8002,
    DOIP_PT_DIAG_MESSAGE_NEGATIVE_ACK           = 0x8003,
} doip_payload_type_t;

/* =========================================================================
 * NACK / Response Codes
 * ========================================================================= */

/** Generic header NACK codes (ISO 13400-2, Table 18) */
typedef enum {
    DOIP_NACK_INCORRECT_PATTERN             = 0x00,
    DOIP_NACK_UNKNOWN_PAYLOAD_TYPE          = 0x01,
    DOIP_NACK_MESSAGE_TOO_LARGE             = 0x02,
    DOIP_NACK_OUT_OF_MEMORY                 = 0x03,
    DOIP_NACK_INVALID_PAYLOAD_LENGTH        = 0x04,
} doip_nack_code_t;

/** Routing activation response codes (ISO 13400-2, Table 25) */
typedef enum {
    DOIP_ROUTING_DENIED_UNKNOWN_SA          = 0x00,
    DOIP_ROUTING_DENIED_ALL_SOCKETS_BUSY    = 0x01,
    DOIP_ROUTING_DENIED_SA_MISMATCH         = 0x02,
    DOIP_ROUTING_DENIED_SA_REGISTERED       = 0x03,
    DOIP_ROUTING_DENIED_MISSING_AUTH        = 0x04,
    DOIP_ROUTING_DENIED_REJECTED_CONFIRM    = 0x05,
    DOIP_ROUTING_DENIED_UNSUPPORTED_TYPE    = 0x06,
    DOIP_ROUTING_SUCCESS                    = 0x10,
    DOIP_ROUTING_PENDING_CONFIRMATION       = 0x11,
} doip_routing_response_code_t;

/** Diagnostic message ACK codes (ISO 13400-2, Table 31) */
typedef enum {
    DOIP_DIAG_ACK_OK                        = 0x00,
} doip_diag_ack_code_t;

/** Diagnostic message NACK codes (ISO 13400-2, Table 32) */
typedef enum {
    DOIP_DIAG_NACK_RESERVED                 = 0x00,
    DOIP_DIAG_NACK_INVALID_SA               = 0x02,
    DOIP_DIAG_NACK_UNKNOWN_TA               = 0x03,
    DOIP_DIAG_NACK_MESSAGE_TOO_LARGE        = 0x04,
    DOIP_DIAG_NACK_OUT_OF_MEMORY            = 0x05,
    DOIP_DIAG_NACK_TARGET_UNREACHABLE       = 0x06,
    DOIP_DIAG_NACK_UNKNOWN_NETWORK          = 0x07,
    DOIP_DIAG_NACK_TRANSPORT_PROTOCOL_ERROR = 0x08,
} doip_diag_nack_code_t;

/* =========================================================================
 * Wire-Format Structures
 * ========================================================================= */

/**
 * Generic DoIP header (ISO 13400-2, Table 14).
 * All multi-byte fields are big-endian on the wire; helpers below handle
 * byte-order conversion.
 */
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;      /**< Always DOIP_PROTOCOL_VERSION */
    uint8_t  inverse_version;       /**< Always ~protocol_version */
    uint16_t payload_type;          /**< doip_payload_type_t (network byte order) */
    uint32_t payload_length;        /**< Length of payload following header (NBO) */
} doip_header_t;

/** Generic NACK payload */
typedef struct __attribute__((packed)) {
    uint8_t nack_code;              /**< doip_nack_code_t */
} doip_generic_nack_t;

/** Routing Activation Request payload (ISO 13400-2, Table 24) */
typedef struct __attribute__((packed)) {
    uint16_t source_address;        /**< Tester logical address (NBO) */
    uint8_t  activation_type;       /**< 0x00 = default */
    uint8_t  reserved[4];           /**< ISO reserved, shall be 0 */
} doip_routing_activation_request_t;

/** Routing Activation Response payload (ISO 13400-2, Table 25) */
typedef struct __attribute__((packed)) {
    uint16_t tester_logical_address;    /**< Echo of SA (NBO) */
    uint16_t entity_logical_address;    /**< DoIP entity address (NBO) */
    uint8_t  response_code;            /**< doip_routing_response_code_t */
    uint8_t  reserved[4];              /**< ISO reserved */
} doip_routing_activation_response_t;

/** Alive Check Request — zero-length payload */
/** Alive Check Response — zero-length payload */

/** Vehicle Identification Request — zero-length payload */
/** Vehicle Identification Request with EID */
typedef struct __attribute__((packed)) {
    uint8_t  eid[6];                /**< EID bytes */
} doip_vehicle_id_request_eid_t;

/** Vehicle Identification Request with VIN */
typedef struct __attribute__((packed)) {
    uint8_t  vin[17];               /**< VIN ASCII, no null terminator */
} doip_vehicle_id_request_vin_t;

/** Vehicle Announcement / Identification Response (ISO 13400-2, Table 22) */
typedef struct __attribute__((packed)) {
    uint8_t  vin[17];               /**< VIN ASCII */
    uint16_t logical_address;       /**< Entity logical address (NBO) */
    uint8_t  eid[6];                /**< Entity identification */
    uint8_t  gid[6];                /**< Group identification */
    uint8_t  further_action;        /**< Further action required */
    uint8_t  vin_gid_sync;          /**< VIN/GID sync status */
} doip_vehicle_announcement_t;

/** Entity Status Response (ISO 13400-2, Table 28) */
typedef struct __attribute__((packed)) {
    uint8_t  node_type;             /**< 0x00=gateway, 0x01=node */
    uint8_t  max_concurrent_sockets;
    uint8_t  currently_open_sockets;
    uint32_t max_data_size;         /**< NBO */
} doip_entity_status_response_t;

/** Power Mode Info Response (ISO 13400-2, Table 30) */
typedef struct __attribute__((packed)) {
    uint8_t  power_mode;            /**< 0x00=not ready, 0x01=ready, 0x02=not supported */
} doip_power_mode_response_t;

/** Diagnostic Message payload (ISO 13400-2, Table 31) */
typedef struct __attribute__((packed)) {
    uint16_t source_address;        /**< NBO */
    uint16_t target_address;        /**< NBO */
    /* user_data follows immediately — variable length */
} doip_diag_message_t;

/** Diagnostic Message Positive ACK (ISO 13400-2, Table 32) */
typedef struct __attribute__((packed)) {
    uint16_t source_address;        /**< NBO */
    uint16_t target_address;        /**< NBO */
    uint8_t  ack_code;              /**< doip_diag_ack_code_t */
    /* previous_diag_message may follow — optional */
} doip_diag_positive_ack_t;

/** Diagnostic Message Negative ACK (ISO 13400-2, Table 32) */
typedef struct __attribute__((packed)) {
    uint16_t source_address;        /**< NBO */
    uint16_t target_address;        /**< NBO */
    uint8_t  nack_code;             /**< doip_diag_nack_code_t */
    /* previous_diag_message may follow — optional */
} doip_diag_negative_ack_t;

/* =========================================================================
 * Generic Message Container
 * ========================================================================= */

/**
 * Full DoIP message: header + heap-allocated payload buffer.
 * The caller is responsible for freeing payload with doip_message_free().
 */
typedef struct {
    doip_payload_type_t type;
    uint32_t            payload_length;
    uint8_t            *payload;        /**< Heap-allocated; may be NULL for zero-length */
} doip_message_t;

/* =========================================================================
 * Return / Error Codes
 * ========================================================================= */

typedef enum {
    DOIP_OK                     =  0,
    DOIP_ERR_INVALID_ARG        = -1,
    DOIP_ERR_SOCKET             = -2,
    DOIP_ERR_BIND               = -3,
    DOIP_ERR_CONNECT            = -4,
    DOIP_ERR_SEND               = -5,
    DOIP_ERR_RECV               = -6,
    DOIP_ERR_TIMEOUT            = -7,
    DOIP_ERR_HEADER             = -8,   /**< Bad protocol version / inverse byte */
    DOIP_ERR_PAYLOAD_TYPE       = -9,   /**< Unknown or unexpected payload type */
    DOIP_ERR_PAYLOAD_LENGTH     = -10,  /**< Length field inconsistent with payload type */
    DOIP_ERR_NO_MEMORY          = -11,
    DOIP_ERR_CALLBACK_FULL      = -12,  /**< doip_max_callbacks reached */
    DOIP_ERR_CALLBACK_EXISTS    = -13,
    DOIP_ERR_CALLBACK_RESPONSE  = -14,  /**< Callback returned invalid response type */
    DOIP_ERR_NOT_INITIALIZED    = -15,
} doip_status_t;

/* =========================================================================
 * Payload Type Metadata  (for validation)
 * ========================================================================= */

/**
 * Static descriptor of a known payload type.
 * min_length == max_length == 0 means zero-length payload only.
 * max_length == UINT32_MAX means unbounded (variable).
 */
typedef struct {
    doip_payload_type_t type;
    const char         *name;
    uint32_t            min_length;     /**< Minimum valid payload bytes */
    uint32_t            max_length;     /**< Maximum valid payload bytes (UINT32_MAX = unbounded) */
    doip_payload_type_t expected_response; /**< 0x0000 if no directed response */
} doip_payload_descriptor_t;

/**
 * Look up the descriptor for a payload type.
 * @return Pointer to static descriptor, or NULL if unknown.
 */
const doip_payload_descriptor_t *doip_get_payload_descriptor(doip_payload_type_t type);

/**
 * Validate that a payload length is acceptable for its type.
 * @return DOIP_OK if valid, DOIP_ERR_PAYLOAD_LENGTH otherwise.
 */
doip_status_t doip_validate_payload_length(doip_payload_type_t type, uint32_t length);

/* =========================================================================
 * Header Encode / Decode Helpers
 * ========================================================================= */

/**
 * Encode a DoIP header into a caller-supplied 8-byte buffer (big-endian).
 * @param buf           Must be at least DOIP_HEADER_LENGTH bytes.
 * @param payload_type  Payload type identifier.
 * @param payload_len   Length of the payload that follows.
 */
void doip_encode_header(uint8_t *buf, doip_payload_type_t payload_type, uint32_t payload_len);

/**
 * Decode a DoIP header from an 8-byte big-endian buffer.
 * Validates protocol version and inverse byte.
 * @param buf   At least DOIP_HEADER_LENGTH bytes.
 * @param out   Populated on DOIP_OK.
 * @return DOIP_OK or DOIP_ERR_HEADER.
 */
doip_status_t doip_decode_header(const uint8_t *buf, doip_header_t *out);

/* =========================================================================
 * Message Lifecycle
 * ========================================================================= */

/**
 * Allocate and initialise a doip_message_t with a copy of the payload.
 * @param type    Payload type.
 * @param payload Raw payload bytes (may be NULL when length == 0).
 * @param length  Payload byte count.
 * @param out     Set to newly allocated message on DOIP_OK; caller frees with doip_message_free().
 */
doip_status_t doip_message_create(doip_payload_type_t type,
                                   const void         *payload,
                                   uint32_t            length,
                                   doip_message_t    **out);

/**
 * Free a message previously allocated by doip_message_create() or the
 * client/server receive paths.
 */
void doip_message_free(doip_message_t *msg);

/* =========================================================================
 * Client Context
 * ========================================================================= */

/** Opaque client context — allocate with doip_client_create(). */
typedef struct doip_client doip_client_t;

/**
 * Allocate a client context.
 * Does NOT open sockets; call doip_client_connect_tcp() / doip_client_init_udp()
 * to open the respective sockets.
 *
 * @param out   Set to the new context on DOIP_OK.
 * @return DOIP_OK or DOIP_ERR_NO_MEMORY.
 */
doip_status_t doip_client_create(doip_client_t **out);

/**
 * Release the client context and close all open sockets.
 */
void doip_client_destroy(doip_client_t *client);

/**
 * Open and connect the TCP diagnostic socket.
 *
 * @param client    Client context.
 * @param host      Server IP address string (IPv4 or IPv6).
 * @param port      Server port (typically DOIP_PORT).
 * @return DOIP_OK, DOIP_ERR_SOCKET, or DOIP_ERR_CONNECT.
 */
doip_status_t doip_client_connect_tcp(doip_client_t *client,
                                       const char    *host,
                                       uint16_t       port);

/**
 * Open the UDP socket for vehicle discovery.
 * Enables broadcast on the socket automatically.
 *
 * @param client    Client context.
 * @param port      Local bind port (0 for ephemeral).
 * @return DOIP_OK, DOIP_ERR_SOCKET, or DOIP_ERR_BIND.
 */
doip_status_t doip_client_init_udp(doip_client_t *client, uint16_t port);

/**
 * Close the TCP socket (routing deactivation is the caller's responsibility).
 */
void doip_client_disconnect_tcp(doip_client_t *client);

/**
 * Close the UDP socket.
 */
void doip_client_close_udp(doip_client_t *client);

/**
 * Return the TCP socket file descriptor (-1 if not connected).
 */
int doip_client_get_tcp_fd(const doip_client_t *client);

/**
 * Return the UDP socket file descriptor (-1 if not initialised).
 */
int doip_client_get_udp_fd(const doip_client_t *client);

/* -------------------------------------------------------------------------
 * Client Send API
 * ------------------------------------------------------------------------- */

/**
 * Send a pre-built DoIP message over TCP.
 * Validates the payload length against the payload type descriptor before sending.
 *
 * @param client    Connected TCP client.
 * @param msg       Message to transmit; payload_length and payload must be consistent.
 * @return DOIP_OK, DOIP_ERR_NOT_INITIALIZED, DOIP_ERR_PAYLOAD_LENGTH, or DOIP_ERR_SEND.
 */
doip_status_t doip_client_send_tcp(doip_client_t        *client,
                                    const doip_message_t *msg);

/**
 * Send a pre-built DoIP message as a UDP unicast datagram.
 *
 * @param client    Initialised UDP client.
 * @param msg       Message to transmit.
 * @param dest_ip   Destination IP address string.
 * @param dest_port Destination port.
 * @return DOIP_OK, DOIP_ERR_NOT_INITIALIZED, DOIP_ERR_PAYLOAD_LENGTH, or DOIP_ERR_SEND.
 */
doip_status_t doip_client_send_udp(doip_client_t        *client,
                                    const doip_message_t *msg,
                                    const char           *dest_ip,
                                    uint16_t              dest_port);

/**
 * Broadcast a Vehicle Identification Request over UDP.
 * Constructs and sends DOIP_PT_VEHICLE_ID_REQUEST with an empty payload.
 *
 * @param client    Initialised UDP client.
 * @param bcast_ip  Broadcast / multicast address string.
 * @param port      Destination port (typically DOIP_PORT).
 */
doip_status_t doip_client_send_vehicle_id_request(doip_client_t *client,
                                                   const char    *bcast_ip,
                                                   uint16_t       port);

/**
 * Send a Routing Activation Request over TCP.
 *
 * @param client          Connected TCP client.
 * @param source_address  Tester logical address (host byte order).
 * @param activation_type Typically 0x00 (default).
 */
doip_status_t doip_client_send_routing_activation(doip_client_t *client,
                                                   uint16_t       source_address,
                                                   uint8_t        activation_type);

/**
 * Send a diagnostic (UDS) message over TCP.
 *
 * @param client         Connected TCP client with active routing.
 * @param source_address Tester logical address (host byte order).
 * @param target_address ECU logical address (host byte order).
 * @param data           UDS payload bytes.
 * @param data_length    Length of data.
 */
doip_status_t doip_client_send_diag_message(doip_client_t *client,
                                             uint16_t       source_address,
                                             uint16_t       target_address,
                                             const uint8_t *data,
                                             uint32_t       data_length);

/**
 * Send an Alive Check Request over TCP.
 */
doip_status_t doip_client_send_alive_check(doip_client_t *client);

/* -------------------------------------------------------------------------
 * Client Receive API
 * ------------------------------------------------------------------------- */

/**
 * Receive one DoIP message from the TCP socket.
 * Allocates a doip_message_t; caller must call doip_message_free().
 *
 * @param client        Connected client.
 * @param timeout_ms    Receive timeout in milliseconds (0 = block indefinitely).
 * @param out           Set to received message on DOIP_OK.
 */
doip_status_t doip_client_recv_tcp(doip_client_t  *client,
                                    uint32_t        timeout_ms,
                                    doip_message_t **out);

/**
 * Receive one DoIP message from the UDP socket.
 * Allocates a doip_message_t; caller must call doip_message_free().
 *
 * @param client        Initialised UDP client.
 * @param timeout_ms    Receive timeout in milliseconds (0 = block indefinitely).
 * @param src_ip        If non-NULL, populated with sender's IP string (at least 46 bytes).
 * @param src_port      If non-NULL, populated with sender's port.
 * @param out           Set to received message on DOIP_OK.
 */
doip_status_t doip_client_recv_udp(doip_client_t  *client,
                                    uint32_t        timeout_ms,
                                    char           *src_ip,
                                    uint16_t       *src_port,
                                    doip_message_t **out);

/* =========================================================================
 * Server Context & Callback Interface
 * ========================================================================= */

/** Opaque server context — allocate with doip_server_create(). */
typedef struct doip_server doip_server_t;

/**
 * Server request handler callback type.
 *
 * Invoked by the server when a fully-validated DoIP message of the registered
 * payload type is received from a client.  The handler must:
 *   1. Process @p request.
 *   2. Populate @p response_out by calling doip_message_create() or by
 *      building a response message directly.
 *   3. Return DOIP_OK to have the server transmit @p *response_out, or any
 *      other status to have the server send a generic NACK / diagnostic NACK
 *      and discard the response.
 *
 * The server validates that @p *response_out has the payload type that is
 * listed as expected_response in the descriptor for the incoming message's
 * type.  If validation fails the server logs an error and sends a generic NACK.
 *
 * Ownership of @p *response_out is transferred to the server on DOIP_OK; the
 * server will call doip_message_free() after transmission.  Do NOT free it
 * inside the callback.
 *
 * @param server        The server context.
 * @param client_fd     File descriptor of the connected client (TCP) or -1 (UDP).
 * @param request       The incoming message (read-only; freed by server after callback returns).
 * @param response_out  Output: set to a heap-allocated response message.
 * @param user_data     Opaque pointer registered alongside the callback.
 * @return DOIP_OK on success, any doip_status_t on failure.
 */
typedef doip_status_t (*doip_handler_fn)(doip_server_t        *server,
                                          int                   client_fd,
                                          const doip_message_t *request,
                                          doip_message_t      **response_out,
                                          void                 *user_data);

/**
 * Allocate and initialise a server context.
 *
 * @param logical_address   DoIP entity logical address (host byte order).
 * @param out               Set to the new context on DOIP_OK.
 */
doip_status_t doip_server_create(uint16_t logical_address, doip_server_t **out);

/**
 * Release the server context and close all sockets.
 */
void doip_server_destroy(doip_server_t *server);

/**
 * Open and bind the TCP listening socket.
 *
 * @param server    Server context.
 * @param bind_ip   IP to bind (NULL or "" to bind INADDR_ANY).
 * @param port      Port to listen on (typically DOIP_PORT).
 * @return DOIP_OK, DOIP_ERR_SOCKET, or DOIP_ERR_BIND.
 */
doip_status_t doip_server_init_tcp(doip_server_t *server,
                                    const char    *bind_ip,
                                    uint16_t       port);

/**
 * Open and bind the UDP socket.
 * Enables broadcast and SO_REUSEADDR automatically.
 *
 * @param server    Server context.
 * @param bind_ip   IP to bind (NULL or "" for INADDR_ANY).
 * @param port      Port to bind (typically DOIP_PORT).
 * @return DOIP_OK, DOIP_ERR_SOCKET, or DOIP_ERR_BIND.
 */
doip_status_t doip_server_init_udp(doip_server_t *server,
                                    const char    *bind_ip,
                                    uint16_t       port);

/**
 * Register a handler callback for a specific payload type.
 *
 * Only one handler per payload type is supported.  Attempting to register a
 * second handler for the same type returns DOIP_ERR_CALLBACK_EXISTS.
 *
 * @param server        Server context.
 * @param payload_type  The payload type this handler services.
 * @param handler       Callback function pointer.
 * @param user_data     Opaque pointer forwarded to each handler invocation.
 * @return DOIP_OK, DOIP_ERR_CALLBACK_FULL, DOIP_ERR_CALLBACK_EXISTS, or DOIP_ERR_INVALID_ARG.
 */
doip_status_t doip_server_register_handler(doip_server_t      *server,
                                             doip_payload_type_t payload_type,
                                             doip_handler_fn     handler,
                                             void               *user_data);

/**
 * Deregister a previously registered handler.
 *
 * @param server        Server context.
 * @param payload_type  Payload type whose handler should be removed.
 * @return DOIP_OK, or DOIP_ERR_INVALID_ARG if no handler was registered.
 */
doip_status_t doip_server_deregister_handler(doip_server_t      *server,
                                               doip_payload_type_t payload_type);

/**
 * Process one pending event on the server (non-blocking).
 *
 * Uses select() internally to check for activity on the TCP listening socket,
 * all active TCP client sockets, and the UDP socket.  When data arrives:
 *   - Parses the DoIP header and payload.
 *   - Validates the message.
 *   - Dispatches to the registered handler (if any).
 *   - Transmits the response returned by the handler, or a NACK on error.
 *
 * Designed to be called repeatedly from an application event loop.
 *
 * @param server        Initialised server.
 * @param timeout_ms    select() timeout in milliseconds (0 = return immediately).
 * @return DOIP_OK (including the case where no data was ready),
 *         DOIP_ERR_NOT_INITIALIZED, or a socket error code.
 */
doip_status_t doip_server_poll(doip_server_t *server, uint32_t timeout_ms);

/**
 * Return the TCP listening socket fd (-1 if not initialised).
 */
int doip_server_get_tcp_fd(const doip_server_t *server);

/**
 * Return the UDP socket fd (-1 if not initialised).
 */
int doip_server_get_udp_fd(const doip_server_t *server);

/* =========================================================================
 * Utility: network byte-order helpers
 * ========================================================================= */

/** Host-to-network and network-to-host for 16-bit and 32-bit values
 *  (thin wrappers around htons/ntohs so callers don't need <arpa/inet.h>). */
uint16_t doip_hton16(uint16_t v);
uint16_t doip_ntoh16(uint16_t v);
uint32_t doip_hton32(uint32_t v);
uint32_t doip_ntoh32(uint32_t v);

#endif /* DOIP_H */
