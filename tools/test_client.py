#!/usr/bin/env python3
"""
ash protocol test client — exercises wire protocol and session lifecycle.

Usage:
    ./tools/test_client.py [--host HOST] [--port PORT] [--test N]
                           [--storage-dir DIR]

Default host/port: 127.0.0.1:4000

Tests 1-7 run automatically.
Tests 10-16 (CAN interface management) run automatically; the server must have
CAP_NET_ADMIN (root) to create/destroy vcan interfaces.
Tests 18-28 (definition store) run automatically.
Tests 29-36 (signal ownership) run automatically.
Tests 37-39 (CAP_NET_ADMIN) run automatically; they self-skip when the server
holds CAP_NET_ADMIN.
Tests 40-43 (configuration persistence) run automatically; tests 40, 42, and 43
self-skip when the server has no storage dir.  Pass --storage-dir to match the
directory given to ash-server --storage-dir.
Test 8 (graceful server shutdown) requires manually stopping the server and
must be requested with --test 8.
Test 9 (keep-alive timeout) waits 30+ seconds and must be requested with --test 9.
Test 17 (NOTIFY_IFACE_DOWN) requires manually deleting vcan99 while the test
waits, and must be requested with --test 17.
"""

import argparse
import os
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

# Definition management (SPEC §6)
MSG_DEF_SIGNAL  = 0x0020
MSG_DEF_PDU     = 0x0021
MSG_DEF_FRAME   = 0x0022
MSG_DEF_DELETE  = 0x0023
MSG_DEF_ACK     = 0x8020

ERR_DEF_INVALID   = 0x0020
ERR_DEF_CONFLICT  = 0x0021
ERR_DEF_IN_USE    = 0x0022

# Configuration persistence (SPEC §9)
MSG_CFG_SAVE    = 0x0040
MSG_CFG_LOAD    = 0x0041
MSG_CFG_ACK     = 0x8040

ERR_CFG_IO        = 0x0040
ERR_CFG_CHECKSUM  = 0x0041
ERR_CFG_CONFLICT  = 0x0042

DEF_TYPE_SIGNAL = 0x01
DEF_TYPE_PDU    = 0x02
DEF_TYPE_FRAME  = 0x03

KEEPALIVE_TIMEOUT_SEC    = 30

# vcan interface name used by tests 10-17
TEST_VCAN = 'vcan99'

# Storage directory for cfg tests 40-43 (set from --storage-dir in main)
_storage_dir = None

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
    0x0020: 'DEF_SIGNAL',
    0x0021: 'DEF_PDU',
    0x0022: 'DEF_FRAME',
    0x0023: 'DEF_DELETE',
    0x8020: 'DEF_ACK',
    0x0030: 'OWN_ACQUIRE',
    0x0031: 'OWN_RELEASE',
    0x0032: 'OWN_LOCK',
    0x0033: 'OWN_UNLOCK',
    0x8030: 'OWN_ACK',
    0x9001: 'NOTIFY_OWN_REVOKED',
    0x0040: 'CFG_SAVE',
    0x0041: 'CFG_LOAD',
    0x8040: 'CFG_ACK',
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
    0x0013: 'ERR_PERMISSION_DENIED',
    0x0020: 'ERR_DEF_INVALID',
    0x0021: 'ERR_DEF_CONFLICT',
    0x0022: 'ERR_DEF_IN_USE',
    0x0030: 'ERR_OWN_NOT_AVAILABLE',
    0x0031: 'ERR_OWN_NOT_HELD',
    0x0040: 'ERR_CFG_IO',
    0x0041: 'ERR_CFG_CHECKSUM',
    0x0042: 'ERR_CFG_CONFLICT',
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


# ── Definition management helpers ────────────────────────────────────────────

def make_def_signal(name, data_type=0x01, byte_order=0x01, bit_length=8,
                    scale=1.0, offset=0.0, min_val=0.0, max_val=255.0):
    """Build a DEF_SIGNAL frame (SPEC §6.1).

    data_type:  0x01=uint, 0x02=sint, 0x03=float
    byte_order: 0x01=little-endian, 0x02=big-endian
    """
    n = name.encode()
    payload = bytes([len(n)]) + n
    payload += bytes([data_type, byte_order, bit_length])
    payload += struct.pack('>dddd', scale, offset, min_val, max_val)
    return make_frame(PROTO_VERSION, MSG_DEF_SIGNAL, payload)


def make_def_pdu(name, pdu_length, signal_mappings):
    """Build a DEF_PDU frame (SPEC §6.2).

    signal_mappings: list of (signal_name, start_bit) tuples
    """
    n = name.encode()
    payload = bytes([len(n)]) + n + bytes([pdu_length, len(signal_mappings)])
    for sig_name, start_bit in signal_mappings:
        sn = sig_name.encode()
        payload += bytes([len(sn)]) + sn + bytes([start_bit])
    return make_frame(PROTO_VERSION, MSG_DEF_PDU, payload)


def make_def_frame(name, can_id, id_type, dlc, tx_period, pdu_mappings):
    """Build a DEF_FRAME frame (SPEC §6.3).

    id_type:     0x01=standard (11-bit), 0x02=extended (29-bit)
    tx_period:   ms; 0 = event-driven
    pdu_mappings: list of (pdu_name, byte_offset) tuples
    """
    n = name.encode()
    payload = bytes([len(n)]) + n
    payload += struct.pack('>I', can_id)
    payload += bytes([id_type, dlc])
    payload += struct.pack('>H', tx_period)
    payload += bytes([len(pdu_mappings)])
    for pdu_name, byte_offset in pdu_mappings:
        pn = pdu_name.encode()
        payload += bytes([len(pn)]) + pn + bytes([byte_offset])
    return make_frame(PROTO_VERSION, MSG_DEF_FRAME, payload)


def make_def_delete(name, def_type):
    """Build a DEF_DELETE frame (SPEC §6.4).

    def_type: DEF_TYPE_SIGNAL, DEF_TYPE_PDU, or DEF_TYPE_FRAME
    """
    n = name.encode()
    payload = bytes([len(n)]) + n + bytes([def_type])
    return make_frame(PROTO_VERSION, MSG_DEF_DELETE, payload)


def def_cleanup(s, *name_type_pairs):
    """Best-effort DEF_DELETE for each (name, def_type) pair (reverse order)."""
    for name, def_type in reversed(name_type_pairs):
        s.sendall(make_def_delete(name, def_type))
        recv_frame(s)


# ── Definition store tests (SPEC §6) ─────────────────────────────────────────

def test_def_signal(host, port):
    """Test 18 — DEF_SIGNAL: define a signal → DEF_ACK."""
    print('\n[18] DEF_SIGNAL — expect DEF_ACK')
    with open_session(host, port, 'def-sig-test')[0] as s:
        s.sendall(make_def_signal('t18_sig'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_DEF_ACK,
              f'response is DEF_ACK (got {fmt_type(msg_type)})')
        def_cleanup(s, ('t18_sig', DEF_TYPE_SIGNAL))


def test_def_pdu(host, port):
    """Test 19 — DEF_PDU: define signal then PDU referencing it → DEF_ACK."""
    print('\n[19] DEF_PDU — define signal then PDU referencing it, expect DEF_ACK')
    with open_session(host, port, 'def-pdu-test')[0] as s:
        # Prerequisite: define the signal
        s.sendall(make_def_signal('t19_sig'))
        if recv_frame(s) is None:
            check(False, 'DEF_SIGNAL prerequisite failed')
            return

        s.sendall(make_def_pdu('t19_pdu', 8, [('t19_sig', 0)]))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            def_cleanup(s, ('t19_sig', DEF_TYPE_SIGNAL))
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_DEF_ACK,
              f'response is DEF_ACK (got {fmt_type(msg_type)})')
        def_cleanup(s, ('t19_sig', DEF_TYPE_SIGNAL), ('t19_pdu', DEF_TYPE_PDU))


def test_def_frame(host, port):
    """Test 20 — DEF_FRAME: full define chain signal → PDU → frame → DEF_ACK."""
    print('\n[20] DEF_FRAME — full define chain, expect DEF_ACK for each')
    with open_session(host, port, 'def-frame-test')[0] as s:
        s.sendall(make_def_signal('t20_sig'))
        if recv_frame(s) is None:
            check(False, 'DEF_SIGNAL prerequisite failed')
            return

        s.sendall(make_def_pdu('t20_pdu', 8, [('t20_sig', 0)]))
        if recv_frame(s) is None:
            check(False, 'DEF_PDU prerequisite failed')
            def_cleanup(s, ('t20_sig', DEF_TYPE_SIGNAL))
            return

        s.sendall(make_def_frame('t20_frame', 0x123, 0x01, 8, 0,
                                 [('t20_pdu', 0)]))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            def_cleanup(s, ('t20_sig', DEF_TYPE_SIGNAL), ('t20_pdu', DEF_TYPE_PDU))
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_DEF_ACK,
              f'response is DEF_ACK (got {fmt_type(msg_type)})')
        def_cleanup(s,
                    ('t20_sig', DEF_TYPE_SIGNAL),
                    ('t20_pdu', DEF_TYPE_PDU),
                    ('t20_frame', DEF_TYPE_FRAME))


def test_def_pdu_unknown_signal(host, port):
    """Test 21 — DEF_PDU with an undefined signal reference → ERR_DEF_INVALID."""
    print('\n[21] DEF_PDU with unknown signal — expect ERR_DEF_INVALID')
    with open_session(host, port, 'def-pdu-badsig')[0] as s:
        s.sendall(make_def_pdu('t21_pdu', 8, [('no_such_signal', 0)]))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_DEF_INVALID,
              f'err_code is ERR_DEF_INVALID (got {fmt_err(code)})')


def test_def_frame_unknown_pdu(host, port):
    """Test 22 — DEF_FRAME with an undefined PDU reference → ERR_DEF_INVALID."""
    print('\n[22] DEF_FRAME with unknown PDU — expect ERR_DEF_INVALID')
    with open_session(host, port, 'def-frame-badpdu')[0] as s:
        s.sendall(make_def_frame('t22_frame', 0x456, 0x01, 8, 0,
                                 [('no_such_pdu', 0)]))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_DEF_INVALID,
              f'err_code is ERR_DEF_INVALID (got {fmt_err(code)})')


def test_def_type_conflict(host, port):
    """Test 23 — Reusing a signal name as a PDU name → ERR_DEF_CONFLICT."""
    print('\n[23] Type conflict: define signal, reuse name as PDU — expect ERR_DEF_CONFLICT')
    with open_session(host, port, 'def-conflict-test')[0] as s:
        s.sendall(make_def_signal('t23_name'))
        if recv_frame(s) is None:
            check(False, 'DEF_SIGNAL prerequisite failed')
            return

        # Attempt to define a PDU with the same name
        s.sendall(make_def_pdu('t23_name', 8, []))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            def_cleanup(s, ('t23_name', DEF_TYPE_SIGNAL))
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_DEF_CONFLICT,
              f'err_code is ERR_DEF_CONFLICT (got {fmt_err(code)})')
        def_cleanup(s, ('t23_name', DEF_TYPE_SIGNAL))


def test_def_delete_signal_in_use(host, port):
    """Test 24 — Delete a signal referenced by a PDU → ERR_DEF_IN_USE."""
    print('\n[24] DEF_DELETE signal in use by PDU — expect ERR_DEF_IN_USE')
    with open_session(host, port, 'def-del-sig-inuse')[0] as s:
        s.sendall(make_def_signal('t24_sig'))
        if recv_frame(s) is None:
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_def_pdu('t24_pdu', 8, [('t24_sig', 0)]))
        if recv_frame(s) is None:
            check(False, 'DEF_PDU prerequisite failed')
            def_cleanup(s, ('t24_sig', DEF_TYPE_SIGNAL))
            return

        s.sendall(make_def_delete('t24_sig', DEF_TYPE_SIGNAL))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            def_cleanup(s, ('t24_sig', DEF_TYPE_SIGNAL), ('t24_pdu', DEF_TYPE_PDU))
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_DEF_IN_USE,
              f'err_code is ERR_DEF_IN_USE (got {fmt_err(code)})')
        def_cleanup(s, ('t24_sig', DEF_TYPE_SIGNAL), ('t24_pdu', DEF_TYPE_PDU))


def test_def_delete_pdu_in_use(host, port):
    """Test 25 — Delete a PDU referenced by a frame → ERR_DEF_IN_USE."""
    print('\n[25] DEF_DELETE PDU in use by frame — expect ERR_DEF_IN_USE')
    with open_session(host, port, 'def-del-pdu-inuse')[0] as s:
        s.sendall(make_def_signal('t25_sig'))
        if recv_frame(s) is None:
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_def_pdu('t25_pdu', 8, [('t25_sig', 0)]))
        if recv_frame(s) is None:
            check(False, 'DEF_PDU prerequisite failed')
            def_cleanup(s, ('t25_sig', DEF_TYPE_SIGNAL))
            return
        s.sendall(make_def_frame('t25_frame', 0x789, 0x01, 8, 0,
                                 [('t25_pdu', 0)]))
        if recv_frame(s) is None:
            check(False, 'DEF_FRAME prerequisite failed')
            def_cleanup(s, ('t25_sig', DEF_TYPE_SIGNAL), ('t25_pdu', DEF_TYPE_PDU))
            return

        s.sendall(make_def_delete('t25_pdu', DEF_TYPE_PDU))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            def_cleanup(s, ('t25_sig', DEF_TYPE_SIGNAL), ('t25_pdu', DEF_TYPE_PDU),
                        ('t25_frame', DEF_TYPE_FRAME))
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_DEF_IN_USE,
              f'err_code is ERR_DEF_IN_USE (got {fmt_err(code)})')
        def_cleanup(s, ('t25_sig', DEF_TYPE_SIGNAL), ('t25_pdu', DEF_TYPE_PDU),
                    ('t25_frame', DEF_TYPE_FRAME))


def test_def_pdu_signal_count_limit(host, port):
    """Test 26 — DEF_PDU with signal_count > 32 → ERR_DEF_INVALID."""
    print('\n[26] DEF_PDU signal_count=33 (> 32 limit) — expect ERR_DEF_INVALID')
    with open_session(host, port, 'def-pdu-siglimit')[0] as s:
        # Build a PDU payload with signal_count=33 but no actual mappings.
        # The server validates the count before parsing mappings.
        n = b't26_pdu'
        payload = bytes([len(n)]) + n + bytes([8, 33])  # pdu_length=8, signal_count=33
        s.sendall(make_frame(PROTO_VERSION, MSG_DEF_PDU, payload))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload_resp = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload_resp)
        check(code == ERR_DEF_INVALID,
              f'err_code is ERR_DEF_INVALID (got {fmt_err(code)})')


def test_def_frame_pdu_count_limit(host, port):
    """Test 27 — DEF_FRAME with pdu_count > 8 → ERR_DEF_INVALID."""
    print('\n[27] DEF_FRAME pdu_count=9 (> 8 limit) — expect ERR_DEF_INVALID')
    with open_session(host, port, 'def-frame-pdulimit')[0] as s:
        # Build a FRAME payload with pdu_count=9 but no actual mappings.
        n = b't27_frame'
        payload  = bytes([len(n)]) + n
        payload += struct.pack('>I', 0x100)   # can_id
        payload += bytes([0x01, 8])            # id_type=standard, dlc=8
        payload += struct.pack('>H', 0)        # tx_period=0
        payload += bytes([9])                  # pdu_count=9 (over limit)
        s.sendall(make_frame(PROTO_VERSION, MSG_DEF_FRAME, payload))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload_resp = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload_resp)
        check(code == ERR_DEF_INVALID,
              f'err_code is ERR_DEF_INVALID (got {fmt_err(code)})')


def test_def_persistence(host, port):
    """Test 28 — Definitions persist after session disconnect."""
    print('\n[28] Definition persistence — signal defined in session 1 visible to session 2')
    # Session 1: define signal
    s1, _ = open_session(host, port, 'def-persist-s1')
    if s1 is None:
        check(False, 'could not establish session 1 (prerequisite failed)')
        return

    try:
        s1.sendall(make_def_signal('t28_sig'))
        if recv_frame(s1) is None:
            check(False, 'DEF_SIGNAL in session 1 failed')
            return
        s1.sendall(make_frame(PROTO_VERSION, MSG_SESSION_CLOSE))
        recv_frame(s1)
    finally:
        s1.close()

    # Session 2: define a PDU referencing the signal from session 1 — must succeed
    with open_session(host, port, 'def-persist-s2')[0] as s2:
        s2.sendall(make_def_pdu('t28_pdu', 8, [('t28_sig', 0)]))
        frame = recv_frame(s2)
        print_response(frame)
        if not check(frame is not None, 'received a response in session 2'):
            def_cleanup(s2, ('t28_sig', DEF_TYPE_SIGNAL))
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_DEF_ACK,
              f'DEF_PDU referencing persisted signal succeeds (got {fmt_type(msg_type)})')
        def_cleanup(s2, ('t28_sig', DEF_TYPE_SIGNAL), ('t28_pdu', DEF_TYPE_PDU))


# ── Ownership helpers ─────────────────────────────────────────────────────────

MSG_OWN_ACQUIRE       = 0x0030
MSG_OWN_RELEASE       = 0x0031
MSG_OWN_LOCK          = 0x0032
MSG_OWN_UNLOCK        = 0x0033
MSG_OWN_ACK           = 0x8030
MSG_NOTIFY_OWN_REVOKED = 0x9001

ERR_OWN_NOT_AVAILABLE = 0x0030
ERR_OWN_NOT_HELD      = 0x0031

OWN_ON_DISC_STOP    = 0x01
OWN_ON_DISC_LAST    = 0x02
OWN_ON_DISC_DEFAULT = 0x03


def make_own_acquire(sig_name, on_disconnect=OWN_ON_DISC_STOP):
    n = sig_name.encode()
    payload = bytes([len(n)]) + n + bytes([on_disconnect])
    return make_frame(PROTO_VERSION, MSG_OWN_ACQUIRE, payload)


def make_own_release(sig_name):
    n = sig_name.encode()
    payload = bytes([len(n)]) + n
    return make_frame(PROTO_VERSION, MSG_OWN_RELEASE, payload)


def make_own_lock(sig_name):
    n = sig_name.encode()
    payload = bytes([len(n)]) + n
    return make_frame(PROTO_VERSION, MSG_OWN_LOCK, payload)


def make_own_unlock(sig_name):
    n = sig_name.encode()
    payload = bytes([len(n)]) + n
    return make_frame(PROTO_VERSION, MSG_OWN_UNLOCK, payload)


def _define_signal(s, sig_name):
    """Helper: define a signal and return True on success."""
    s.sendall(make_def_signal(sig_name))
    f = recv_frame(s)
    return f is not None and f[1] == MSG_DEF_ACK


def _delete_signal(s, sig_name):
    """Helper: best-effort DEF_DELETE for a signal."""
    s.sendall(make_def_delete(sig_name, DEF_TYPE_SIGNAL))
    recv_frame(s)


# ── Ownership tests (SPEC §8) ─────────────────────────────────────────────────

def test_own_acquire(host, port):
    """Test 29 — OWN_ACQUIRE on a defined signal → OWN_ACK."""
    print('\n[29] OWN_ACQUIRE — define signal, acquire ownership, expect OWN_ACK')
    with open_session(host, port, 'own-acquire-test')[0] as s:
        if not _define_signal(s, 't29_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_own_acquire('t29_sig'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            _delete_signal(s, 't29_sig')
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_OWN_ACK,
              f'response is OWN_ACK (got {fmt_type(msg_type)})')
        s.sendall(make_own_release('t29_sig'))
        recv_frame(s)
        _delete_signal(s, 't29_sig')


def test_own_release(host, port):
    """Test 30 — OWN_RELEASE by owning session → OWN_ACK."""
    print('\n[30] OWN_RELEASE — acquire then release, expect OWN_ACK')
    with open_session(host, port, 'own-release-test')[0] as s:
        if not _define_signal(s, 't30_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_own_acquire('t30_sig'))
        if recv_frame(s) is None:
            check(False, 'OWN_ACQUIRE prerequisite failed')
            _delete_signal(s, 't30_sig')
            return
        s.sendall(make_own_release('t30_sig'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            _delete_signal(s, 't30_sig')
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_OWN_ACK,
              f'response is OWN_ACK (got {fmt_type(msg_type)})')
        _delete_signal(s, 't30_sig')


def test_own_release_not_held(host, port):
    """Test 31 — OWN_RELEASE without ownership → ERR_OWN_NOT_HELD."""
    print('\n[31] OWN_RELEASE without ownership — expect ERR_OWN_NOT_HELD')
    with open_session(host, port, 'own-release-notheld')[0] as s:
        if not _define_signal(s, 't31_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_own_release('t31_sig'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            _delete_signal(s, 't31_sig')
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_OWN_NOT_HELD,
              f'err_code is ERR_OWN_NOT_HELD (got {fmt_err(code)})')
        _delete_signal(s, 't31_sig')


def test_own_transfer(host, port):
    """Test 32 — Ownership transfer: session 2 acquires unlocked signal owned by session 1;
    session 1 receives NOTIFY_OWN_REVOKED."""
    print('\n[32] Ownership transfer — session 2 acquires session 1\'s unlocked signal;'
          ' session 1 gets NOTIFY_OWN_REVOKED')
    s1, _ = open_session(host, port, 'own-transfer-s1')
    if s1 is None:
        check(False, 'could not establish session 1')
        return

    try:
        if not _define_signal(s1, 't32_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s1.sendall(make_own_acquire('t32_sig'))
        if recv_frame(s1) is None:
            check(False, 'OWN_ACQUIRE prerequisite failed')
            _delete_signal(s1, 't32_sig')
            return

        with open_session(host, port, 'own-transfer-s2')[0] as s2:
            s2.sendall(make_own_acquire('t32_sig'))
            ack = recv_frame(s2)
            print_response(ack)
            if not check(ack is not None, 'session 2 received a response to OWN_ACQUIRE'):
                return
            _, msg_type, _ = ack
            check(msg_type == MSG_OWN_ACK,
                  f'session 2 OWN_ACQUIRE succeeds (got {fmt_type(msg_type)})')

            # Session 1 should receive NOTIFY_OWN_REVOKED
            s1.settimeout(2)
            try:
                notif = recv_frame(s1)
                print_response(notif)
                if not check(notif is not None, 'session 1 received NOTIFY_OWN_REVOKED'):
                    return
                _, ntype, npayload = notif
                check(ntype == MSG_NOTIFY_OWN_REVOKED,
                      f'notification is NOTIFY_OWN_REVOKED (got {fmt_type(ntype)})')
                if npayload:
                    nlen = npayload[0]
                    nname = npayload[1:1 + nlen].decode(errors='replace')
                    check(nname == 't32_sig',
                          f'notification names t32_sig (got {nname!r})')
            except OSError:
                check(False, 'session 1 timed out waiting for NOTIFY_OWN_REVOKED')

            s2.sendall(make_own_release('t32_sig'))
            recv_frame(s2)

    finally:
        _delete_signal(s1, 't32_sig')
        s1.close()


def test_own_locked_not_available(host, port):
    """Test 33 — Locked signal cannot be acquired by another session → ERR_OWN_NOT_AVAILABLE."""
    print('\n[33] OWN_ACQUIRE on locked signal — expect ERR_OWN_NOT_AVAILABLE')
    s1, _ = open_session(host, port, 'own-lock-s1')
    if s1 is None:
        check(False, 'could not establish session 1')
        return

    try:
        if not _define_signal(s1, 't33_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s1.sendall(make_own_acquire('t33_sig'))
        if recv_frame(s1) is None:
            check(False, 'OWN_ACQUIRE prerequisite failed')
            _delete_signal(s1, 't33_sig')
            return
        s1.sendall(make_own_lock('t33_sig'))
        if recv_frame(s1) is None:
            check(False, 'OWN_LOCK prerequisite failed')
            _delete_signal(s1, 't33_sig')
            return

        with open_session(host, port, 'own-lock-s2')[0] as s2:
            s2.sendall(make_own_acquire('t33_sig'))
            frame = recv_frame(s2)
            print_response(frame)
            if not check(frame is not None, 'session 2 received a response'):
                return
            _, msg_type, payload = frame
            check(msg_type == MSG_ERR,
                  f'response is MSG_ERR (got {fmt_type(msg_type)})')
            code, _ = decode_err(payload)
            check(code == ERR_OWN_NOT_AVAILABLE,
                  f'err_code is ERR_OWN_NOT_AVAILABLE (got {fmt_err(code)})')

        # Cleanup: unlock and release before deleting
        s1.sendall(make_own_unlock('t33_sig'))
        recv_frame(s1)
        s1.sendall(make_own_release('t33_sig'))
        recv_frame(s1)
        _delete_signal(s1, 't33_sig')

    finally:
        s1.close()


def test_own_lock_unlock(host, port):
    """Test 34 — OWN_LOCK / OWN_UNLOCK by owner → OWN_ACK each."""
    print('\n[34] OWN_LOCK / OWN_UNLOCK by owner — expect OWN_ACK each')
    with open_session(host, port, 'own-lock-unlock')[0] as s:
        if not _define_signal(s, 't34_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_own_acquire('t34_sig'))
        if recv_frame(s) is None:
            check(False, 'OWN_ACQUIRE prerequisite failed')
            _delete_signal(s, 't34_sig')
            return

        s.sendall(make_own_lock('t34_sig'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received response to OWN_LOCK'):
            _delete_signal(s, 't34_sig')
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_OWN_ACK,
              f'OWN_LOCK → OWN_ACK (got {fmt_type(msg_type)})')

        s.sendall(make_own_unlock('t34_sig'))
        frame2 = recv_frame(s)
        print_response(frame2)
        if not check(frame2 is not None, 'received response to OWN_UNLOCK'):
            _delete_signal(s, 't34_sig')
            return
        _, msg_type2, _ = frame2
        check(msg_type2 == MSG_OWN_ACK,
              f'OWN_UNLOCK → OWN_ACK (got {fmt_type(msg_type2)})')

        s.sendall(make_own_release('t34_sig'))
        recv_frame(s)
        _delete_signal(s, 't34_sig')


def test_own_lock_not_held(host, port):
    """Test 35 — OWN_LOCK without ownership → ERR_OWN_NOT_HELD."""
    print('\n[35] OWN_LOCK without ownership — expect ERR_OWN_NOT_HELD')
    with open_session(host, port, 'own-lock-notheld')[0] as s:
        if not _define_signal(s, 't35_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s.sendall(make_own_lock('t35_sig'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            _delete_signal(s, 't35_sig')
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_OWN_NOT_HELD,
              f'err_code is ERR_OWN_NOT_HELD (got {fmt_err(code)})')
        _delete_signal(s, 't35_sig')


def test_own_session_close_releases(host, port):
    """Test 36 — Session close releases ownership; another session can acquire."""
    print('\n[36] Session close releases ownership — second session can acquire after first closes')
    s1, _ = open_session(host, port, 'own-close-s1')
    if s1 is None:
        check(False, 'could not establish session 1')
        return

    try:
        if not _define_signal(s1, 't36_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return
        s1.sendall(make_own_acquire('t36_sig'))
        if recv_frame(s1) is None:
            check(False, 'OWN_ACQUIRE prerequisite failed')
            _delete_signal(s1, 't36_sig')
            return
        s1.sendall(make_frame(PROTO_VERSION, MSG_SESSION_CLOSE))
        recv_frame(s1)
    finally:
        s1.close()

    # New session: ownership now available
    with open_session(host, port, 'own-close-s2')[0] as s2:
        s2.sendall(make_own_acquire('t36_sig'))
        frame = recv_frame(s2)
        print_response(frame)
        if not check(frame is not None, 'session 2 received a response'):
            _delete_signal(s2, 't36_sig')
            return
        _, msg_type, _ = frame
        check(msg_type == MSG_OWN_ACK,
              f'ownership available after session close (got {fmt_type(msg_type)})')
        s2.sendall(make_own_release('t36_sig'))
        recv_frame(s2)
        _delete_signal(s2, 't36_sig')


# ── Configuration persistence helpers ────────────────────────────────────────

def make_cfg_save(name):
    """Build a CFG_SAVE frame (SPEC §9.3)."""
    n = name.encode()
    return make_frame(PROTO_VERSION, MSG_CFG_SAVE, bytes([len(n)]) + n)


def make_cfg_load(name):
    """Build a CFG_LOAD frame (SPEC §9.4)."""
    n = name.encode()
    return make_frame(PROTO_VERSION, MSG_CFG_LOAD, bytes([len(n)]) + n)


# ── Configuration persistence tests (SPEC §9) ─────────────────────────────────

def test_cfg_save_load_roundtrip(host, port):
    """Test 40 — CFG_SAVE / CFG_LOAD round-trip.

    Defines a signal, saves the store, deletes the signal, reloads, and
    verifies the signal is accessible again.  Self-skips when the server has
    no storage directory (CFG_SAVE returns ERR_CFG_IO).
    """
    print('\n[40] CFG_SAVE/LOAD round-trip — save defs, delete, reload, verify')
    with open_session(host, port, 'cfg-roundtrip')[0] as s:
        if not _define_signal(s, 't40_sig'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return

        # Save
        s.sendall(make_cfg_save('t40_rt'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response to CFG_SAVE'):
            def_cleanup(s, ('t40_sig', DEF_TYPE_SIGNAL))
            return
        _, msg_type, payload = frame
        if msg_type == MSG_ERR:
            code, _ = decode_err(payload)
            if code == ERR_CFG_IO:
                print('  [skip] server has no storage dir — start with --storage-dir')
                def_cleanup(s, ('t40_sig', DEF_TYPE_SIGNAL))
                return
        if not check(msg_type == MSG_CFG_ACK,
                     f'CFG_SAVE → CFG_ACK (got {fmt_type(msg_type)})'):
            def_cleanup(s, ('t40_sig', DEF_TYPE_SIGNAL))
            return

        # Delete the signal (no deps)
        s.sendall(make_def_delete('t40_sig', DEF_TYPE_SIGNAL))
        if recv_frame(s) is None:
            check(False, 'DEF_DELETE prerequisite failed')
            return

        # Reload
        s.sendall(make_cfg_load('t40_rt'))
        frame2 = recv_frame(s)
        print_response(frame2)
        if not check(frame2 is not None, 'received a response to CFG_LOAD'):
            return
        _, msg_type2, _ = frame2
        if not check(msg_type2 == MSG_CFG_ACK,
                     f'CFG_LOAD → CFG_ACK (got {fmt_type(msg_type2)})'):
            return

        # Verify signal is back by defining a PDU referencing it
        s.sendall(make_def_pdu('t40_pdu', 8, [('t40_sig', 0)]))
        frame3 = recv_frame(s)
        if not check(frame3 is not None and frame3[1] == MSG_DEF_ACK,
                     'signal accessible after reload (DEF_PDU → DEF_ACK)'):
            def_cleanup(s, ('t40_sig', DEF_TYPE_SIGNAL))
            return

        def_cleanup(s, ('t40_sig', DEF_TYPE_SIGNAL), ('t40_pdu', DEF_TYPE_PDU))


def test_cfg_load_nonexistent(host, port):
    """Test 41 — CFG_LOAD of a nonexistent config name → ERR_CFG_IO."""
    print('\n[41] CFG_LOAD nonexistent name — expect ERR_CFG_IO')
    with open_session(host, port, 'cfg-nofile')[0] as s:
        s.sendall(make_cfg_load('t41_no_such_config_xyz'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_CFG_IO,
              f'err_code is ERR_CFG_IO (got {fmt_err(code)})')


def test_cfg_load_conflict(host, port):
    """Test 42 — CFG_LOAD conflicts with in-memory type → ERR_CFG_CONFLICT.

    Saves a signal named 't42_name', then redefines 't42_name' as a PDU, then
    loads the saved file — the file has a signal where memory has a PDU.
    Self-skips when the server has no storage directory.
    """
    print('\n[42] CFG_LOAD conflict — file has signal X, memory has PDU X → ERR_CFG_CONFLICT')
    with open_session(host, port, 'cfg-conflict')[0] as s:
        # Define signal t42_name
        if not _define_signal(s, 't42_name'):
            check(False, 'DEF_SIGNAL prerequisite failed')
            return

        # Save (file will contain signal t42_name)
        s.sendall(make_cfg_save('t42_conf'))
        frame = recv_frame(s)
        if frame is None:
            check(False, 'CFG_SAVE prerequisite: no response')
            def_cleanup(s, ('t42_name', DEF_TYPE_SIGNAL))
            return
        _, msg_type, payload = frame
        if msg_type == MSG_ERR:
            code, _ = decode_err(payload)
            if code == ERR_CFG_IO:
                print('  [skip] server has no storage dir — start with --storage-dir')
                def_cleanup(s, ('t42_name', DEF_TYPE_SIGNAL))
                return
        if msg_type != MSG_CFG_ACK:
            check(False, f'CFG_SAVE prerequisite failed (got {fmt_type(msg_type)})')
            def_cleanup(s, ('t42_name', DEF_TYPE_SIGNAL))
            return

        # Delete signal t42_name so we can redefine it as a PDU
        s.sendall(make_def_delete('t42_name', DEF_TYPE_SIGNAL))
        if recv_frame(s) is None:
            check(False, 'DEF_DELETE prerequisite failed')
            return

        # Define t42_helper (needed for the PDU to reference)
        if not _define_signal(s, 't42_helper'):
            check(False, 'DEF_SIGNAL (helper) prerequisite failed')
            return

        # Redefine t42_name as a PDU
        s.sendall(make_def_pdu('t42_name', 8, [('t42_helper', 0)]))
        if recv_frame(s) is None:
            check(False, 'DEF_PDU prerequisite failed')
            def_cleanup(s, ('t42_helper', DEF_TYPE_SIGNAL))
            return

        # Load — file has signal t42_name, memory has PDU t42_name → conflict
        s.sendall(make_cfg_load('t42_conf'))
        frame2 = recv_frame(s)
        print_response(frame2)
        if not check(frame2 is not None, 'received a response to CFG_LOAD'):
            def_cleanup(s, ('t42_helper', DEF_TYPE_SIGNAL),
                        ('t42_name', DEF_TYPE_PDU))
            return
        _, msg_type2, payload2 = frame2
        check(msg_type2 == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type2)})')
        code2, _ = decode_err(payload2)
        check(code2 == ERR_CFG_CONFLICT,
              f'err_code is ERR_CFG_CONFLICT (got {fmt_err(code2)})')

        def_cleanup(s, ('t42_helper', DEF_TYPE_SIGNAL), ('t42_name', DEF_TYPE_PDU))


def test_cfg_load_bad_checksum(host, port):
    """Test 43 — CFG_LOAD of a file with a corrupted CRC → ERR_CFG_CHECKSUM.

    Writes a minimal .ashcfg file with a deliberately wrong CRC into the
    storage directory.  Requires --storage-dir to match the server's
    --storage-dir; self-skips otherwise.
    """
    print('\n[43] CFG_LOAD bad checksum — expect ERR_CFG_CHECKSUM'
          ' (skipped if --storage-dir not provided)')
    if not _storage_dir:
        print('  [skip] --storage-dir not provided')
        return

    # Write a file with valid header fields but a wrong CRC.
    # Note: CRC32 of an empty body is 0x00000000 (0xFFFFFFFF ^ 0xFFFFFFFF),
    # so 0x00000000 would actually pass validation.  Use 0xDEADBEEF instead.
    path = os.path.join(_storage_dir, 't43_badcrc.ashcfg')
    magic   = 0x41534843
    version = 0x0001
    count   = 0
    bad_crc = 0xDEADBEEF   # intentionally wrong (correct CRC for empty body = 0x00000000)
    # '>IHHI': magic(uint32) + version(uint16) + count(uint16) + crc(uint32)
    header = struct.pack('>I', magic) + struct.pack('>H', version) + \
             struct.pack('>H', count) + struct.pack('>I', bad_crc)

    try:
        os.makedirs(_storage_dir, exist_ok=True)
        with open(path, 'wb') as f:
            f.write(header)
    except OSError as e:
        print(f'  [skip] cannot write to storage dir: {e}')
        return

    with open_session(host, port, 'cfg-badcrc')[0] as s:
        s.sendall(make_cfg_load('t43_badcrc'))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_CFG_CHECKSUM,
              f'err_code is ERR_CFG_CHECKSUM (got {fmt_err(code)})')

    try:
        os.remove(path)
    except OSError:
        pass


# ── CAP_NET_ADMIN tests ───────────────────────────────────────────────────────

ERR_PERMISSION_DENIED = 0x0013


def test_cap_net_admin_vcan_create(host, port):
    """Test 37 — IFACE_VCAN_CREATE without CAP_NET_ADMIN → ERR_PERMISSION_DENIED.

    Skipped automatically when the server holds CAP_NET_ADMIN (operation
    succeeds instead and is cleaned up).
    """
    print('\n[37] IFACE_VCAN_CREATE without CAP_NET_ADMIN — expect ERR_PERMISSION_DENIED'
          ' (skipped if server has CAP_NET_ADMIN)')
    with open_session(host, port, 'cap-vcan-create')[0] as s:
        s.sendall(make_iface_vcan_create(TEST_VCAN))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        if msg_type == MSG_IFACE_VCAN_ACK:
            print('  [skip] server holds CAP_NET_ADMIN')
            s.sendall(make_iface_vcan_destroy(TEST_VCAN))
            recv_frame(s)
            return
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PERMISSION_DENIED,
              f'err_code is ERR_PERMISSION_DENIED (got {fmt_err(code)})')


def test_cap_net_admin_vcan_destroy(host, port):
    """Test 38 — IFACE_VCAN_DESTROY without CAP_NET_ADMIN → ERR_PERMISSION_DENIED.

    Skipped automatically when the server holds CAP_NET_ADMIN.
    """
    print('\n[38] IFACE_VCAN_DESTROY without CAP_NET_ADMIN — expect ERR_PERMISSION_DENIED'
          ' (skipped if server has CAP_NET_ADMIN)')
    with open_session(host, port, 'cap-vcan-destroy')[0] as s:
        s.sendall(make_iface_vcan_destroy(TEST_VCAN))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        if msg_type == MSG_IFACE_VCAN_ACK:
            print('  [skip] server holds CAP_NET_ADMIN')
            return
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PERMISSION_DENIED,
              f'err_code is ERR_PERMISSION_DENIED (got {fmt_err(code)})')


def test_cap_net_admin_attach_bitrate(host, port):
    """Test 39 — IFACE_ATTACH with non-zero bitrate without CAP_NET_ADMIN →
    ERR_PERMISSION_DENIED.

    Uses IFACE_VCAN_CREATE as a capability probe: if it succeeds the server
    holds CAP_NET_ADMIN and the test is skipped.  If the server lacks the
    capability, attempts IFACE_ATTACH with bitrate=500000 on any listed
    interface and expects ERR_PERMISSION_DENIED.
    """
    print('\n[39] IFACE_ATTACH with bitrate≠0 without CAP_NET_ADMIN — expect ERR_PERMISSION_DENIED'
          ' (skipped if server has CAP_NET_ADMIN)')
    with open_session(host, port, 'cap-attach-bitrate')[0] as s:
        # Probe capability via VCAN_CREATE
        s.sendall(make_iface_vcan_create('cap_probe99'))
        probe = recv_frame(s)
        if probe and probe[1] == MSG_IFACE_VCAN_ACK:
            print('  [skip] server holds CAP_NET_ADMIN')
            s.sendall(make_iface_vcan_destroy('cap_probe99'))
            recv_frame(s)
            return

        # Server lacks CAP_NET_ADMIN — find any interface to test with
        s.sendall(make_iface_list())
        list_frame = recv_frame(s)
        if list_frame is None or list_frame[1] != MSG_IFACE_LIST_RESP:
            print('  [skip] could not list interfaces')
            return
        ifaces = parse_iface_list_resp(list_frame[2])
        if not ifaces:
            print('  [skip] no interfaces available')
            return

        iface_name, _ = ifaces[0]
        s.sendall(make_iface_attach(iface_name, bitrate=500000))
        frame = recv_frame(s)
        print_response(frame)
        if not check(frame is not None, 'received a response'):
            return
        _, msg_type, payload = frame
        check(msg_type == MSG_ERR,
              f'response is MSG_ERR (got {fmt_type(msg_type)})')
        code, _ = decode_err(payload)
        check(code == ERR_PERMISSION_DENIED,
              f'err_code is ERR_PERMISSION_DENIED (got {fmt_err(code)})')


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
    test_def_signal,                    # 18
    test_def_pdu,                       # 19
    test_def_frame,                     # 20
    test_def_pdu_unknown_signal,        # 21
    test_def_frame_unknown_pdu,         # 22
    test_def_type_conflict,             # 23
    test_def_delete_signal_in_use,      # 24
    test_def_delete_pdu_in_use,         # 25
    test_def_pdu_signal_count_limit,    # 26
    test_def_frame_pdu_count_limit,     # 27
    test_def_persistence,               # 28
    test_own_acquire,                   # 29
    test_own_release,                   # 30
    test_own_release_not_held,          # 31
    test_own_transfer,                  # 32
    test_own_locked_not_available,      # 33
    test_own_lock_unlock,               # 34
    test_own_lock_not_held,             # 35
    test_own_session_close_releases,    # 36
    test_cap_net_admin_vcan_create,     # 37
    test_cap_net_admin_vcan_destroy,    # 38
    test_cap_net_admin_attach_bitrate,  # 39
    test_cfg_save_load_roundtrip,       # 40
    test_cfg_load_nonexistent,          # 41
    test_cfg_load_conflict,             # 42
    test_cfg_load_bad_checksum,         # 43
]

AUTO_TESTS = TESTS[:7] + TESTS[9:16] + TESTS[17:]  # 1-7, 10-16, and 18-43 run unattended


def main():
    global _storage_dir

    parser = argparse.ArgumentParser(
        description='ash protocol + session lifecycle test client')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=4000)
    parser.add_argument('--storage-dir', default=None,
                        help='path matching ash-server --storage-dir (needed for tests 40, 42, 43)')
    parser.add_argument('--test', type=int, choices=range(1, len(TESTS) + 1),
                        metavar='N', help=f'run only test N (1-{len(TESTS)})')
    args = parser.parse_args()

    _storage_dir = args.storage_dir

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
        print('     (Tests 18-43 run automatically above)')

    print()


if __name__ == '__main__':
    main()
