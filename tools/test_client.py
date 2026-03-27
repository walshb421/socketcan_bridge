#!/usr/bin/env python3
"""
ash protocol test client — exercises wire protocol and session lifecycle.

Usage:
    ./tools/test_client.py [--host HOST] [--port PORT] [--test N]

Default host/port: 127.0.0.1:4000

Tests 1-7 run automatically.
Test 8 (graceful server shutdown) requires manually stopping the server and
must be requested with --test 8.
Test 9 (keep-alive timeout) waits 30+ seconds and must be requested with --test 9.
"""

import argparse
import socket
import struct
import sys
import time

# ── Wire constants (mirrors include/ash/proto.h) ──────────────────────────────

PROTO_VERSION           = 0x0001
PROTO_HEADER_SIZE       = 8
PROTO_MAX_PAYLOAD       = 65535

MSG_SESSION_INIT        = 0x0001
MSG_SESSION_KEEPALIVE   = 0x0002
MSG_SESSION_CLOSE       = 0x0003
MSG_SESSION_INIT_ACK    = 0x8001
MSG_SESSION_CLOSE_ACK   = 0x8003
MSG_ERR                 = 0xFFFF
MSG_NOTIFY_SERVER_CLOSE = 0x9003

ERR_PROTOCOL_VERSION     = 0x0001
ERR_UNKNOWN_MESSAGE_TYPE = 0x0002
ERR_PAYLOAD_TOO_LARGE    = 0x0003
ERR_NOT_IN_SESSION       = 0x0004
ERR_ALREADY_IN_SESSION   = 0x0005
ERR_NAME_TOO_LONG        = 0x0006

KEEPALIVE_TIMEOUT_SEC    = 30

# ── Frame helpers ─────────────────────────────────────────────────────────────

def make_frame(version, msg_type, payload=b''):
    """Return a complete wire frame: 8-byte header + payload."""
    header = struct.pack('>HHI', version, msg_type, len(payload))
    return header + payload


def make_session_init(client_name='test-client'):
    name = client_name.encode()
    payload = bytes([len(name)]) + name
    return make_frame(PROTO_VERSION, MSG_SESSION_INIT, payload)


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


def parse_session_id(payload):
    """Parse 4-byte big-endian session ID from SESSION_INIT_ACK payload."""
    if len(payload) < 4:
        return None
    return struct.unpack('>I', payload[:4])[0]


# ── Pretty-printers ───────────────────────────────────────────────────────────

MSG_NAMES = {
    0x0001: 'SESSION_INIT',
    0x0002: 'SESSION_KEEPALIVE',
    0x0003: 'SESSION_CLOSE',
    0x8001: 'SESSION_INIT_ACK',
    0x8003: 'SESSION_CLOSE_ACK',
    0x9003: 'NOTIFY_SERVER_CLOSE',
    0xFFFF: 'MSG_ERR',
}

ERR_NAMES = {
    0x0001: 'ERR_PROTOCOL_VERSION',
    0x0002: 'ERR_UNKNOWN_MESSAGE_TYPE',
    0x0003: 'ERR_PAYLOAD_TOO_LARGE',
    0x0004: 'ERR_NOT_IN_SESSION',
    0x0005: 'ERR_ALREADY_IN_SESSION',
    0x0006: 'ERR_NAME_TOO_LONG',
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
    elif msg_type == MSG_SESSION_INIT_ACK:
        sid = parse_session_id(payload)
        print(f'            session_id={sid}')


def check(cond, label):
    print(f'  {"✓" if cond else "✗"} {label}')
    return cond


# ── Protocol framing tests (originally PR #25) ────────────────────────────────

def test_bad_version(host, port):
    """Test 1 — Bad protocol version → ERR_PROTOCOL_VERSION, connection closed."""
    print('\n[1] Bad protocol version (0x00FF) — expect ERR_PROTOCOL_VERSION, connection closed')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_frame(0x00FF, MSG_SESSION_INIT))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR, f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PROTOCOL_VERSION,
              f'err_code is ERR_PROTOCOL_VERSION (got {fmt_err(code)})')
        s.settimeout(1)
        try:
            tail = s.recv(8)
            check(tail == b'', 'connection closed by server')
        except socket.timeout:
            check(False, 'connection closed by server (timed out)')


def test_payload_too_large(host, port):
    """Test 2 — payload_len > 65535 → ERR_PAYLOAD_TOO_LARGE, connection closed."""
    oversized = PROTO_MAX_PAYLOAD + 1
    print(f'\n[2] Oversized payload ({oversized} bytes) — expect ERR_PAYLOAD_TOO_LARGE, connection closed')
    with socket.create_connection((host, port), timeout=10) as s:
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_INIT, b'\x00' * oversized))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR, f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PAYLOAD_TOO_LARGE,
              f'err_code is ERR_PAYLOAD_TOO_LARGE (got {fmt_err(code)})')
        s.settimeout(1)
        try:
            tail = s.recv(8)
            check(tail == b'', 'connection closed by server')
        except socket.timeout:
            check(False, 'connection closed by server (timed out)')


def test_unknown_type(host, port):
    """Test 3 — Unregistered message type after session → ERR_UNKNOWN_MESSAGE_TYPE, open."""
    print('\n[3] Unknown message type after session — expect ERR_UNKNOWN_MESSAGE_TYPE, connection kept open')
    with socket.create_connection((host, port), timeout=3) as s:
        # Establish session first
        s.sendall(make_session_init())
        frame = recv_frame(s)
        if frame is None or frame[1] != MSG_SESSION_INIT_ACK:
            check(False, 'could not establish session (prerequisite failed)')
            return
        # Send unknown type
        s.sendall(make_frame(PROTO_VERSION, 0x7FFF))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR, f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_UNKNOWN_MESSAGE_TYPE,
              f'err_code is ERR_UNKNOWN_MESSAGE_TYPE (got {fmt_err(code)})')
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_KEEPALIVE))
        frame2 = recv_frame(s)
        # SESSION_KEEPALIVE has no response; send another unknown to confirm open
        s.sendall(make_frame(PROTO_VERSION, 0x7FFF))
        frame3 = recv_frame(s)
        check(frame3 is not None, 'connection stayed open')

# ── Session lifecycle tests ───────────────────────────────────────────────────

def test_session_init(host, port):
    """Test 4 — SESSION_INIT → SESSION_INIT_ACK with non-zero session ID."""
    print('\n[4] SESSION_INIT — expect SESSION_INIT_ACK with non-zero session ID')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_session_init('my-client'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_SESSION_INIT_ACK,
              f'response is SESSION_INIT_ACK (got {fmt_type(msg_type)})')
        sid = parse_session_id(payload)
        check(sid is not None and sid != 0, f'session_id is non-zero (got {sid})')


def test_already_in_session(host, port):
    """Test 5 — SESSION_INIT on active session → ERR_ALREADY_IN_SESSION."""
    print('\n[5] SESSION_INIT on active session — expect ERR_ALREADY_IN_SESSION')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_session_init('client-a'))
        frame = recv_frame(s)
        if frame is None or frame[1] != MSG_SESSION_INIT_ACK:
            check(False, 'could not establish session (prerequisite failed)')
            return
        s.sendall(make_session_init('client-b'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR, f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_ALREADY_IN_SESSION,
              f'err_code is ERR_ALREADY_IN_SESSION (got {fmt_err(code)})')


def test_not_in_session(host, port):
    """Test 6 — Non-SESSION_INIT before session established → ERR_NOT_IN_SESSION."""
    print('\n[6] Message before SESSION_INIT — expect ERR_NOT_IN_SESSION')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_KEEPALIVE))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR, f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_NOT_IN_SESSION,
              f'err_code is ERR_NOT_IN_SESSION (got {fmt_err(code)})')
        # Connection should remain open
        s.sendall(make_session_init('late-client'))
        frame2 = recv_frame(s)
        check(frame2 is not None and frame2[1] == MSG_SESSION_INIT_ACK,
              'connection stayed open; SESSION_INIT still accepted')


def test_session_close(host, port):
    """Test 7 — SESSION_CLOSE → SESSION_CLOSE_ACK, connection closed."""
    print('\n[7] SESSION_CLOSE — expect SESSION_CLOSE_ACK, connection closed')
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(make_session_init('closing-client'))
        frame = recv_frame(s)
        if frame is None or frame[1] != MSG_SESSION_INIT_ACK:
            check(False, 'could not establish session (prerequisite failed)')
            return
        s.sendall(make_frame(PROTO_VERSION, MSG_SESSION_CLOSE))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_SESSION_CLOSE_ACK,
              f'response is SESSION_CLOSE_ACK (got {fmt_type(msg_type)})')
        s.settimeout(1)
        try:
            tail = s.recv(8)
            check(tail == b'', 'connection closed by server after ACK')
        except socket.timeout:
            check(False, 'connection closed by server (timed out)')


def test_server_close(host, port):
    """
    Test 8 — Graceful server shutdown → NOTIFY_SERVER_CLOSE (0x9003).
    Requires manually stopping the server (Ctrl-C) while this test waits.
    """
    print('\n[8] Graceful shutdown — stop the server now (Ctrl-C on server)')
    print('    Waiting up to 60 s for NOTIFY_SERVER_CLOSE … (Ctrl-C here to skip)')
    try:
        with socket.create_connection((host, port), timeout=60) as s:
            s.sendall(make_session_init('shutdown-watcher'))
            ack = recv_frame(s)
            if ack is None or ack[1] != MSG_SESSION_INIT_ACK:
                check(False, 'could not establish session')
                return
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


def test_keepalive_timeout(host, port):
    """
    Test 9 — Keep-alive timeout (30 s).
    Connects, establishes a session, then stays idle.  The server should
    close the connection after KEEPALIVE_TIMEOUT_SEC seconds.
    """
    wait = KEEPALIVE_TIMEOUT_SEC + 5
    print(f'\n[9] Keep-alive timeout — connecting and waiting {wait} s without sending anything')
    with socket.create_connection((host, port), timeout=wait + 5) as s:
        s.sendall(make_session_init('idle-client'))
        frame = recv_frame(s)
        if frame is None or frame[1] != MSG_SESSION_INIT_ACK:
            check(False, 'could not establish session')
            return
        print(f'  session established, waiting {wait} s for timeout …')
        s.settimeout(wait + 5)
        start = time.monotonic()
        tail = s.recv(8)
        elapsed = time.monotonic() - start
        check(tail == b'',
              f'connection closed by server (EOF received after {elapsed:.1f} s)')
        check(elapsed >= KEEPALIVE_TIMEOUT_SEC - 1,
              f'closed after at least {KEEPALIVE_TIMEOUT_SEC - 1} s '
              f'(actual: {elapsed:.1f} s)')


# ── Entry point ───────────────────────────────────────────────────────────────

TESTS = [
    test_bad_version,           # 1
    test_payload_too_large,     # 2
    test_unknown_type,          # 3
    test_session_init,          # 4
    test_already_in_session,    # 5
    test_not_in_session,        # 6
    test_session_close,         # 7
    test_server_close,          # 8  — interactive
    test_keepalive_timeout,     # 9  — slow (30+ s)
]

AUTO_TESTS = TESTS[:7]   # 1-7 run unattended


def main():
    parser = argparse.ArgumentParser(
        description='ash protocol + session lifecycle test client')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=4000)
    parser.add_argument('--test', type=int, choices=range(1, len(TESTS) + 1),
                        metavar='N', help=f'run only test N (1-{len(TESTS)})')
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
        for fn in AUTO_TESTS:
            run(fn)
        print('\n[8] Graceful shutdown  — run with --test 8')
        print('[9] Keep-alive timeout — run with --test 9 (takes 35 s)')

    print()


if __name__ == '__main__':
    main()
