#!/usr/bin/env python3
"""
examples/signal_monitor.py — ash signal monitor example

Usage:
    python3 signal_monitor.py [host] [port] [iface]
    Default: host=127.0.0.1, port=4000, iface=vcan0

Demonstrates: attach to a CAN interface, define a cyclic signal, then print
decoded SIG_RX values to stdout as they arrive.

This is a passive monitor.  It defines WheelSpeed and starts cyclic TX, but
SIG_RX events are only delivered when a frame is received from the bus.  On
vcan0 the server does not receive its own transmitted frames, so run
drive_sequence.py (or another client writing to a frame on the same interface)
in a second terminal to generate observable traffic.
"""

import sys
import signal
import time

# Allow running from the repo root without installing the library
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..","lib"))

from ash import (AshClient, AshError, SignalDef, PduDef, PduSignalMap,
                  FrameDef, FramePduMap)

HOST          = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT          = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
IFACE         = sys.argv[3] if len(sys.argv) > 3 else "vcan0"
DURATION_S    = 5
TX_PERIOD_MS  = 50

stop = False

def _sigint(sig, frame):
    global stop
    stop = True

signal.signal(signal.SIGINT, _sigint)


def main():
    client = AshClient(HOST, PORT, "signal-monitor-py")
    print(f"Connected to {HOST}:{PORT}")

    try:
        # Ensure the vcan interface exists
        try:
            client.vcan_create(IFACE)
        except AshError:
            pass  # already exists

        # Clean up any leftover definitions
        for name, kind in [("MonitorFrame", "frame"),
                            ("MonitorPDU",   "pdu"),
                            ("WheelSpeed",   "signal")]:
            try:
                client.delete_def(name, kind)
            except AshError:
                pass

        # Define signal: WheelSpeed — 16-bit unsigned LE, 0.1 km/h per LSB
        client.define_signal(SignalDef(
            name="WheelSpeed", data_type="uint", byte_order="LE",
            bit_length=16, scale=0.1, min=0.0, max=6553.5,
        ))
        client.define_pdu(PduDef(
            name="MonitorPDU", length=8,
            signals=[PduSignalMap("WheelSpeed", 0)],
        ))
        # Cyclic frame — server retransmits at TX_PERIOD_MS automatically
        client.define_frame(FrameDef(
            name="MonitorFrame", can_id=0x200, id_type="std", dlc=8,
            tx_period_ms=TX_PERIOD_MS,
            pdus=[FramePduMap("MonitorPDU", 0)],
        ))

        client.iface_attach(IFACE, "2.0B", 0)
        print(f"Attached to {IFACE}")

        client.acquire("WheelSpeed", on_disconnect="stop")
        client.write("WheelSpeed", 80.0)  # seed value — starts cyclic TX
        print(f"Monitoring WheelSpeed (cyclic {TX_PERIOD_MS} ms) "
              f"for {DURATION_S} s — press Ctrl+C to stop")

        events   = 0
        deadline = time.monotonic() + DURATION_S

        while not stop and time.monotonic() < deadline:
            ev = client.poll(timeout=0.2)
            if ev is None:
                continue
            if ev.type == "sig_rx":
                print(f"  SIG_RX  {ev.signal:<20s} = {ev.value:.1f} km/h")
                events += 1
            elif ev.type == "iface_down":
                print(f"  IFACE_DOWN: {ev.iface}")
                break
            elif ev.type == "server_close":
                print("  SERVER_CLOSE")
                break

        print(f"Received {events} SIG_RX event(s)")

    finally:
        try:
            client.release("WheelSpeed")
        except AshError:
            pass
        try:
            client.iface_detach(IFACE)
        except AshError:
            pass
        client.disconnect()


if __name__ == "__main__":
    main()
