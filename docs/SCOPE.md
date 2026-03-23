# Project Scope

## Overview

This project is an **automotive network emulation and simulation bridge** — a scriptable, client-configured framework for interacting with, simulating, and testing automotive networks.

## Definitions

- **Signal** — a named, typed value carried within a PDU; has encoding, scaling, and optionally CRC or SecOC attributes
- **PDU** — Protocol Data Unit; a structured payload mapped to one or more signals, carried within a frame
- **Frame** — a raw bus-level message (CAN frame, Ethernet frame) that carries one or more PDUs
- **Interface** — a physical or virtual network adapter managed by the bridge (CAN interface via SocketCAN, or Ethernet interface)
- **Client** — a process that connects to the bridge over TCP to configure interfaces, define signals, and interact with the network at runtime

## Goals

- **Scriptability** — the primary goal; all behavior should be drivable via scripts
- **Signal and PDU configuration** — define, map, and manipulate signals and PDUs at runtime
- **CRC and SecOC support** — configure signal-level security and integrity features
- **Client-driven configuration** — all network definitions, interface layouts, and authentication information are provided by the client over the TCP interface; nothing is hardcoded server-side

## Primary Use Cases

### 1. Simulation and Emulation
- Manipulate runtime network data
- Perform partial integration tests against real or virtual ECUs
- Drive signals locally using curves, equations, or scripted sequences

### 2. Testing
- Run UDS (Unified Diagnostic Services) over CAN or DoIP
- Forward diagnostic or runtime data between interfaces
- Drive and observe signals in a scriptable manner

## Supported Interfaces

- **CAN** — CAN 2.0A (11-bit), CAN 2.0B (29-bit), and CAN-FD via the Linux SocketCAN subsystem; both physical hardware and virtual (vcan) interfaces are supported
- **Ethernet** — standard Linux Ethernet interfaces; may connect to physical automotive networks via a media converter (e.g. 100BASE-T1 to standard Ethernet); the bridge has no knowledge of or responsibility for the physical media layer

## Architectural Parameters

- **Target OS: Linux** — the bridge server requires Linux; SocketCAN and Ethernet-based interfaces both depend on Linux kernel interfaces and are not expected to be ported, though this could theoretically change in the future
- **Client OS** — clients may run on any OS; however, any interface type that depends on Linux (SocketCAN, raw Ethernet) is out of scope for non-Linux clients initially
- **Implementation language** — the bridge server is implemented in C; example clients will be provided in C and Python
- **Concurrency model** — threaded; the server manages interfaces and sessions using threads
- **Socket architecture** — a single persistent TCP socket acts as the main configuration server; additional TCP and UDP sockets are spawned dynamically based on the interfaces the client configures; see Protocol Message Categories for the distinction between the configuration plane and the application plane
- **Protocol framing** — the TCP configuration protocol uses a DoIP-inspired message framing scheme: a fixed header containing a message type field, which implies the expected payload length and parameter structure; all configuration and application-layer interactions use this format
- **Persistent storage** — the bridge persists signal and network definitions to local files acting as non-volatile memory; configurations are modular so they are portable between devices; files include checksums that are validated on read; whether file-level encryption will be supported is deferred

## Client Session Model

- **Multiple clients** — the server supports multiple simultaneous client connections
- **Signal ownership** — only one client may drive a given signal at a time; ownership is acquired when a client configures or takes control of a signal
- **Configuration persistence** — signal and network configuration persists across client disconnections; a reconnecting or new client may overwrite existing configuration
- **Ownership control** — clients may explicitly release ownership of a signal or block other clients from modifying it while connected
- **Disconnect and timeout behavior** — when a client disconnects or times out, all signal ownership held by that client is released, making those signals available to other clients
- **Keep-alive mechanism** — clients that hold a lock on a signal must maintain it via an explicit keep-alive; if the keep-alive lapses, the lock is released automatically; this prevents deadlocks where a crashed or stalled client indefinitely blocks a resource
- **Per-signal disconnect behavior** — how a signal behaves on client disconnect or timeout is configurable per signal at the application layer:
  - Continue transmitting at the last known value indefinitely
  - Stop transmitting when the driving client stops providing updates
  - Continue transmitting but transition to a designated fallback value

## Trust and Security

There are two distinct layers of trust and security in this system:

### Signal Safety and ACL
- **Client-configurable safety controls** — clients may define signal-level safety behaviors such as interlocks and range limits as part of their configuration
- **Fault injection override** — clients may explicitly disable safety controls to perform fault injection; this is an intentional capability, not a bypass
- **Safety through ownership model** — the combination of signal ACLs, explicit locking, and the keep-alive mechanism provides a baseline level of safety validation suitable for use on real automotive networks; this is the primary safety mechanism in the initial scope

### Client Trust and Transport Security
- **Initial deployment model** — the bridge is initially intended for use within a trusted compute cluster (same device, or a small local cluster of devices such as Raspberry Pis or similar); in this model, no client authentication or transport encryption is required
- **TLS not supported initially** — the TCP interface will not use TLS in the initial scope, but the design must not preclude adding it; it is an explicit future requirement
- **Future client trust models** — as the bridge is extended to support more ad hoc clients (e.g., Windows clients, or implementations of APIs such as Vector FDX), a more formal client trust and authentication model will need to be defined; the scope and mechanism for this is deferred
- **Client-provided credentials** — authentication information used at the automotive network layer (e.g., SecOC keys, UDS security access seeds/keys) is supplied by the client and is not validated by the bridge itself

## Protocol Message Categories

All configuration messages flow over the primary TCP configuration socket. Runtime application interaction flows over the per-interface TCP/UDP sockets spawned at configuration time. These are two distinct protocol planes.

### Configuration Plane (Primary TCP Socket)

- **Session and Ownership** — client session establishment, signal and resource locking, lock release, keep-alive heartbeat, timeout negotiation
- **Interface Discovery** — enumerate available CAN and Ethernet interfaces present on the host
- **CAN Interface Configuration** — attach/detach interfaces, set baud rate, select CAN 2.0A/2.0B or CAN-FD mode, configure hardware and software filters, create and destroy virtual CAN (vcan) interfaces
- **Ethernet Interface Configuration** — attach to host Ethernet interfaces at runtime, configure IP address assignment (static or DHCP), define VLANs
- **Signal and PDU Configuration** — define signal types, scaling, and encoding; map signals to frames and PDUs; configure CRC and SecOC per signal; assign signals to runtime sockets; this is the largest category
- **DoIP Configuration** — configure the bridge as a DoIP client (how requests are issued and responses consumed) or as a DoIP server (how incoming requests are matched and responses generated); includes routing activation, logical addressing, and entity announcement behavior
- **Runtime Socket Management** — create, configure, and tear down the per-interface UDP/TCP sockets over which application interaction occurs

### Application Plane (Per-Interface Runtime Sockets)

- **Signal Drive** — write a value to a signal being transmitted
- **Signal Read / Subscribe** — read the current value of a signal or subscribe to updates
- **Frame Drive** — write a raw frame directly, bypassing signal abstraction
- **Diagnostic Request/Response** — send UDS requests and receive responses over CAN or DoIP
- **Signal Curve / Equation Drive** — configure a signal to be driven autonomously by a predefined curve, equation, or replay sequence
- **Fault Injection** — override or suppress signal values, disable interlocks, inject errors into frames

## Supported Standards and Protocol Versions

- **DoIP** — ISO 13400-2; the bridge implements DoIP transport including routing activation, logical addressing, and entity announcement
- **UDS** — the bridge provides transport of UDS payloads over CAN (ISO-TP) and DoIP, but does not interpret or construct UDS content; the client is responsible for building all UDS requests and interpreting all responses; the bridge is UDS-transparent
- **CAN** — CAN 2.0A (11-bit identifier), CAN 2.0B (29-bit identifier), and CAN-FD via the Linux SocketCAN subsystem
- **SecOC** — signal-level SecOC configuration is supported; the specific profile and algorithm are client-defined
- **ISO-TP** — required for UDS over CAN; supported as a transport layer, not exposed as a standalone feature

## Out-of-Scope Interfaces

The bridge supports CAN and Ethernet only. The following are explicitly out of scope:

- LIN
- FlexRay
- Ethernet AVB / TSN as a distinct protocol layer
- Any other automotive bus or network type not listed above

## Limitations and Non-Goals

- **No GUI** — initial scope covers the server and its TCP interface only; a frontend is explicitly out of scope at this stage
- **No ECU flashing/programming as a first-class feature** — the bridge will not implement flashing logic itself; however, it will support full UDS sessions and can act as a transparent DoIP-to-CAN gateway, meaning an external diagnostic tool may perform flashing through the bridge
- **No network definition file parsing** — the bridge does not parse any network definition formats (ARXML, DBC, CDD, ODX, etc.) directly; the client is responsible for extracting signal definitions, scaling functions, and any other necessary type information from whatever source it uses — whether a file format or manual definition — and providing that data to the bridge via the TCP interface
