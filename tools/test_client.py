#!/usr/bin/env python3
"""
ash protocol test client — exercises all PR #25 test cases.

Usage:
    ./tools/test_client.py [--host HOST] [--port PORT] [--test N]

Default host/port: 127.0.0.1:4000

Tests 1-4 run automatically.  Test 5 (graceful shutdown) requires you to
stop the server manually while the client is connected, so it is skipped in
a full run unless --test 5 is given explicitly.
"""

import argparse
import socket
import struct
import sys

# ── Wire constants (mirrors include/ash/proto.h) ──────────────────────────────

PROTO_VERSION           = 0x0001
PROTO_HEADER_SIZE       = 8
PROTO_MAX_PAYLOAD       = 65535

MSG_SESSION_INIT        = 0x0001
MSG_ERR                 = 0xFFFF
MSG_NOTIFY_SERVER_CLOSE = 0x9003

ERR_PROTOCOL_VERSION     = 0x0001
ERR_UNKNOWN_MESSAGE_TYPE = 0x0002
ERR_PAYLOAD_TOO_LARGE    = 0x0003

# ── Frame helpers ─────────────────────────────────────────────────────────────

def make_frame(version, msg_type, payload=b''):
    """Return a complete wire frame: 8-byte header + payload."""
    header = struct.pack('>HHI', version, msg_type, len(payload))
    return header + payload


def recv_frame(sock):
    """Read one complete frame from *sock*.
    Returns (version, msg_type, payload) or None on EOF/error."""
    raw = _recv_exact(sock, PROTO_HEADER_SIZE)
    if raw is None:
        return None
    version, msg_type, payload_len = struct.unpack('>HHI', raw)
    payload = b''
    if payload_len:
        payload = _recv_exact(sock, payload_len)
        if payload is None:
            return None
    return version, msg_type, payload


def _recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def decode_err(payload):
    """Parse ERR payload: big-endian u16 err_code + optional message string."""
    if len(payload) < 2:
        return None, b''
    code = struct.unpack('>H', payload[:2])[0]
    return code, payload[2:]


# ── Pretty-printers ───────────────────────────────────────────────────────────

MSG_NAMES = {
    0x0001: 'SESSION_INIT',
    0x8001: 'SESSION_INIT_ACK',
    0x9003: 'NOTIFY_SERVER_CLOSE',
    0xFFFF: 'MSG_ERR',
}

ERR_NAMES = {
    0x0001: 'ERR_PROTOCOL_VERSION',
    0x0002: 'ERR_UNKNOWN_MESSAGE_TYPE',
    0x0003: 'ERR_PAYLOAD_TOO_LARGE',
}

def fmt_type(t):
    return MSG_NAMES.get(t, f'0x{t:04X}')

def fmt_err(c):
    return ERR_NAMES.get(c, f'0x{c:04X}')

def print_response(frame):
    if frame is None:
        print('  response: [none — connection closed]')
        return
    version, msg_type, payload = frame
    print(f'  response: version=0x{version:04X}  type={fmt_type(msg_type)}'
          f'  payload_len={len(payload)}')
    if msg_type == MSG_ERR:
        code, msg = decode_err(payload)
        print(f'            err_code={fmt_err(code)}'
              + (f'  msg={msg.decode(errors="replace")!r}' if msg else ''))


def check(cond, label):
    print(f'  {"✓" if cond else "✗"} {label}')
    return cond


# ── Individual test cases ─────────────────────────────────────────────────────

def test_valid_session_init(host, port):
    """
    Test 1 — Valid SESSION_INIT (version=0x0001, type=0x0001)

    No handlers are registered in the current build, so proto_dispatch() falls
    through to the default path and returns ERR_UNKNOWN_MESSAGE_TYPE while
    keeping the connection open.  The test verifies the response code and that
    the connection is still usable after the error reply.
    """
    print('\n[1] Valid SESSION_INIT — expect ERR_UNKNOWN_MESSAGE_TYPE, connection kept open')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_INIT))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_UNKNOWN_MESSAGE_TYPE,
              f'err_code is ERR_UNKNOWN_MESSAGE_TYPE (got {fmt_err(code)})')

        # Verify connection is still open by sending a second frame.
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_INIT))
        frame2 = recv_frame(s)
        check(frame2 is not None,
              'connection stayed open (second frame got a response)')


def test_bad_version(host, port):
    """
    Test 2 — Frame with version != 0x0001

    proto_validate_header() should reply ERR_PROTOCOL_VERSION (0x0001) and
    set close_conn=1, causing the server to close the connection.
    """
    print('\n[2] Bad protocol version (0x00FF) — expect ERR_PROTOCOL_VERSION, connection closed')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_frame(0x00FF, MSG_SESSION_INIT))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PROTOCOL_VERSION,
              f'err_code is ERR_PROTOCOL_VERSION (got {fmt_err(code)})')

        # Connection should be closed — a subsequent read should return EOF.
        s.settimeout(1)
        try:
            tail = s.recv(8)
            check(tail == b'', 'connection closed by server after ERR')
        except socket.timeout:
            check(False, 'connection closed by server (timed out — still open?)')


def test_payload_too_large(host, port):
    """
    Test 3 — payload_len > PROTO_MAX_PAYLOAD (65535)

    The server calls proto_read_frame() before proto_validate_header(), so it
    will malloc and recv the full declared payload before validating.  We must
    send the complete 65536-byte payload or proto_read_exact() will block.

    Expected: ERR_PAYLOAD_TOO_LARGE (0x0003), connection closed.
    """
    oversized = PROTO_MAX_PAYLOAD + 1   # 65536
    print(f'\n[3] Oversized payload (payload_len={oversized}) — expect ERR_PAYLOAD_TOO_LARGE, connection closed')
    print( '    (sending full payload so the server can advance to validation)')
    with socket.create_connection((host, port), timeout=10) as s:
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_INIT, b'\x00' * oversized))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PAYLOAD_TOO_LARGE,
              f'err_code is ERR_PAYLOAD_TOO_LARGE (got {fmt_err(code)})')

        s.settimeout(1)
        try:
            tail = s.recv(8)
            check(tail == b'', 'connection closed by server after ERR')
        except socket.timeout:
            check(False, 'connection closed by server (timed out — still open?)')


def test_unknown_type(host, port):
    """
    Test 4 — Unregistered message type (0x7FFF)

    proto_dispatch() finds no handler and sends ERR_UNKNOWN_MESSAGE_TYPE
    (0x0002).  The connection must remain open.
    """
    unknown = 0x7FFF
    print(f'\n[4] Unknown message type (0x{unknown:04X}) — expect ERR_UNKNOWN_MESSAGE_TYPE, connection kept open')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_frame(PROTO_VERSION, unknown))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_UNKNOWN_MESSAGE_TYPE,
              f'err_code is ERR_UNKNOWN_MESSAGE_TYPE (got {fmt_err(code)})')

        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_INIT))
        frame2 = recv_frame(s)
        check(frame2 is not None,
              'connection stayed open (second frame got a response)')


def test_server_close(host, port):
    """
    Test 5 — Graceful server shutdown

    Connect, then stop the server (Ctrl-C or SIGTERM).  The server's
    server_destroy() sends a wire-framed NOTIFY_SERVER_CLOSE (0x9003) to every
    connected client before closing their fds.

    This test waits up to 60 seconds for the frame.  Press Ctrl-C here to skip.
    """
    print('\n[5] Graceful shutdown — connect and stop the server (Ctrl-C on server)')
    print('    Waiting up to 60 s for NOTIFY_SERVER_CLOSE (0x9003) … (Ctrl-C here to skip)')
    try:
        with socket.create_connection((host, port), timeout=60) as s:
            s.settimeout(60)
            frame = recv_frame(s)
            print_response(frame)
            if not check(frame is not None, 'received a frame before connection dropped'):
                return
            _, msg_type, _ = frame
            check(msg_type == MSG_NOTIFY_SERVER_CLOSE,
                  f'received NOTIFY_SERVER_CLOSE (got {fmt_type(msg_type)})')
    except KeyboardInterrupt:
        print('  [skipped]')


# ── Entry point ───────────────────────────────────────────────────────────────

TESTS = [
    test_valid_session_init,
    test_bad_version,
    test_payload_too_large,
    test_unknown_type,
    test_server_close,
]


def main():
    parser = argparse.ArgumentParser(
        description='ash protocol test client (PR #25 / issue #5)')
    parser.add_argument('--host', default='127.0.0.1',
                        help='server host (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=4000,
                        help='server port (default: 4000)')
    parser.add_argument('--test', type=int, choices=range(1, 6), metavar='N',
                        help='run only test N (1-5)')
    args = parser.parse_args()

    print(f'ash test client — {args.host}:{args.port}')

    def run(fn):
        try:
            fn(args.host, args.port)
        except ConnectionRefusedError:
            print(f'  error: connection refused — is ash-server running on'
                  f' port {args.port}?', file=sys.stderr)
            sys.exit(1)

    if args.test:
        run(TESTS[args.test - 1])
    else:
        # Run tests 1-4 automatically; test 5 needs manual server intervention.
        for fn in TESTS[:-1]:
            run(fn)
        print('\n[5] Graceful shutdown — run with --test 5 to execute interactively')

    print()


if __name__ == '__main__':
    main()
