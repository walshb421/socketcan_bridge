# Implementation Specification

## 1. Overview

This document defines the low-level implementation requirements for the **ash** bridge server MVP. It covers wire protocol, data structures, state machines, interface management, signal model, persistence format, and client library API.

---

## 2. Server Architecture

### 2.1 Process Model

- Single process, event-driven using `epoll` on Linux.
- All I/O (TCP accept, client sockets, SocketCAN sockets) is non-blocking and managed through a single epoll loop.
- No worker threads in MVP. All processing occurs on the main loop. A threaded concurrency model is listed as the long-term architectural intent but is deferred post-MVP; the single-threaded epoll approach is sufficient for MVP and reduces implementation complexity.

### 2.2 Startup

1. Parse configuration from command-line arguments and/or a config file.
2. Bind the configuration plane TCP listener on a configurable port (default: `4000`).
3. Load persisted network definitions from the configured storage directory (default: `/var/lib/ash/`).
4. Enter the epoll event loop.

### 2.3 Shutdown

- On `SIGTERM` or `SIGINT`, gracefully close all client sessions by sending `NOTIFY_SERVER_CLOSING` to each connected client, then release all interface attachments and flush any pending persistence writes before exiting.

---

## 3. Configuration Plane Protocol

### 3.1 Transport

- TCP, one persistent connection per client session.
- Server listens on a single port (default: `4000`).
- Connections are accepted and tracked as session objects.

### 3.2 Frame Format

All messages on the configuration plane use the following fixed-length header:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        Protocol Version       |          Message Type         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Payload Length                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field            | Size    | Description                                      |
|------------------|---------|--------------------------------------------------|
| Protocol Version | 2 bytes | Must be `0x0001` for this specification          |
| Message Type     | 2 bytes | Identifies the payload structure (see §3.4)      |
| Payload Length   | 4 bytes | Length of the payload following the header, in bytes. May be 0. |

All multi-byte integer fields are big-endian unless otherwise stated.

Header size: **8 bytes**.

Maximum payload length (MVP): **65535 bytes**. The Payload Length field is 4 bytes for future extensibility; implementations MUST reject any message with Payload Length > 65535 and return `ERR_PAYLOAD_TOO_LARGE`.

### 3.3 Framing Rules

- The server reads exactly 8 bytes to parse the header, then reads exactly `Payload Length` bytes for the payload.
- If `Protocol Version` does not match, the server sends `ERR_PROTOCOL_VERSION` and closes the connection.
- If `Message Type` is unknown, the server sends `ERR_UNKNOWN_MESSAGE_TYPE` and continues.
- If `Payload Length` exceeds the maximum, the server sends `ERR_PAYLOAD_TOO_LARGE` and closes the connection.
- Any message containing a name field where `Name Len` exceeds 64 is rejected with `ERR_NAME_TOO_LONG`. The connection remains open.

### 3.4 Message Type Registry

#### Client → Server

| Message Type | Value  | Description                          |
|--------------|--------|--------------------------------------|
| `SESSION_INIT`         | `0x0001` | Initiate a session               |
| `SESSION_KEEPALIVE`    | `0x0002` | Heartbeat                        |
| `SESSION_CLOSE`        | `0x0003` | Graceful disconnect              |
| `IFACE_LIST`           | `0x0010` | Request available network interfaces |
| `IFACE_ATTACH`         | `0x0011` | Attach to a CAN interface        |
| `IFACE_DETACH`         | `0x0012` | Detach from a CAN interface      |
| `IFACE_VCAN_CREATE`    | `0x0013` | Create a virtual CAN interface   |
| `IFACE_VCAN_DESTROY`   | `0x0014` | Destroy a virtual CAN interface  |
| `DEF_SIGNAL`           | `0x0020` | Define a signal                  |
| `DEF_PDU`              | `0x0021` | Define a PDU                     |
| `DEF_FRAME`            | `0x0022` | Define a frame                   |
| `DEF_DELETE`           | `0x0023` | Delete a definition by name      |
| `OWN_ACQUIRE`          | `0x0030` | Acquire ownership of a signal    |
| `OWN_RELEASE`          | `0x0031` | Release ownership of a signal    |
| `OWN_LOCK`             | `0x0032` | Lock a signal (prevent takeover) |
| `OWN_UNLOCK`           | `0x0033` | Unlock a signal                  |
| `CFG_SAVE`             | `0x0040` | Persist current definitions      |
| `CFG_LOAD`             | `0x0041` | Load definitions from storage    |

#### Server → Client

| Message Type | Value  | Description                                     |
|--------------|--------|-------------------------------------------------|
| `SESSION_INIT_ACK`     | `0x8001` | Session accepted                        |
| `SESSION_CLOSE_ACK`    | `0x8003` | Close acknowledged                      |
| `IFACE_LIST_RESP`      | `0x8010` | List of available interfaces            |
| `IFACE_ATTACH_ACK`     | `0x8011` | Interface attached, app socket details  |
| `IFACE_DETACH_ACK`     | `0x8012` | Interface detached                      |
| `IFACE_VCAN_ACK`       | `0x8013` | vcan created                            |
| `DEF_ACK`              | `0x8020` | Definition accepted                     |
| `OWN_ACK`              | `0x8030` | Ownership operation accepted            |
| `CFG_ACK`              | `0x8040` | Persistence operation complete          |
| `NOTIFY_OWN_REVOKED`   | `0x9001` | Ownership was revoked (unsolicited)     |
| `NOTIFY_IFACE_DOWN`    | `0x9002` | Interface went down (unsolicited)       |
| `NOTIFY_SERVER_CLOSING`| `0x9003` | Server is shutting down (unsolicited)   |
| `ERR`                  | `0xFFFF` | Error response                          |

**Notes:**
- Value `0x8002` is reserved. `SESSION_KEEPALIVE` has no acknowledgement.
- `IFACE_VCAN_ACK` (`0x8013`) is used to acknowledge both `IFACE_VCAN_CREATE` and `IFACE_VCAN_DESTROY`.

### 3.5 Error Payload

All `ERR` messages share this payload structure:

```
+------------------+------------------+
| Error Code (2B)  | Message (N bytes, UTF-8, not null-terminated) |
+------------------+------------------+
```

| Error Code | Name                       | Description                              |
|------------|----------------------------|------------------------------------------|
| `0x0001`   | `ERR_PROTOCOL_VERSION`     | Unsupported protocol version             |
| `0x0002`   | `ERR_UNKNOWN_MESSAGE_TYPE` | Unrecognized message type                |
| `0x0003`   | `ERR_PAYLOAD_TOO_LARGE`    | Payload exceeds maximum length           |
| `0x0004`   | `ERR_NOT_IN_SESSION`       | Operation requires an active session     |
| `0x0005`   | `ERR_ALREADY_IN_SESSION`   | SESSION_INIT sent on active session      |
| `0x0006`   | `ERR_NAME_TOO_LONG`        | Name field exceeds maximum length of 64 bytes |
| `0x0010`   | `ERR_IFACE_NOT_FOUND`      | Named interface does not exist           |
| `0x0011`   | `ERR_IFACE_ALREADY_ATTACHED` | Interface already attached to a session |
| `0x0012`   | `ERR_IFACE_ATTACH_FAILED`  | SocketCAN bind failed                    |
| `0x0020`   | `ERR_DEF_INVALID`          | Malformed definition payload             |
| `0x0021`   | `ERR_DEF_CONFLICT`         | Name already in use by a definition of a different type |
| `0x0022`   | `ERR_DEF_IN_USE`           | Definition cannot be deleted; referenced by another definition |
| `0x0030`   | `ERR_OWN_NOT_AVAILABLE`    | Signal owned or locked by another client |
| `0x0031`   | `ERR_OWN_NOT_HELD`         | Client does not hold ownership           |
| `0x0040`   | `ERR_CFG_IO`               | File I/O error during persistence        |
| `0x0041`   | `ERR_CFG_CHECKSUM`         | Checksum mismatch on load                |
| `0x0042`   | `ERR_CFG_CONFLICT`         | Loaded file contains definitions conflicting with in-memory state |

---

## 4. Session Lifecycle

### 4.1 Session Establishment

1. Client connects via TCP to the config plane port.
2. Client sends `SESSION_INIT`.
3. Server assigns a session ID (u32, server-generated, non-zero), creates session state, and responds with `SESSION_INIT_ACK`.
4. All subsequent messages on the connection are processed in the context of that session.

> **Sequential protocol:** The configuration plane is strictly request/response. The client MUST wait for an ACK or ERR response before sending the next request. Pipelining is not supported in the MVP.

**`SESSION_INIT` payload:**

```
+----------------------+------------------------------+
| Client Name Len (1B) | Client Name (N bytes, UTF-8) |
+----------------------+------------------------------+
```

**`SESSION_INIT_ACK` payload:**

```
+------------------+
| Session ID (4B)  |
+------------------+
```

### 4.2 Keep-Alive

- The server enforces a keep-alive timeout (default: `30 seconds`).
- Receiving any message from the client resets the timer, including `SESSION_KEEPALIVE`.
- `SESSION_KEEPALIVE` has no payload.
- On timeout expiry, the server releases all ownership held by the session and closes the TCP connection without sending a notification (connection drop is the signal).

### 4.3 Session Teardown

- Client sends `SESSION_CLOSE` (no payload).
- Server releases all ownership and detaches all interfaces owned by the session, then responds with `SESSION_CLOSE_ACK` (no payload) and closes the TCP connection.
- On unexpected disconnect (EOF or TCP error), the server performs the same cleanup silently.

---

## 5. Application Plane Protocol

### 5.1 Transport

- Per-interface TCP socket, bound by the server on a dynamically assigned port.
- The port is communicated to the client in `IFACE_ATTACH_ACK`.
- The client connects to this port to begin runtime interaction on that interface.
- Multiple clients may connect to the same application socket if they hold ownership of different signals on that interface.
- Connecting to an application plane socket implicitly subscribes the client to all inbound signal updates (`SIG_RX`) and raw frames (`FRAME_RX`) received on that interface. There is no explicit subscribe/unsubscribe message in the MVP.
- The application plane socket listener is created when the interface is attached and destroyed when the interface is detached or the owning session closes. All connected clients are disconnected when the socket is destroyed.
- Interface attachment is exclusive: only one session may attach a given interface at a time. Multiple clients may connect to the resulting application plane socket.

### 5.2 Frame Format

Application plane messages use a simplified 4-byte header:

```
+-----------------+-----------------+
| Message Type (2B) | Payload Len (2B) |
+-----------------+-----------------+
```

Maximum application payload: **4096 bytes**.

### 5.3 Message Types (Application Plane)

#### Client → Server

| Message Type | Value  | Description                     |
|--------------|--------|---------------------------------|
| `SIG_WRITE`  | `0x0001` | Write a signal value            |
| `SIG_READ`   | `0x0002` | Read current signal value       |
| `FRAME_TX`   | `0x0010` | Transmit a raw CAN frame        |

#### Server → Client

| Message Type | Value  | Description                     |
|--------------|--------|---------------------------------|
| `SIG_READ_RESP` | `0x8002` | Signal value response        |
| `SIG_RX`        | `0x8003` | Unsolicited inbound signal update |
| `FRAME_RX`      | `0x8010` | Inbound raw CAN frame           |
| `APP_ERR`       | `0xFFFF` | Error (2-byte code + message)   |

**Application Plane Error Codes (`APP_ERR`):**

| Code   | Name                  | Description                                              |
|--------|-----------------------|----------------------------------------------------------|
| `0x0001` | `ERR_SIG_NOT_FOUND` | Named signal does not exist                             |
| `0x0002` | `ERR_SIG_NOT_OWNED` | Client does not hold ownership of the signal            |
| `0x0003` | `ERR_SIG_NOT_MAPPED`| Signal exists but is not contained in any frame         |
| `0x0004` | `ERR_FRAME_NOT_FOUND`| CAN ID does not match any known frame definition       |
| `0x0005` | `ERR_DLC_INVALID`   | DLC value is invalid for the active CAN mode            |

### 5.4 Signal Write Payload (`SIG_WRITE`)

```
+-------------------+------------------+------------------+
| Name Len (1B)     | Name (N bytes)   | Value (8 bytes, IEEE 754 double) |
+-------------------+------------------+------------------+
```

- The client must hold ownership of the named signal.
- The server encodes the value into the signal's bit position within the PDU/frame and routes the frame to the interface corresponding to the application plane socket on which the `SIG_WRITE` was received.
- If the signal is not contained in any frame definition, the server returns `APP_ERR` with `ERR_SIG_NOT_MAPPED`.

### 5.5 Signal Read Payload (`SIG_READ` / `SIG_READ_RESP`)

Request:
```
+-------------------+------------------+
| Name Len (1B)     | Name (N bytes)   |
+-------------------+------------------+
```

Response:
```
+-------------------+------------------+------------------+
| Name Len (1B)     | Name (N bytes)   | Value (8B double)|
+-------------------+------------------+------------------+
```

- Reads do not require ownership. Any client connected to the application plane socket may read any signal associated with that interface.

### 5.6 Raw Frame Transmit/Receive (`FRAME_TX` / `FRAME_RX`)

```
+------------------+-------------------+------------------+------------------+
| CAN ID (4B)      | DLC (1B)          | Flags (1B)       | Data (0–64 bytes)|
+------------------+-------------------+------------------+------------------+
```

- CAN ID bit 31 set indicates extended (29-bit) ID.
- `Flags` bit 0: set to `1` for CAN FD frames. Bit 1: set to `1` to enable Bit Rate Switch (BRS). All other bits reserved, must be zero.
- For CAN 2.0 frames (`Flags` bit 0 = 0): DLC MUST be 0–8. Data length equals DLC.
- For CAN FD frames (`Flags` bit 0 = 1): DLC follows the CAN FD encoding:

| DLC | Data Length (bytes) |
|-----|---------------------|
| 0–8 | 0–8 (same as CAN 2.0) |
| 9   | 12                  |
| 10  | 16                  |
| 11  | 20                  |
| 12  | 24                  |
| 13  | 32                  |
| 14  | 48                  |
| 15  | 64                  |

---

## 6. Signal Model

### 6.1 Signal Definition (`DEF_SIGNAL` payload)

```
+------------------+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | Data Type (1B)   | Byte Order (1B)  |
+------------------+------------------+------------------+------------------+
| Bit Length (1B)  | Scale (8B dbl)   | Offset (8B dbl)  | Min (8B dbl)     |
+------------------+------------------+------------------+------------------+
| Max (8B dbl)     |
+------------------+
```

**Data Type values:**

| Value | Type         |
|-------|--------------|
| `0x01` | Unsigned integer |
| `0x02` | Signed integer   |
| `0x03` | IEEE 754 float (32-bit) |

**Byte Order values:**

| Value | Meaning       |
|-------|---------------|
| `0x01` | Little-endian (Intel) |
| `0x02` | Big-endian (Motorola)  |

Physical value = (raw × scale) + offset, clamped to [min, max].

Signal names are case-sensitive. Maximum name length: **64 bytes**. Names exceeding this are rejected with `ERR_NAME_TOO_LONG`.

The on_disconnect default value (`0x03`) reverts the signal to the physical value corresponding to raw = 0, which equals the signal's configured offset.

### 6.2 PDU Definition (`DEF_PDU` payload)

```
+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | PDU Length (1B, in bytes) |
+------------------+------------------+------------------+
| Signal Count (1B)|
+------------------+
```

Followed by `Signal Count` signal mapping entries:

```
+------------------+------------------+------------------+
| Sig Name Len (1B)| Sig Name (N bytes)| Start Bit (1B)  |
+------------------+------------------+------------------+
```

- `Start Bit` is the LSB position of the signal within the PDU, numbered from bit 0 at the LSB of byte 0.
- All referenced signal names must already be defined.
- `Signal Count` MUST NOT exceed **32**. The server rejects PDU definitions exceeding this limit with `ERR_DEF_INVALID`.

### 6.3 Frame Definition (`DEF_FRAME` payload)

```
+------------------+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | CAN ID (4B)      | ID Type (1B)     |
+------------------+------------------+------------------+------------------+
| DLC (1B)         | TX Period (2B, ms, 0=event-driven) | PDU Count (1B)   |
+------------------+------------------+------------------+------------------+
```

Followed by `PDU Count` PDU mapping entries:

```
+------------------+------------------+------------------+
| PDU Name Len (1B)| PDU Name (N bytes)| Byte Offset (1B)|
+------------------+------------------+------------------+
```

**ID Type values:**

| Value | Meaning    |
|-------|------------|
| `0x01` | Standard (11-bit) |
| `0x02` | Extended (29-bit) |

- `TX Period`: cyclic transmission interval in milliseconds. `0` means event-driven (transmit on signal write).
- All referenced PDU names must already be defined.
- `PDU Count` MUST NOT exceed **8**. The server rejects frame definitions exceeding this limit with `ERR_DEF_INVALID`.
- For CAN FD frames, `DLC` may be 0–15 per the CAN FD DLC encoding table in §5.6.

### 6.4 Definition Deletion (`DEF_DELETE` payload)

```
+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | Def Type (1B)    |
+------------------+------------------+------------------+
```

Def Type: `0x01` = signal, `0x02` = PDU, `0x03` = frame.

Deletion fails with `ERR_DEF_IN_USE` if another definition depends on the target (e.g., deleting a signal that is mapped in a PDU, or a PDU that is mapped in a frame).

---

## 7. Interface Management

### 7.1 Interface List (`IFACE_LIST` / `IFACE_LIST_RESP`)

Request has no payload.

Response:

```
+------------------+
| Interface Count (1B) |
+------------------+
```

Followed by `Interface Count` entries:

```
+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | State (1B)       |
+------------------+------------------+------------------+
```

State: `0x01` = available, `0x02` = attached by this session, `0x03` = attached by another session.

### 7.2 Interface Attach (`IFACE_ATTACH` payload)

```
+------------------+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | Mode (1B)        | Bitrate (4B, bps)|
+------------------+------------------+------------------+------------------+
| RX Filter Count (1B) |
+------------------+
```

Followed by `RX Filter Count` filter entries (may be 0 for promiscuous):

```
+------------------+------------------+
| CAN ID (4B)      | Mask (4B)        |
+------------------+------------------+
```

**Mode values:**

| Value | Meaning   |
|-------|-----------|
| `0x01` | CAN 2.0A (standard IDs only) |
| `0x02` | CAN 2.0B (standard and extended IDs) |
| `0x03` | CAN FD    |

**`IFACE_ATTACH_ACK` payload:**

```
+------------------+
| App Port (2B)    |
+------------------+
```

`App Port` is the TCP port the client should connect to for the application plane socket for this interface.

### 7.3 Interface Detach (`IFACE_DETACH` payload)

```
+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   |
+------------------+------------------+
```

### 7.4 Virtual CAN (`IFACE_VCAN_CREATE` / `IFACE_VCAN_DESTROY`)

Create payload:
```
+------------------+------------------+
| Name Len (1B)    | Name (N bytes, e.g. "vcan0") |
+------------------+------------------+
```

Destroy payload: same structure.

The server executes `ip link add/del` via the kernel `NETLINK_ROUTE` socket (not via shell). The created interface is brought up automatically. `IFACE_VCAN_ACK` has no payload on success.

---

## 8. Signal Ownership

### 8.1 Acquire (`OWN_ACQUIRE` payload)

```
+------------------+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   | On Disconnect (1B) |
+------------------+------------------+------------------+
```

**On Disconnect behavior:**

| Value | Behavior on session close/timeout        |
|-------|------------------------------------------|
| `0x01` | Stop transmitting the containing frame  |
| `0x02` | Continue at last value                  |
| `0x03` | Revert to default value (0 / offset)    |

- Ownership is NOT implicitly granted when a signal is defined via `DEF_SIGNAL`. The client must explicitly call `OWN_ACQUIRE` before it may write to the signal.
- If the signal is already owned by another client and not locked, ownership is transferred and `NOTIFY_OWN_REVOKED` is sent to the previous owner.
- If locked, the server responds with `ERR_OWN_NOT_AVAILABLE`.
- For cyclic frames (`tx_period_ms > 0`): cyclic transmission begins when the first `SIG_WRITE` is received for any signal in the frame. If all signals in a cyclic frame are released with `on_disconnect = stop`, the cyclic timerfd is disarmed. If any signal uses `on_disconnect = last` or `on_disconnect = default`, the cyclic timer continues firing after that client disconnects.

### 8.2 Release (`OWN_RELEASE` payload)

```
+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   |
+------------------+------------------+
```

### 8.3 Lock / Unlock (`OWN_LOCK` / `OWN_UNLOCK`)

Same payload as `OWN_RELEASE`. Locking requires the caller to currently hold ownership. A locked signal cannot be acquired by any other session.

### 8.4 `OWN_ACK` payload

```
+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   |
+------------------+------------------+
```

### 8.5 Definition Namespace

Signal, PDU, and frame definitions reside in a single global namespace shared across all sessions. Definitions persist across client disconnects — a session disconnect does NOT delete the definitions that session created.

Re-defining a name with the same type is allowed and overwrites the existing definition, provided no other definition currently depends on it (e.g., a signal mapped in a PDU cannot be redefined while that PDU exists). Re-defining a name with a different type (e.g., reusing a signal name for a PDU) is rejected with `ERR_DEF_CONFLICT`.

---

## 9. Persistence

### 9.1 Storage Directory

Default: `/var/lib/ash/`. Configurable via server startup argument `--storage-dir`.

One file per named configuration set, with the `.ashcfg` extension.

### 9.2 File Format

Files are binary with the following layout:

```
+------------------+------------------+
| Magic (4B)       | Format Version (2B) |
+------------------+------------------+
| Entry Count (2B) | CRC32 (4B, of all bytes following this field) |
+------------------+------------------+
```

Magic: `0x41534843` (`"ASHC"`).
Format Version: `0x0001`.

Followed by `Entry Count` definition entries, each:

```
+------------------+------------------+------------------+
| Entry Type (1B)  | Entry Length (2B)| Entry Payload (N bytes) |
+------------------+------------------+------------------+
```

Entry Type: `0x01` = signal, `0x02` = PDU, `0x03` = frame.
Entry Payload: the same binary payload format as the corresponding `DEF_*` configuration plane message.

### 9.3 Save (`CFG_SAVE` payload)

```
+------------------+------------------+
| Name Len (1B)    | Name (N bytes)   |
+------------------+------------------+
```

Saves all definitions currently held in the server's global definition store to a file `<name>.ashcfg` in the storage directory. This is a global save — all signals, PDUs, and frames from all sessions are included. Overwrites if the file exists. `CFG_ACK` has no payload on success.

### 9.4 Load (`CFG_LOAD` payload)

Same structure as `CFG_SAVE`. Loads and applies all definitions from `<name>.ashcfg`. The load is atomic:
- On checksum failure, responds with `ERR_CFG_CHECKSUM` and applies nothing.
- If any definition in the file conflicts with an existing in-memory definition (same name, different type), the server aborts the load, responds with `ERR_CFG_CONFLICT`, and applies nothing.
- On success, all definitions from the file are merged into the global store. `CFG_ACK` has no payload.

### 9.5 Auto-load on Startup

On startup, the server loads all `.ashcfg` files found in the storage directory into a shared definition namespace. These definitions are available to all sessions but not owned by any.

---

## 10. Internal Data Structures

### 10.1 Session

```c
typedef struct ash_session {
    uint32_t        id;
    int             config_fd;
    char            client_name[64];
    time_t          last_rx;
    ash_signal_ref  *owned_signals;   /* linked list */
    ash_iface_ref   *attached_ifaces; /* linked list of ash_iface_t owned by this session */
} ash_session_t;
```

### 10.2 Signal Definition

```c
typedef struct ash_signal_def {
    char        name[64];
    uint8_t     data_type;
    uint8_t     byte_order;
    uint8_t     bit_length;
    double      scale;
    double      offset;
    double      min;
    double      max;
    double      current_value;
    uint32_t    owner_session_id;   /* 0 = unowned */
    bool        locked;
    uint8_t     on_disconnect;
} ash_signal_def_t;
```

### 10.3 PDU Definition

```c
typedef struct ash_pdu_signal_map {
    char        signal_name[64];
    uint8_t     start_bit;
} ash_pdu_signal_map_t;

typedef struct ash_pdu_def {
    char                    name[64];
    uint8_t                 length;   /* bytes */
    uint8_t                 signal_count;
    ash_pdu_signal_map_t    signals[32];
} ash_pdu_def_t;
```

### 10.4 Frame Definition

```c
typedef struct ash_frame_pdu_map {
    char        pdu_name[64];
    uint8_t     byte_offset;
} ash_frame_pdu_map_t;

typedef struct ash_frame_def {
    char                    name[64];
    uint32_t                can_id;
    uint8_t                 id_type;
    uint8_t                 dlc;
    uint16_t                tx_period_ms;
    uint8_t                 pdu_count;
    ash_frame_pdu_map_t     pdus[8];
} ash_frame_def_t;
```

### 10.5 Interface State

```c
typedef struct ash_iface {
    char        name[16];
    int         can_fd;             /* SocketCAN socket fd */
    int         app_listen_fd;      /* application plane TCP listener fd */
    uint16_t    app_port;
    uint32_t    owner_session_id;   /* 0 = unattached */
    uint8_t     mode;
    uint32_t    bitrate;
} ash_iface_t;

typedef struct ash_cyclic_frame {
    ash_frame_def_t     *frame;
    ash_iface_t         *iface;
    int                  timerfd;
    uint8_t              data[64];  /* current frame data buffer */
} ash_cyclic_frame_t;
```

---

## 11. Encoding and Decoding

### 11.1 Signal Packing (Write Path)

1. Look up the signal by name in the definition store.
2. Compute raw value: `raw = (physical - offset) / scale`, clamped to the signal's representable range.
3. Look up which PDU contains this signal and the signal's `start_bit` and `bit_length`. If the signal is not mapped to any PDU, return `APP_ERR` with `ERR_SIG_NOT_MAPPED`.
4. Look up which frame contains that PDU and the PDU's `byte_offset`.
5. Pack `raw` into the frame's data bytes at the correct bit position, respecting `byte_order`.
6. If the frame is event-driven (`tx_period_ms == 0`), enqueue the frame for immediate transmission on the interface's SocketCAN socket.
7. If cyclic, update the frame's current data buffer; the cyclic timer handles transmission.

### 11.2 Signal Unpacking (Read Path)

1. Receive a raw CAN frame on a SocketCAN socket.
2. Match the CAN ID to a known frame definition.
3. For each PDU in the frame (at its `byte_offset`), extract each signal's bits at `start_bit` with `bit_length`.
4. Apply: `physical = (raw × scale) + offset`.
5. Update `current_value` on the signal definition.
6. Push a `SIG_RX` message to all application plane clients subscribed to that interface.

### 11.3 Cyclic Transmission

- A timerfd is created per cyclic frame the first time a `SIG_WRITE` is received for any signal in that frame.
- On timer expiry, the server transmits the frame's current data buffer on the interface corresponding to the application socket that delivered the first write.
- Timerfd events are handled in the main epoll loop.
- When all owning sessions of signals in a cyclic frame disconnect or release ownership with `on_disconnect = stop`, the timerfd is disarmed and the `ash_cyclic_frame_t` entry is removed.
- If any signal in the frame has `on_disconnect = last` or `on_disconnect = default`, the timerfd continues running after those sessions disconnect, using the last-written or default-computed data buffer.

---

## 12. Client Library API

### 12.1 C Library (`libash`)

```c
/* Connection */
ash_ctx_t *ash_connect(const char *host, uint16_t port, const char *client_name);
void       ash_disconnect(ash_ctx_t *ctx);
int        ash_keepalive(ash_ctx_t *ctx);

/* Interface management */
int ash_iface_list(ash_ctx_t *ctx, ash_iface_info_t *out, size_t max_count);
int ash_iface_attach(ash_ctx_t *ctx, const char *iface, uint8_t mode, uint32_t bitrate);
int ash_iface_detach(ash_ctx_t *ctx, const char *iface);
int ash_vcan_create(ash_ctx_t *ctx, const char *name);
int ash_vcan_destroy(ash_ctx_t *ctx, const char *name);

/* Definitions */
int ash_define_signal(ash_ctx_t *ctx, const ash_signal_def_t *def);
int ash_define_pdu(ash_ctx_t *ctx, const ash_pdu_def_t *def);
int ash_define_frame(ash_ctx_t *ctx, const ash_frame_def_t *def);
int ash_delete_def(ash_ctx_t *ctx, const char *name, uint8_t def_type);

/* Ownership */
int ash_acquire(ash_ctx_t *ctx, const char *signal, uint8_t on_disconnect);
int ash_release(ash_ctx_t *ctx, const char *signal);
int ash_lock(ash_ctx_t *ctx, const char *signal);
int ash_unlock(ash_ctx_t *ctx, const char *signal);

/* Runtime */
int    ash_write(ash_ctx_t *ctx, const char *signal, double value);
int    ash_read(ash_ctx_t *ctx, const char *signal, double *value_out);
int    ash_frame_tx(ash_ctx_t *ctx, const char *iface, uint32_t can_id, uint8_t dlc, const uint8_t *data);
int    ash_poll(ash_ctx_t *ctx, ash_event_t *event, int timeout_ms);

/* Persistence */
int ash_cfg_save(ash_ctx_t *ctx, const char *name);
int ash_cfg_load(ash_ctx_t *ctx, const char *name);
```

- `ash_connect` performs `SESSION_INIT` and returns an opaque context.
- `ash_poll` blocks up to `timeout_ms` and returns the next inbound event (signal RX, frame RX, notification, or error). Returns 0 on timeout, 1 on event, -1 on error.
- All functions return 0 on success, negative errno-compatible value on failure.

### 12.2 Python Library (`ash`)

The Python library wraps `libash` via `ctypes` or a native C extension and exposes a class-based API:

```python
class AshClient:
    def __init__(self, host: str, port: int = 4000, name: str = ""):
        ...

    def disconnect(self) -> None: ...

    def iface_list(self) -> list[IfaceInfo]: ...
    def iface_attach(self, iface: str, mode: str, bitrate: int) -> None: ...
    def iface_detach(self, iface: str) -> None: ...
    def vcan_create(self, name: str) -> None: ...
    def vcan_destroy(self, name: str) -> None: ...

    def define_signal(self, signal: SignalDef) -> None: ...
    def define_pdu(self, pdu: PduDef) -> None: ...
    def define_frame(self, frame: FrameDef) -> None: ...
    def delete_def(self, name: str, def_type: str) -> None: ...

    def acquire(self, signal: str, on_disconnect: str = "stop") -> None: ...
    def release(self, signal: str) -> None: ...
    def lock(self, signal: str) -> None: ...
    def unlock(self, signal: str) -> None: ...

    def write(self, signal: str, value: float) -> None: ...
    def read(self, signal: str) -> float: ...
    def frame_tx(self, iface: str, can_id: int, data: bytes) -> None: ...
    def poll(self, timeout: float = 0.0) -> AshEvent | None: ...

    def cfg_save(self, name: str) -> None: ...
    def cfg_load(self, name: str) -> None: ...
```

- Exceptions are raised on error using an `AshError` base class with a `code` attribute matching the protocol error codes.
- `on_disconnect` accepts string values `"stop"`, `"last"`, or `"default"`.
- `mode` accepts string values `"2.0A"`, `"2.0B"`, or `"FD"`.

---

## 13. Build and Deployment

### 13.1 Build System

- CMake, minimum version 3.16.
- Targets: `ash-server` (executable), `libash` (shared and static), `ash-python` (Python extension or ctypes wrapper).
- Compiler: GCC or Clang, C11, with `-Wall -Wextra -Werror` in Debug builds.

### 13.2 Dependencies

- Linux kernel ≥ 5.4 (for SocketCAN and timerfd support).
- No third-party runtime dependencies for `ash-server` or `libash`.
- Python ≥ 3.9 for the Python library.

### 13.3 Installation

- `ash-server` installs to `/usr/local/bin/ash-server`.
- `libash.so` installs to `/usr/local/lib/`.
- Headers install to `/usr/local/include/ash/`.
- A systemd unit file (`ash.service`) is provided to run the server as a daemon.
