#!/usr/bin/env python3
"""
ash protocol test client — exercises wire protocol and session lifecycle.

Usage:
    ./tools/test_client.py [--host HOST] [--port PORT] [--test N]

Default host/port: 127.0.0.1:4000

Tests 1-7 run automatically.
Tests 10-16 (CAN interface management) run automatically; the server must have
CAP_NET_ADMIN (root) to create/destroy vcan interfaces.
Test 8 (graceful server shutdown) requires manually stopping the server and
must be requested with --test 8.
Test 9 (keep-alive timeout) waits 30+ seconds and must be requested with --test 9.
Test 17 (NOTIFY_IFACE_DOWN) requires manually deleting vcan99 while the test
waits, and must be requested with --test 17.
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

# Session management
MSG_SESSION_INIT        = 0x0001
MSG_SESSION_KEEPALIVE   = 0x0002
MSG_SESSION_CLOSE       = 0x0003
MSG_SESSION_INIT_ACK    = 0x8001
MSG_SESSION_CLOSE_ACK   = 0x8003
MSG_NOTIFY_SERVER_CLOSE = 0x9003
MSG_ERR                 = 0xFFFF

# Interface management (SPEC §7)
MSG_IFACE_LIST          = 0x0010
MSG_IFACE_ATTACH        = 0x0011
MSG_IFACE_DETACH        = 0x0012
MSG_IFACE_VCAN_CREATE   = 0x0013
MSG_IFACE_VCAN_DESTROY  = 0x0014
MSG_IFACE_LIST_RESP     = 0x8010
MSG_IFACE_ATTACH_ACK    = 0x8011
MSG_IFACE_DETACH_ACK    = 0x8012
MSG_IFACE_VCAN_ACK      = 0x8013
MSG_NOTIFY_IFACE_DOWN   = 0x9002

# Session error codes
ERR_PROTOCOL_VERSION     = 0x0001
ERR_UNKNOWN_MESSAGE_TYPE = 0x0002
ERR_PAYLOAD_TOO_LARGE    = 0x0003
ERR_NOT_IN_SESSION       = 0x0004
ERR_ALREADY_IN_SESSION   = 0x0005
ERR_NAME_TOO_LONG        = 0x0006

# Interface error codes
ERR_IFACE_NOT_FOUND         = 0x0010
ERR_IFACE_ALREADY_ATTACHED  = 0x0011
ERR_IFACE_ATTACH_FAILED     = 0x0012

# Interface states (in IFACE_LIST_RESP)
IFACE_STATE_AVAILABLE       = 0x01
IFACE_STATE_ATTACHED_THIS   = 0x02
IFACE_STATE_ATTACHED_OTHER  = 0x03

KEEPALIVE_TIMEOUT_SEC    = 30

# vcan interface name used by tests 10-17
TEST_VCAN = 'vcan99'

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
    0x9002: 'NOTIFY_IFACE_DOWN',
    0x9003: 'NOTIFY_SERVER_CLOSE',
    0x0010: 'IFACE_LIST',
    0x0011: 'IFACE_ATTACH',
    0x0012: 'IFACE_DETACH',
    0x0013: 'IFACE_VCAN_CREATE',
    0x0014: 'IFACE_VCAN_DESTROY',
    0x8010: 'IFACE_LIST_RESP',
    0x8011: 'IFACE_ATTACH_ACK',
    0x8012: 'IFACE_DETACH_ACK',
    0x8013: 'IFACE_VCAN_ACK',
    0xFFFF: 'MSG_ERR',
}

ERR_NAMES = {
    0x0001: 'ERR_PROTOCOL_VERSION',
    0x0002: 'ERR_UNKNOWN_MESSAGE_TYPE',
    0x0003: 'ERR_PAYLOAD_TOO_LARGE',
    0x0004: 'ERR_NOT_IN_SESSION',
    0x0005: 'ERR_ALREADY_IN_SESSION',
    0x0006: 'ERR_NAME_TOO_LONG',
    0x0010: 'ERR_IFACE_NOT_FOUND',
    0x0011: 'ERR_IFACE_ALREADY_ATTACHED',
    0x0012: 'ERR_IFACE_ATTACH_FAILED',
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


# ── Interface management helpers ──────────────────────────────────────────────

def make_iface_vcan_create(name):
    n = name.encode()
    return make_frame(PROTO_VERSION, MSG_IFACE_VCAN_CREATE, bytes([len(n)]) + n)


def make_iface_vcan_destroy(name):
    n = name.encode()
    return make_frame(PROTO_VERSION, MSG_IFACE_VCAN_DESTROY, bytes([len(n)]) + n)


def make_iface_list():
    return make_frame(PROTO_VERSION, MSG_IFACE_LIST)


def make_iface_attach(name, mode=0x01, bitrate=0, filters=None):
    """Build an IFACE_ATTACH frame.

    mode:    0x01=CAN2.0A  0x02=CAN2.0B  0x03=CAN FD
    bitrate: bps; 0 means do not reconfigure
    filters: list of (can_id, mask) tuples; empty = promiscuous
    """
    n = name.encode()
    payload = bytes([len(n)]) + n
    payload += bytes([mode])
    payload += struct.pack('>I', bitrate)
    filters = filters or []
    payload += bytes([len(filters)])
    for can_id, mask in filters:
        payload += struct.pack('>II', can_id, mask)
    return make_frame(PROTO_VERSION, MSG_IFACE_ATTACH, payload)


def make_iface_detach(name):
    n = name.encode()
    return make_frame(PROTO_VERSION, MSG_IFACE_DETACH, bytes([len(n)]) + n)


def parse_iface_list_resp(payload):
    """Parse IFACE_LIST_RESP → list of (name, state) tuples."""
    if not payload:
        return []
    pos = 0
    count = payload[pos]; pos += 1
    result = []
    for _ in range(count):
        if pos >= len(payload):
            break
        nlen = payload[pos]; pos += 1
        name = payload[pos:pos + nlen].decode(errors='replace'); pos += nlen
        state = payload[pos]; pos += 1
        result.append((name, state))
    return result


def parse_attach_ack(payload):
    """Parse IFACE_ATTACH_ACK → app_port (u16 big-endian)."""
    if len(payload) < 2:
        return None
    return struct.unpack('>H', payload[:2])[0]


def open_session(host, port, name='test-client'):
    """Connect, send SESSION_INIT, return (socket, session_id) or (None, None)."""
    s = socket.create_connection((host, port), timeout=5)
    s.sendall(make_session_init(name))
    frame = recv_frame(s)
    if frame is None or frame[1] != MSG_SESSION_INIT_ACK:
        s.close()
        return None, None
    return s, parse_session_id(frame[2])


# ── Interface management tests (SPEC §7) ──────────────────────────────────────

def test_iface_vcan_create(host, port):
    """Test 10 — IFACE_VCAN_CREATE creates vcan99; IFACE_VCAN_DESTROY removes it."""
    print(f'\n[10] IFACE_VCAN_CREATE {TEST_VCAN} — expect IFACE_VCAN_ACK; destroy — expect IFACE_VCAN_ACK')
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(make_session_init('vcan-create-test'))
        if recv_frame(s) is None:
            check(False, 'could not establish session (prerequisite failed)')
            return

        # Create
        s.sendall(make_iface_vcan_create(TEST_VCAN))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response to VCAN_CREATE'):
            return
        _, msg_type, _ = frame
        ok = check(msg_type == MSG_IFACE_VCAN_ACK,
                   f'response is IFACE_VCAN_ACK (got {fmt_type(msg_type)})')

        # Destroy (cleanup regardless of create result)
        s.sendall(make_iface_vcan_destroy(TEST_VCAN))
        frame2 = recv_frame(s)
        print_response(frame2)
        if ok:
            if not check(frame2 is not None, 'received a response to VCAN_DESTROY'):
                return
            _, msg_type2, _ = frame2
            check(msg_type2 == MSG_IFACE_VCAN_ACK,
                  f'response is IFACE_VCAN_ACK (got {fmt_type(msg_type2)})')


def test_iface_list(host, port):
    """Test 11 — IFACE_LIST_RESP contains vcan99 as available after creation."""
    print(f'\n[11] IFACE_LIST — expect {TEST_VCAN} listed as available (state=0x01)')
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(make_session_init('iface-list-test'))
        if recv_frame(s) is None:
            check(False, 'could not establish session (prerequisite failed)')
            return

        # Create vcan
        s.sendall(make_iface_vcan_create(TEST_VCAN))
        frame = recv_frame(s)
        if frame is None or frame[1] != MSG_IFACE_VCAN_ACK:
            check(False, f'VCAN_CREATE prerequisite failed ({fmt_type(frame[1]) if frame else "no response"})')
            return

        try:
            # List
            s.sendall(make_iface_list())
            frame = recv_frame(s)
            print_response(frame)
            if not check(frame is not None, 'received a response'):
                return
            _, msg_type, payload = frame
            if not check(msg_type == MSG_IFACE_LIST_RESP,
                         f'response is IFACE_LIST_RESP (got {fmt_type(msg_type)})'):
                return
            ifaces = parse_iface_list_resp(payload)
            names = [n for n, _ in ifaces]
            print(f'  interfaces: {ifaces}')
            if check(TEST_VCAN in names, f'{TEST_VCAN} is present in list'):
                state = dict(ifaces)[TEST_VCAN]
                check(state == IFACE_STATE_AVAILABLE,
                      f'{TEST_VCAN} state is AVAILABLE (0x01) (got 0x{state:02X})')
        finally:
            s.sendall(make_iface_vcan_destroy(TEST_VCAN))
            recv_frame(s)


def test_iface_attach(host, port):
    """Test 12 — IFACE_ATTACH returns a valid app-plane port; list shows attached-this."""
    print(f'\n[12] IFACE_ATTACH {TEST_VCAN} — expect IFACE_ATTACH_ACK with non-zero port; list shows attached-this')
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(make_session_init('iface-attach-test'))
        if recv_frame(s) is None:
            check(False, 'could not establish session (prerequisite failed)')
            return

        s.sendall(make_iface_vcan_create(TEST_VCAN))
        if recv_frame(s) is None:
            check(False, 'VCAN_CREATE prerequisite failed')
            return

        try:
            s.sendall(make_iface_attach(TEST_VCAN))
            frame = recv_frame(s)
            print_response(frame)
            if not check(frame is not None, 'received a response to IFACE_ATTACH'):
                return
            _, msg_type, payload = frame
            if not check(msg_type == MSG_IFACE_ATTACH_ACK,
                         f'response is IFACE_ATTACH_ACK (got {fmt_type(msg_type)})'):
                return
            port_val = parse_attach_ack(payload)
            check(port_val is not None and port_val != 0,
                  f'app-plane port is non-zero (got {port_val})')

            # Verify list shows attached-this
            s.sendall(make_iface_list())
            frame2 = recv_frame(s)
            if frame2 and frame2[1] == MSG_IFACE_LIST_RESP:
                ifaces = parse_iface_list_resp(frame2[2])
                state = dict(ifaces).get(TEST_VCAN)
                check(state == IFACE_STATE_ATTACHED_THIS,
                      f'list shows {TEST_VCAN} as ATTACHED_THIS (0x02) (got 0x{state:02X})'
                      if state is not None else f'{TEST_VCAN} missing from list')
        finally:
            s.sendall(make_iface_vcan_destroy(TEST_VCAN))
            recv_frame(s)


def test_iface_already_attached(host, port):
    """Test 13 — Second IFACE_ATTACH (same or different session) → ERR_IFACE_ALREADY_ATTACHED; list shows attached-other from second session."""
    print(f'\n[13] Duplicate IFACE_ATTACH — expect ERR_IFACE_ALREADY_ATTACHED; list shows attached-other from second session')
    with socket.create_connection((host, port), timeout=5) as s1, \
         socket.create_connection((host, port), timeout=5) as s2:
        s1.sendall(make_session_init('attach-owner'))
        if recv_frame(s1) is None:
            check(False, 'could not establish session 1 (prerequisite failed)')
            return
        s2.sendall(make_session_init('attach-thief'))
        if recv_frame(s2) is None:
            check(False, 'could not establish session 2 (prerequisite failed)')
            return

        s1.sendall(make_iface_vcan_create(TEST_VCAN))
        if recv_frame(s1) is None:
            check(False, f'VCAN_CREATE prerequisite failed')
            return

        try:
            # First attach (from s1) — should succeed
            s1.sendall(make_iface_attach(TEST_VCAN))
            frame = recv_frame(s1)
            if frame is None or frame[1] != MSG_IFACE_ATTACH_ACK:
                check(False, 'first attach prerequisite failed')
                return

            # Second attach (from s2) — should fail
            s2.sendall(make_iface_attach(TEST_VCAN))
            frame2 = recv_frame(s2)
            print_response(frame2)
            if not check(frame2 is not None, 'received a response to second IFACE_ATTACH'):
                return
            _, msg_type, payload = frame2
            check(msg_type == MSG_ERR,
                  f'response is MSG_ERR (got {fmt_type(msg_type)})')
            code, _ = decode_err(payload)
            check(code == ERR_IFACE_ALREADY_ATTACHED,
                  f'err_code is ERR_IFACE_ALREADY_ATTACHED (got {fmt_err(code)})')

            # Second session's IFACE_LIST should show attached-other
            s2.sendall(make_iface_list())
            frame3 = recv_frame(s2)
            if frame3 and frame3[1] == MSG_IFACE_LIST_RESP:
                ifaces = parse_iface_list_resp(frame3[2])
                state = dict(ifaces).get(TEST_VCAN)
                check(state == IFACE_STATE_ATTACHED_OTHER,
                      f'list (from s2) shows {TEST_VCAN} as ATTACHED_OTHER (0x03) (got 0x{state:02X})'
                      if state is not None else f'{TEST_VCAN} missing from list')
        finally:
            s1.sendall(make_iface_vcan_destroy(TEST_VCAN))
            recv_frame(s1)


def test_iface_detach(host, port):
    """Test 14 — IFACE_DETACH releases interface; subsequent attach by another session succeeds."""
    print(f'\n[14] IFACE_DETACH — expect IFACE_DETACH_ACK; re-attach by second session succeeds')
    with socket.create_connection((host, port), timeout=5) as s1, \
         socket.create_connection((host, port), timeout=5) as s2:
        s1.sendall(make_session_init('detach-owner'))
        if recv_frame(s1) is None:
            check(False, 'could not establish session 1 (prerequisite failed)')
            return
        s2.sendall(make_session_init('detach-reattach'))
        if recv_frame(s2) is None:
            check(False, 'could not establish session 2 (prerequisite failed)')
            return

        s1.sendall(make_iface_vcan_create(TEST_VCAN))
        if recv_frame(s1) is None:
            check(False, 'VCAN_CREATE prerequisite failed')
            return

        try:
            s1.sendall(make_iface_attach(TEST_VCAN))
            if recv_frame(s1) is None:
                check(False, 'IFACE_ATTACH prerequisite failed')
                return

            # Detach
            s1.sendall(make_iface_detach(TEST_VCAN))
            frame = recv_frame(s1)
            print_response(frame)
            if not check(frame is not None, 'received a response to IFACE_DETACH'):
                return
            _, msg_type, _ = frame
            check(msg_type == MSG_IFACE_DETACH_ACK,
                  f'response is IFACE_DETACH_ACK (got {fmt_type(msg_type)})')

            # Re-attach from second session
            s2.sendall(make_iface_attach(TEST_VCAN))
            frame2 = recv_frame(s2)
            print_response(frame2)
            if not check(frame2 is not None, 'received response to re-attach'):
                return
            _, msg_type2, payload2 = frame2
            check(msg_type2 == MSG_IFACE_ATTACH_ACK,
                  f're-attach succeeds (got {fmt_type(msg_type2)})')
            if msg_type2 == MSG_IFACE_ATTACH_ACK:
                port_val = parse_attach_ack(payload2)
                check(port_val is not None and port_val != 0,
                      f're-attach app-plane port is non-zero (got {port_val})')
        finally:
            s1.sendall(make_iface_vcan_destroy(TEST_VCAN))
            recv_frame(s1)


def test_session_close_releases_iface(host, port):
    """Test 15 — Session close (without explicit IFACE_DETACH) releases the interface."""
    print(f'\n[15] Session close releases attached interface — second session can re-attach')
    with socket.create_connection((host, port), timeout=5) as s1:
        s1.sendall(make_session_init('close-owner'))
        if recv_frame(s1) is None:
            check(False, 'could not establish session 1 (prerequisite failed)')
            return

        s1.sendall(make_iface_vcan_create(TEST_VCAN))
        if recv_frame(s1) is None:
            check(False, 'VCAN_CREATE prerequisite failed')
            return

        s1.sendall(make_iface_attach(TEST_VCAN))
        if recv_frame(s1) is None:
            check(False, 'IFACE_ATTACH prerequisite failed')
            return

        # Close session (server must clean up the attachment)
        s1.sendall(make_frame(PROTO_VERSION, MSG_SESSION_CLOSE))
        frame = recv_frame(s1)
        if not check(frame is not None and frame[1] == MSG_SESSION_CLOSE_ACK,
                     'SESSION_CLOSE_ACK received'):
            return

    # New session: attach should now succeed
    try:
        with socket.create_connection((host, port), timeout=5) as s2:
            s2.sendall(make_session_init('close-reattach'))
            if recv_frame(s2) is None:
                check(False, 'could not establish session 2')
                return
            s2.sendall(make_iface_attach(TEST_VCAN))
            frame2 = recv_frame(s2)
            print_response(frame2)
            if not check(frame2 is not None, 'received response to re-attach'):
                return
            _, msg_type, _ = frame2
            check(msg_type == MSG_IFACE_ATTACH_ACK,
                  f'session-close released interface; re-attach succeeds (got {fmt_type(msg_type)})')
    finally:
        with socket.create_connection((host, port), timeout=5) as cleanup:
            cleanup.sendall(make_session_init('cleanup'))
            recv_frame(cleanup)
            cleanup.sendall(make_iface_vcan_destroy(TEST_VCAN))
            recv_frame(cleanup)


def test_iface_vcan_destroy(host, port):
    """Test 16 — IFACE_VCAN_DESTROY removes the interface (not in subsequent IFACE_LIST)."""
    print(f'\n[16] IFACE_VCAN_DESTROY {TEST_VCAN} — interface absent from subsequent IFACE_LIST')
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(make_session_init('vcan-destroy-test'))
        if recv_frame(s) is None:
            check(False, 'could not establish session (prerequisite failed)')
            return

        s.sendall(make_iface_vcan_create(TEST_VCAN))
        if recv_frame(s) is None:
            check(False, 'VCAN_CREATE prerequisite failed')
            return

        s.sendall(make_iface_vcan_destroy(TEST_VCAN))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response to VCAN_DESTROY'):
            return
        _, msg_type, _ = frame
        if not check(msg_type == MSG_IFACE_VCAN_ACK,
                     f'response is IFACE_VCAN_ACK (got {fmt_type(msg_type)})'):
            return

        s.sendall(make_iface_list())
        frame2 = recv_frame(s)
        if frame2 and frame2[1] == MSG_IFACE_LIST_RESP:
            ifaces = parse_iface_list_resp(frame2[2])
            names = [n for n, _ in ifaces]
            check(TEST_VCAN not in names,
                  f'{TEST_VCAN} absent from IFACE_LIST after destroy (got {names})')


def test_notify_iface_down(host, port):
    """
    Test 17 — NOTIFY_IFACE_DOWN emitted when an attached interface is deleted.
    Attach to vcan99, then manually run:  sudo ip link delete vcan99
    The server should send NOTIFY_IFACE_DOWN to this session.
    """
    print(f'\n[17] NOTIFY_IFACE_DOWN — attach to {TEST_VCAN}, then run: sudo ip link delete {TEST_VCAN}')
    print(f'     Waiting up to 60 s for NOTIFY_IFACE_DOWN … (Ctrl-C to skip)')
    try:
        with socket.create_connection((host, port), timeout=60) as s:
            s.sendall(make_session_init('notify-down-watcher'))
            if recv_frame(s) is None:
                check(False, 'could not establish session')
                return

            s.sendall(make_iface_vcan_create(TEST_VCAN))
            frame = recv_frame(s)
            if frame is None or frame[1] != MSG_IFACE_VCAN_ACK:
                check(False, 'VCAN_CREATE prerequisite failed')
                return

            s.sendall(make_iface_attach(TEST_VCAN))
            frame = recv_frame(s)
            if frame is None or frame[1] != MSG_IFACE_ATTACH_ACK:
                check(False, 'IFACE_ATTACH prerequisite failed')
                return

            print(f'  session attached to {TEST_VCAN}; now run: sudo ip link delete {TEST_VCAN}')
            s.settimeout(60)
            frame = recv_frame(s)
            print_response(frame)
            if not check(frame is not None, 'received a notification frame'):
                return
            _, msg_type, payload = frame
            check(msg_type == MSG_NOTIFY_IFACE_DOWN,
                  f'received NOTIFY_IFACE_DOWN (got {fmt_type(msg_type)})')
            if msg_type == MSG_NOTIFY_IFACE_DOWN and payload:
                nlen = payload[0]
                name = payload[1:1 + nlen].decode(errors='replace')
                check(name == TEST_VCAN,
                      f'notification names {TEST_VCAN} (got {name!r})')
    except KeyboardInterrupt:
        print('  [skipped]')


# ── Entry point ───────────────────────────────────────────────────────────────

TESTS = [
    test_bad_version,                   # 1
    test_payload_too_large,             # 2
    test_unknown_type,                  # 3
    test_session_init,                  # 4
    test_already_in_session,            # 5
    test_not_in_session,                # 6
    test_session_close,                 # 7
    test_server_close,                  # 8  — interactive
    test_keepalive_timeout,             # 9  — slow (30+ s)
    test_iface_vcan_create,             # 10
    test_iface_list,                    # 11
    test_iface_attach,                  # 12
    test_iface_already_attached,        # 13
    test_iface_detach,                  # 14
    test_session_close_releases_iface,  # 15
    test_iface_vcan_destroy,            # 16
    test_notify_iface_down,             # 17 — interactive
]

AUTO_TESTS = TESTS[:7] + TESTS[9:16]   # 1-7 and 10-16 run unattended


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
        print('\n[8]  Graceful shutdown    — run with --test 8')
        print('[9]  Keep-alive timeout  — run with --test 9 (takes 35 s)')
        print('[17] NOTIFY_IFACE_DOWN   — run with --test 17 (requires manual ip link delete)')

    print()


if __name__ == '__main__':
    main()
