#!/usr/bin/env python3
"""
examples/drive_sequence.py — ash scripted drive sequence example

Usage:
    python3 drive_sequence.py [host] [port] [iface]
    Default: host=127.0.0.1, port=4000, iface=vcan0

Demonstrates: acquire ownership of a signal and drive it through a timed
sequence of values, illustrating write() and cyclic TX.
"""

import sys
import time
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "lib"))

from ash import (AshClient, AshError, SignalDef, PduDef, PduSignalMap,
                  FrameDef, FramePduMap)

HOST  = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT  = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
IFACE = sys.argv[3] if len(sys.argv) > 3 else "vcan0"

TX_PERIOD_MS = 100

# (value_pct, hold_s)
SEQUENCE = [
    (0.0,   0.2),
    (25.0,  0.5),
    (50.0,  0.5),
    (75.0,  0.5),
    (100.0, 1.0),
    (75.0,  0.3),
    (50.0,  0.3),
    (25.0,  0.3),
    (0.0,   0.5),
]


def main():
    client = AshClient(HOST, PORT, "drive-sequence-py")
    print(f"Connected to {HOST}:{PORT}")

    try:
        try:
            client.vcan_create(IFACE)
        except AshError:
            pass

        for name, kind in [("ThrottleFrame", "frame"),
                            ("ThrottlePDU",   "pdu"),
                            ("ThrottlePos",   "signal")]:
            try:
                client.delete_def(name, kind)
            except AshError:
                pass

        # ThrottlePos: 8-bit unsigned LE, 0.4 % per LSB, range 0–100 %
        client.define_signal(SignalDef(
            name="ThrottlePos", data_type="uint", byte_order="LE",
            bit_length=8, scale=0.4, min=0.0, max=100.0,
        ))
        client.define_pdu(PduDef(
            name="ThrottlePDU", length=8,
            signals=[PduSignalMap("ThrottlePos", 0)],
        ))
        client.define_frame(FrameDef(
            name="ThrottleFrame", can_id=0x300, id_type="std", dlc=8,
            tx_period_ms=TX_PERIOD_MS,
            pdus=[FramePduMap("ThrottlePDU", 0)],
        ))

        client.iface_attach(IFACE, "2.0B", 0)
        client.acquire("ThrottlePos", on_disconnect="default")
        print(f"Driving ThrottlePos through sequence (cyclic {TX_PERIOD_MS} ms)")

        for value, hold in SEQUENCE:
            client.write("ThrottlePos", value)
            print(f"  ThrottlePos → {value:5.1f} %  (hold {hold:.1f} s)")
            time.sleep(hold)

        print("Sequence complete")

    finally:
        try:
            client.release("ThrottlePos")
        except AshError:
            pass
        try:
            client.iface_detach(IFACE)
        except AshError:
            pass
        client.disconnect()


if __name__ == "__main__":
    main()
