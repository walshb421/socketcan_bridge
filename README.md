# SocketCAN Bridge 

This utility aims to be lightweight bridge that allows socketcan devices to be controlled over UDP, TCP and Websockets

## Usage

## Features

- **Server**: Listens on port 8080 and echoes back any messages received
- **Client**: Connects to the server and sends user input messages
- Clean error handling and resource cleanup

## Compilation

```bash
make
```

This creates two executables: `tcp_server` and `tcp_client`

## Usage

### Running the Server

In one terminal:
```bash
./tcp_server
```

The server will start listening on port 8080.

### Running the Client

In another terminal:
```bash
./tcp_client
```

Or connect to a remote server:
```bash
./tcp_client <server_ip>
```

Type messages and press Enter. The server will echo them back. Press Ctrl+D to quit.

## Example Session

**Server terminal:**
```
Server listening on port 8080...
Client connected from 127.0.0.1:54321
Received: Hello, server!
Received: This is a test
Client disconnected
```

**Client terminal:**
```
Connected to server at 127.0.0.1:8080
Type messages (Ctrl+D to quit):
> Hello, server!
Echo: Hello, server!
> This is a test
Echo: This is a test
> 
Closing connection...
```

## Key Socket Programming Concepts

1. **socket()** - Creates a socket endpoint
2. **bind()** - Binds socket to an address/port (server only)
3. **listen()** - Marks socket as passive, ready to accept connections (server only)
4. **accept()** - Accepts incoming connection (server only)
5. **connect()** - Initiates connection to server (client only)
6. **read()/write()** - Send and receive data
7. **close()** - Closes the socket

## Cleanup

```bash
make clean
```
