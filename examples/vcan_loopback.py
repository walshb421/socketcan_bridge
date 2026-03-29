#!/usr/bin/env python3
"""
examples/vcan_loopback.py — ash vcan loopback integration test

Usage:
    python3 vcan_loopback.py [host] [port]
    Default: host=127.0.0.1, port=4000

Demonstrates: self-contained integration test using two sequential client
sessions.  The writer session acquires a signal and writes a value; the server
encodes it into a CAN frame and transmits on vcan0 — the vcan driver loops the
frame back to the server, which decodes it and stores the updated value.  The
reader session then attaches and reads the value back, confirming the full
TX → vcan loopback → RX → decode round-trip.

Note: the server enforces exclusive interface attachment, so the writer
disconnects before the reader connects.
"""

import sys
import math
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lib"))

from ash import (AshClient, AshError, SignalDef, PduDef, PduSignalMap,
                  FrameDef, FramePduMap)

HOST       = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT       = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
IFACE      = "vcan0"
SIGNAL     = "BattVolt"
TEST_VALUE = 12.8   # volts
SCALE      = 0.001  # V per LSB  →  1 LSB tolerance for pass/fail


def writer_session():
    """Session A: define signal, write value, disconnect."""
    print("=== Writer session ===")
    client = AshClient(HOST, PORT, "loopback-writer-py")

    try:
        client.vcan_create(IFACE)
    except AshError:
        pass

    # Clean up leftover defs from a prior run
    for name, kind in [("BattFrame", "frame"), ("BattPDU", "pdu"),
                        (SIGNAL, "signal")]:
        try:
            client.delete_def(name, kind)
        except AshError:
            pass

    # BattVolt: 16-bit unsigned LE, 0.001 V/LSB, range 0–65.535 V
    client.define_signal(SignalDef(
        name=SIGNAL, data_type="uint", byte_order="LE",
        bit_length=16, scale=SCALE, min=0.0, max=65.535,
    ))
    client.define_pdu(PduDef(
        name="BattPDU", length=8,
        signals=[PduSignalMap(SIGNAL, 0)],
    ))
    client.define_frame(FrameDef(
        name="BattFrame", can_id=0x400, id_type="std", dlc=8,
        pdus=[FramePduMap("BattPDU", 0)],
    ))

    client.iface_attach(IFACE, "2.0B", 0)
    client.acquire(SIGNAL, on_disconnect="last")
    client.write(SIGNAL, TEST_VALUE)
    print(f"  Wrote  {SIGNAL:<12s} = {TEST_VALUE:.3f} V")

    client.release(SIGNAL)
    client.iface_detach(IFACE)
    client.disconnect()
    print("  Writer disconnected")


def reader_session() -> float:
    """Session B: attach, read the value written by Session A, return it."""
    print("=== Reader session ===")
    client = AshClient(HOST, PORT, "loopback-reader-py")

    try:
        client.iface_attach(IFACE, "2.0B", 0)
        value = client.read(SIGNAL)
        print(f"  Read   {SIGNAL:<12s} = {value:.3f} V")
        return value
    finally:
        try:
            client.iface_detach(IFACE)
        except AshError:
            pass
        client.disconnect()


def main():
    writer_session()
    read_val = reader_session()

    tolerance = SCALE  # one LSB
    if math.fabs(read_val - TEST_VALUE) <= tolerance:
        print(f"PASS  (|{read_val:.3f} - {TEST_VALUE:.3f}| <= {tolerance:.3f})")
        sys.exit(0)
    else:
        print(f"FAIL  (|{read_val:.3f} - {TEST_VALUE:.3f}| > {tolerance:.3f})",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
