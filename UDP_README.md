# Simple UDP Server and Client in C

A basic UDP echo server and client implementation demonstrating datagram socket programming in C.

## UDP vs TCP Key Differences

- **Connectionless**: No connection establishment (no `listen()`, `accept()`, or `connect()`)
- **Unreliable**: No guarantee of delivery, ordering, or duplicate protection
- **Message-based**: Preserves message boundaries (each send is a discrete datagram)
- **Faster**: Lower overhead, no connection setup/teardown

## Features

- **Server**: Listens on port 8080 and echoes back datagrams from any client
- **Client**: Sends datagrams to the server and receives responses
- Demonstrates `sendto()` and `recvfrom()` for datagram communication

## Compilation

```bash
make -f udp_Makefile
```

This creates two executables: `udp_server` and `udp_client`

## Usage

### Running the Server

In one terminal:
```bash
./udp_server
```

The server will start listening on port 8080. Unlike TCP, it doesn't need to accept connections.

### Running the Client

In another terminal:
```bash
./udp_client
```

Or send to a remote server:
```bash
./udp_client <server_ip>
```

Type messages and press Enter. The server will echo them back. Press Ctrl+D to quit.

## Example Session

**Server terminal:**
```
UDP Server listening on port 8080...
Received from 127.0.0.1:54321 - Hello, UDP server!
Echoed back to client
Received from 127.0.0.1:54321 - Testing datagrams
Echoed back to client
```

**Client terminal:**
```
UDP Client ready to send to 127.0.0.1:8080
Type messages (Ctrl+D to quit):
> Hello, UDP server!
Echo: Hello, UDP server!
> Testing datagrams
Echo: Testing datagrams
> 
Closing connection...
```

## Key UDP Socket Programming Concepts

1. **socket()** - Creates a UDP socket with `SOCK_DGRAM` (not `SOCK_STREAM`)
2. **bind()** - Binds socket to an address/port (server only)
3. **sendto()** - Sends a datagram to a specific address
4. **recvfrom()** - Receives a datagram and stores sender's address
5. **close()** - Closes the socket

### Notable Differences from TCP

- **No `listen()` or `accept()`**: Server just starts receiving with `recvfrom()`
- **No `connect()` needed**: Client sends directly with `sendto()`
- **Each message is independent**: No stream of bytes, discrete datagrams
- **Server can handle multiple clients**: No need to track connections

## Use Cases for UDP

- Real-time applications (gaming, VoIP, video streaming)
- DNS queries
- DHCP
- IoT sensor data
- Situations where occasional packet loss is acceptable

## Cleanup

```bash
make -f udp_Makefile clean
```
