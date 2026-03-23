# Concept

## Overview

**ash** is a bridge server that sits between automotive networks and the tools that interact with them. It exposes a scriptable TCP interface through which clients configure network interfaces, define signal and PDU layouts, and drive or observe traffic at runtime. The server manages the physical or virtual network adapters and handles all bus-level interaction on behalf of connected clients.

## System Components

### Bridge Server

The core component. A long-running Linux daemon written in C that:

- Manages CAN and Ethernet interfaces on the host
- Maintains signal, PDU, and frame definitions provided by clients
- Handles bus-level transmission and reception
- Coordinates access between multiple concurrent clients
- Persists network definitions to local storage

### Client Library

A reference client library (C and Python) that wraps the TCP protocol and provides an ergonomic API for:

- Connecting to the bridge server
- Configuring interfaces and defining signals
- Driving and reading signals at runtime
- Running diagnostic sessions

### Example Clients

Lightweight example programs demonstrating common use cases — signal monitoring, scripted drive sequences, UDS session initiation — implemented using the reference libraries.

## Protocol Architecture

The bridge uses two distinct protocol planes over TCP/UDP:

### Configuration Plane

A single persistent TCP connection per client, established at session start. All configuration messages flow over this socket using a DoIP-inspired fixed-header framing scheme: a header with a message type field that implies the payload structure and length. This is the only socket required to exist before any network interaction begins.

### Application Plane

Per-interface TCP sockets, created dynamically as part of interface configuration. (UDP application sockets are deferred to post-MVP.) Runtime signal drive, signal reads, frame injection, and diagnostic requests flow over these sockets. A client may have multiple application sockets active simultaneously — one per configured interface or functional group.

## Signal Model

The bridge organizes network data in three layers:

- **Signal** — a named, typed value with encoding and scaling metadata. The atomic unit of configuration and runtime interaction.
- **PDU** — a structured payload containing one or more signals, mapped to specific bit positions within the PDU bytes.
- **Frame** — a bus-level message (CAN frame or Ethernet frame) that carries one or more PDUs.

Clients define these layers top-down or bottom-up via the configuration plane. The server uses these definitions to encode outgoing data and decode incoming data automatically. Clients interact with signals by name at the application plane without needing to manage bit-level encoding themselves.

## Interface Management

### CAN

CAN interfaces are managed via the Linux SocketCAN subsystem. The server can:

- Enumerate available interfaces (physical and virtual)
- Attach to and configure an interface (baud rate, CAN 2.0A/2.0B/FD mode, filters)
- Create and destroy virtual CAN interfaces (vcan)
- Transmit and receive frames, demultiplexing to PDUs and signals based on configured definitions

### Ethernet

Ethernet interfaces are managed as standard Linux network interfaces. The server can attach to a host Ethernet adapter and exchange frames at the IP/UDP/TCP layer. Automotive Ethernet connections to physical automotive networks (e.g. 100BASE-T1) are handled by external media converters — the server sees only a standard Linux Ethernet interface.

## Session and Ownership Model

Multiple clients may connect simultaneously. Each client has a session on the configuration plane.

Signal ownership is exclusive: only one client may drive a given signal at a time. A client acquires ownership when it configures or takes control of a signal. Ownership can be explicitly released or locked to prevent other clients from modifying it.

All clients must maintain a keep-alive heartbeat. If the heartbeat lapses, all ownership held by the session is released and the connection is closed. On disconnect, all ownership held by a client is released.

Per-signal disconnect behavior is configurable: continue at last value, stop transmitting, or fall back to a designated default value.

## Persistence

Signal and network definitions are written to local files on explicit save command. These act as non-volatile memory — the server can reload known configurations on restart. Configuration files include checksums validated on read and are designed to be portable between devices.

## Diagnostics

> **Post-MVP:** Diagnostics support is deferred. The following describes the intended post-MVP behavior.

The bridge supports UDS transport over CAN (via ISO-TP) and over Ethernet (via DoIP). It does not interpret or construct UDS content — the client provides raw UDS payloads and the bridge handles framing, transport, and routing. The bridge can act as a DoIP client (issuing requests on behalf of a connected client) or as a DoIP server (receiving requests from external diagnostic tools and routing them to the appropriate target).

## MVP Scope

The MVP covers enough to demonstrate end-to-end signal configuration and runtime interaction over a CAN interface:

1. TCP configuration server with framed protocol
2. Session establishment and keep-alive
3. CAN interface attach, configure, and filter
4. Signal, PDU, and frame definition via the configuration plane
5. Signal drive and read over the application plane
6. Signal ownership and basic ACL
7. Configuration persistence to local files
8. Reference client library in C and Python with basic examples

DoIP, Ethernet interface management, SecOC, and curve/equation drive are deferred to post-MVP.
