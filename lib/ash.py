"""ash — Python client library for the ash CAN bridge server  (SPEC §12.2)

Wraps *libash* via :mod:`ctypes`.  No C compilation required at runtime.

Quick start::

    from ash import AshClient, SignalDef, PduDef, PduSignalMap, FrameDef, FramePduMap

    with AshClient("127.0.0.1", 4000, "my-client") as client:
        client.define_signal(SignalDef("EngineRPM", "uint", "LE", 16,
                                       scale=0.25, max=16383.75))
        client.define_pdu(PduDef("EnginePDU", 8,
                                  [PduSignalMap("EngineRPM", 0)]))
        client.define_frame(FrameDef("EngineFrame", 0x100, "std", 8,
                                      pdus=[FramePduMap("EnginePDU", 0)]))
        client.iface_attach("vcan0", "2.0B", 0)
        client.acquire("EngineRPM", on_disconnect="stop")
        client.write("EngineRPM", 3000.0)
        print(client.read("EngineRPM"))
"""

from __future__ import annotations

import ctypes
import ctypes.util
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Load libash
# ---------------------------------------------------------------------------

def _find_lib() -> ctypes.CDLL:
    here = Path(__file__).parent.resolve()
    for candidate in [
        here.parent / "build" / "libash.so",
        here / "libash.so",
        Path("/usr/local/lib/libash.so"),
    ]:
        if candidate.exists():
            return ctypes.CDLL(str(candidate))
    name = ctypes.util.find_library("ash")
    if name:
        return ctypes.CDLL(name)
    raise OSError("libash not found — build the project first: cmake --build build")


_lib = _find_lib()


# ---------------------------------------------------------------------------
# Internal C structure mirrors  (must match include/ash/ash.h exactly)
# ---------------------------------------------------------------------------

class _IfaceInfo(ctypes.Structure):
    _fields_ = [
        ("name",  ctypes.c_char * 64),
        ("state", ctypes.c_uint8),
    ]


class _PduSignalMap(ctypes.Structure):
    _fields_ = [
        ("signal_name", ctypes.c_char * 64),
        ("start_bit",   ctypes.c_uint8),
    ]


class _SignalDef(ctypes.Structure):
    _fields_ = [
        ("name",       ctypes.c_char * 64),
        ("data_type",  ctypes.c_uint8),
        ("byte_order", ctypes.c_uint8),
        ("bit_length", ctypes.c_uint8),
        ("scale",      ctypes.c_double),
        ("offset",     ctypes.c_double),
        ("min",        ctypes.c_double),
        ("max",        ctypes.c_double),
    ]


class _PduDef(ctypes.Structure):
    _fields_ = [
        ("name",         ctypes.c_char * 64),
        ("length",       ctypes.c_uint8),
        ("signal_count", ctypes.c_uint8),
        ("signals",      _PduSignalMap * 32),
    ]


class _FramePduMap(ctypes.Structure):
    _fields_ = [
        ("pdu_name",    ctypes.c_char * 64),
        ("byte_offset", ctypes.c_uint8),
    ]


class _FrameDef(ctypes.Structure):
    _fields_ = [
        ("name",         ctypes.c_char * 64),
        ("can_id",       ctypes.c_uint32),
        ("id_type",      ctypes.c_uint8),
        ("dlc",          ctypes.c_uint8),
        ("tx_period_ms", ctypes.c_uint16),
        ("pdu_count",    ctypes.c_uint8),
        ("pdus",         _FramePduMap * 8),
    ]


class _SigRx(ctypes.Structure):
    _fields_ = [
        ("signal", ctypes.c_char * 64),
        ("value",  ctypes.c_double),
    ]


class _FrameRx(ctypes.Structure):
    _fields_ = [
        ("can_id", ctypes.c_uint32),
        ("dlc",    ctypes.c_uint8),
        ("flags",  ctypes.c_uint8),
        ("data",   ctypes.c_uint8 * 64),
    ]


class _OwnRevoked(ctypes.Structure):
    _fields_ = [("signal", ctypes.c_char * 64)]


class _IfaceDown(ctypes.Structure):
    _fields_ = [("iface", ctypes.c_char * 16)]


class _AppErr(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint16)]


class _EventUnion(ctypes.Union):
    _fields_ = [
        ("sig_rx",      _SigRx),
        ("frame_rx",    _FrameRx),
        ("own_revoked", _OwnRevoked),
        ("iface_down",  _IfaceDown),
        ("app_err",     _AppErr),
    ]


class _Event(ctypes.Structure):
    _fields_ = [
        ("type",  ctypes.c_int),   # ash_event_type_t is a C enum (int)
        ("iface", ctypes.c_char * 16),
        ("u",     _EventUnion),
    ]


# ---------------------------------------------------------------------------
# Function signatures
# ---------------------------------------------------------------------------

_lib.ash_connect.restype   = ctypes.c_void_p
_lib.ash_connect.argtypes  = [ctypes.c_char_p, ctypes.c_uint16, ctypes.c_char_p]

_lib.ash_disconnect.restype  = None
_lib.ash_disconnect.argtypes = [ctypes.c_void_p]

_lib.ash_keepalive.restype   = ctypes.c_int
_lib.ash_keepalive.argtypes  = [ctypes.c_void_p]

_lib.ash_iface_list.restype  = ctypes.c_int
_lib.ash_iface_list.argtypes = [ctypes.c_void_p,
                                 ctypes.POINTER(_IfaceInfo),
                                 ctypes.c_size_t]

_lib.ash_iface_attach.restype  = ctypes.c_int
_lib.ash_iface_attach.argtypes = [ctypes.c_void_p,
                                   ctypes.c_char_p,
                                   ctypes.c_uint8,
                                   ctypes.c_uint32]

_lib.ash_iface_detach.restype  = ctypes.c_int
_lib.ash_iface_detach.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_vcan_create.restype  = ctypes.c_int
_lib.ash_vcan_create.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_vcan_destroy.restype  = ctypes.c_int
_lib.ash_vcan_destroy.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_define_signal.restype  = ctypes.c_int
_lib.ash_define_signal.argtypes = [ctypes.c_void_p, ctypes.POINTER(_SignalDef)]

_lib.ash_define_pdu.restype  = ctypes.c_int
_lib.ash_define_pdu.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PduDef)]

_lib.ash_define_frame.restype  = ctypes.c_int
_lib.ash_define_frame.argtypes = [ctypes.c_void_p, ctypes.POINTER(_FrameDef)]

_lib.ash_delete_def.restype  = ctypes.c_int
_lib.ash_delete_def.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8]

_lib.ash_acquire.restype  = ctypes.c_int
_lib.ash_acquire.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8]

_lib.ash_release.restype  = ctypes.c_int
_lib.ash_release.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_lock.restype  = ctypes.c_int
_lib.ash_lock.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_unlock.restype  = ctypes.c_int
_lib.ash_unlock.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_write.restype  = ctypes.c_int
_lib.ash_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]

_lib.ash_read.restype  = ctypes.c_int
_lib.ash_read.argtypes = [ctypes.c_void_p,
                           ctypes.c_char_p,
                           ctypes.POINTER(ctypes.c_double)]

_lib.ash_frame_tx.restype  = ctypes.c_int
_lib.ash_frame_tx.argtypes = [ctypes.c_void_p,
                               ctypes.c_char_p,
                               ctypes.c_uint32,
                               ctypes.c_uint8,
                               ctypes.c_uint8,
                               ctypes.c_char_p]

_lib.ash_poll.restype  = ctypes.c_int
_lib.ash_poll.argtypes = [ctypes.c_void_p,
                           ctypes.POINTER(_Event),
                           ctypes.c_int]

_lib.ash_cfg_save.restype  = ctypes.c_int
_lib.ash_cfg_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_lib.ash_cfg_load.restype  = ctypes.c_int
_lib.ash_cfg_load.argtypes = [ctypes.c_void_p, ctypes.c_char_p]


# ---------------------------------------------------------------------------
# Enum / constant maps
# ---------------------------------------------------------------------------

_MODE_STR        = {"2.0A": 0x01, "2.0B": 0x02, "FD": 0x03}
_ON_DISC_STR     = {"stop": 0x01, "last": 0x02, "default": 0x03}
_DEF_TYPE_STR    = {"signal": 0x01, "pdu": 0x02, "frame": 0x03}
_DATA_TYPE_STR   = {"uint": 0x01, "sint": 0x02, "float": 0x03}
_BYTE_ORDER_STR  = {"LE": 0x01, "BE": 0x02}
_ID_TYPE_STR     = {"std": 0x01, "ext": 0x02}

_IFACE_STATE_INT = {0x01: "available", 0x02: "mine", 0x03: "other"}
_EVENT_TYPE_INT  = {
    1: "sig_rx",
    2: "frame_rx",
    3: "own_revoked",
    4: "iface_down",
    5: "server_close",
    6: "app_err",
}


# ---------------------------------------------------------------------------
# Public data types
# ---------------------------------------------------------------------------

@dataclass
class IfaceInfo:
    """Information about a network interface (returned by :meth:`AshClient.iface_list`)."""
    name: str
    state: str  # "available", "mine", or "other"


@dataclass
class SignalDef:
    """Signal definition for :meth:`AshClient.define_signal`."""
    name: str
    data_type: str   # "uint", "sint", or "float"
    byte_order: str  # "LE" (Intel) or "BE" (Motorola)
    bit_length: int
    scale: float = 1.0
    offset: float = 0.0
    min: float = 0.0
    max: float = 0.0


@dataclass
class PduSignalMap:
    """Maps a signal into a PDU at a specific start bit."""
    signal_name: str
    start_bit: int


@dataclass
class PduDef:
    """PDU definition for :meth:`AshClient.define_pdu`."""
    name: str
    length: int              # byte length of the PDU payload
    signals: list[PduSignalMap]


@dataclass
class FramePduMap:
    """Maps a PDU into a frame at a specific byte offset."""
    pdu_name: str
    byte_offset: int


@dataclass
class FrameDef:
    """Frame definition for :meth:`AshClient.define_frame`."""
    name: str
    can_id: int
    id_type: str             # "std" (11-bit) or "ext" (29-bit)
    dlc: int
    tx_period_ms: int = 0    # 0 = event-driven; >0 = cyclic transmit period
    pdus: list[FramePduMap] = field(default_factory=list)


@dataclass
class AshEvent:
    """Event returned by :meth:`AshClient.poll`."""
    type: str      # "sig_rx", "frame_rx", "own_revoked", "iface_down", "server_close", "app_err"
    iface: str     # interface name (if applicable)
    # sig_rx and own_revoked
    signal: Optional[str]   = None
    value: Optional[float]  = None
    # frame_rx
    can_id: Optional[int]   = None
    dlc: Optional[int]      = None
    flags: Optional[int]    = None
    data: Optional[bytes]   = None
    # app_err
    code: Optional[int]     = None


class AshError(Exception):
    """Raised when a libash call returns an error.

    Attributes:
        code: Positive errno value from the failed C call.
    """
    def __init__(self, code: int, msg: str = ""):
        self.code = code
        super().__init__(msg or f"ash error {code}")


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _check(ret: int, fn: str = "") -> int:
    if ret < 0:
        raise AshError(-ret, f"{fn} failed (errno {-ret})")
    return ret


def _enc(s: str | bytes) -> bytes:
    return s.encode() if isinstance(s, str) else s


def _decode_event(ev: _Event) -> AshEvent:
    etype = _EVENT_TYPE_INT.get(ev.type, str(ev.type))
    iface = ev.iface.decode()
    if ev.type == 1:  # sig_rx
        return AshEvent(type=etype, iface=iface,
                        signal=ev.u.sig_rx.signal.decode(),
                        value=ev.u.sig_rx.value)
    if ev.type == 2:  # frame_rx
        fr = ev.u.frame_rx
        return AshEvent(type=etype, iface=iface,
                        can_id=fr.can_id, dlc=fr.dlc, flags=fr.flags,
                        data=bytes(fr.data[:fr.dlc]))
    if ev.type == 3:  # own_revoked
        return AshEvent(type=etype, iface=iface,
                        signal=ev.u.own_revoked.signal.decode())
    if ev.type == 4:  # iface_down
        return AshEvent(type=etype, iface=iface)
    if ev.type == 5:  # server_close
        return AshEvent(type=etype, iface=iface)
    if ev.type == 6:  # app_err
        return AshEvent(type=etype, iface=iface, code=ev.u.app_err.code)
    return AshEvent(type=etype, iface=iface)


# ---------------------------------------------------------------------------
# AshClient
# ---------------------------------------------------------------------------

class AshClient:
    """Client for the ash CAN bridge server (SPEC §12.2).

    Supports use as a context manager::

        with AshClient("127.0.0.1") as c:
            c.iface_attach("vcan0", "2.0B", 0)
            ...
    """

    def __init__(self, host: str, port: int = 4000, name: str = ""):
        ctx = _lib.ash_connect(_enc(host), port, _enc(name))
        if not ctx:
            raise AshError(0, f"ash_connect to {host}:{port} failed")
        self._ctx = ctx

    def disconnect(self) -> None:
        """Close the connection and free all resources."""
        if self._ctx:
            _lib.ash_disconnect(self._ctx)
            self._ctx = None

    def __enter__(self) -> "AshClient":
        return self

    def __exit__(self, *_) -> None:
        self.disconnect()

    # ------------------------------------------------------------------
    # Interface management
    # ------------------------------------------------------------------

    def iface_list(self) -> list[IfaceInfo]:
        """Return a list of available network interfaces."""
        arr = (_IfaceInfo * 64)()
        count = _check(_lib.ash_iface_list(self._ctx, arr, 64), "ash_iface_list")
        return [
            IfaceInfo(
                name=arr[i].name.decode(),
                state=_IFACE_STATE_INT.get(arr[i].state, str(arr[i].state)),
            )
            for i in range(count)
        ]

    def iface_attach(self, iface: str, mode: str, bitrate: int) -> None:
        """Attach to a network interface.

        :param iface:   Interface name, e.g. ``"vcan0"`` or ``"can0"``.
        :param mode:    ``"2.0A"``, ``"2.0B"``, or ``"FD"``.
        :param bitrate: Bitrate in bits/s; use ``0`` for virtual interfaces.
        """
        m = _MODE_STR.get(mode)
        if m is None:
            raise ValueError(f"Unknown mode {mode!r}; expected '2.0A', '2.0B', or 'FD'")
        _check(_lib.ash_iface_attach(self._ctx, _enc(iface), m, bitrate),
               "ash_iface_attach")

    def iface_detach(self, iface: str) -> None:
        """Detach from a network interface."""
        _check(_lib.ash_iface_detach(self._ctx, _enc(iface)), "ash_iface_detach")

    def vcan_create(self, name: str) -> None:
        """Create a virtual CAN interface."""
        _check(_lib.ash_vcan_create(self._ctx, _enc(name)), "ash_vcan_create")

    def vcan_destroy(self, name: str) -> None:
        """Destroy a virtual CAN interface."""
        _check(_lib.ash_vcan_destroy(self._ctx, _enc(name)), "ash_vcan_destroy")

    # ------------------------------------------------------------------
    # Definitions
    # ------------------------------------------------------------------

    def define_signal(self, sig: SignalDef) -> None:
        """Define a signal."""
        dt = _DATA_TYPE_STR.get(sig.data_type)
        if dt is None:
            raise ValueError(f"Unknown data_type {sig.data_type!r}")
        bo = _BYTE_ORDER_STR.get(sig.byte_order)
        if bo is None:
            raise ValueError(f"Unknown byte_order {sig.byte_order!r}")
        c = _SignalDef(
            name=_enc(sig.name),
            data_type=dt,
            byte_order=bo,
            bit_length=sig.bit_length,
            scale=sig.scale,
            offset=sig.offset,
            min=sig.min,
            max=sig.max,
        )
        _check(_lib.ash_define_signal(self._ctx, ctypes.byref(c)), "ash_define_signal")

    def define_pdu(self, pdu: PduDef) -> None:
        """Define a PDU."""
        c = _PduDef(
            name=_enc(pdu.name),
            length=pdu.length,
            signal_count=len(pdu.signals),
        )
        for i, sm in enumerate(pdu.signals):
            c.signals[i].signal_name = _enc(sm.signal_name)
            c.signals[i].start_bit   = sm.start_bit
        _check(_lib.ash_define_pdu(self._ctx, ctypes.byref(c)), "ash_define_pdu")

    def define_frame(self, frame: FrameDef) -> None:
        """Define a frame."""
        it = _ID_TYPE_STR.get(frame.id_type)
        if it is None:
            raise ValueError(f"Unknown id_type {frame.id_type!r}")
        c = _FrameDef(
            name=_enc(frame.name),
            can_id=frame.can_id,
            id_type=it,
            dlc=frame.dlc,
            tx_period_ms=frame.tx_period_ms,
            pdu_count=len(frame.pdus),
        )
        for i, pm in enumerate(frame.pdus):
            c.pdus[i].pdu_name    = _enc(pm.pdu_name)
            c.pdus[i].byte_offset = pm.byte_offset
        _check(_lib.ash_define_frame(self._ctx, ctypes.byref(c)), "ash_define_frame")

    def delete_def(self, name: str, def_type: str) -> None:
        """Delete a signal, PDU, or frame definition.

        :param def_type: ``"signal"``, ``"pdu"``, or ``"frame"``.
        """
        dt = _DEF_TYPE_STR.get(def_type)
        if dt is None:
            raise ValueError(f"Unknown def_type {def_type!r}")
        _check(_lib.ash_delete_def(self._ctx, _enc(name), dt), "ash_delete_def")

    # ------------------------------------------------------------------
    # Ownership
    # ------------------------------------------------------------------

    def acquire(self, signal: str, on_disconnect: str = "stop") -> None:
        """Acquire exclusive write ownership of a signal.

        :param on_disconnect: ``"stop"``, ``"last"``, or ``"default"``.
        """
        od = _ON_DISC_STR.get(on_disconnect)
        if od is None:
            raise ValueError(f"Unknown on_disconnect {on_disconnect!r}")
        _check(_lib.ash_acquire(self._ctx, _enc(signal), od), "ash_acquire")

    def release(self, signal: str) -> None:
        """Release write ownership of a signal."""
        _check(_lib.ash_release(self._ctx, _enc(signal)), "ash_release")

    def lock(self, signal: str) -> None:
        """Lock a signal, preventing ownership changes."""
        _check(_lib.ash_lock(self._ctx, _enc(signal)), "ash_lock")

    def unlock(self, signal: str) -> None:
        """Unlock a previously locked signal."""
        _check(_lib.ash_unlock(self._ctx, _enc(signal)), "ash_unlock")

    # ------------------------------------------------------------------
    # Runtime
    # ------------------------------------------------------------------

    def write(self, signal: str, value: float) -> None:
        """Write a signal value (requires ownership)."""
        _check(_lib.ash_write(self._ctx, _enc(signal), ctypes.c_double(value)),
               "ash_write")

    def read(self, signal: str) -> float:
        """Read the current value of a signal."""
        out = ctypes.c_double(0.0)
        _check(_lib.ash_read(self._ctx, _enc(signal), ctypes.byref(out)), "ash_read")
        return out.value

    def frame_tx(self, iface: str, can_id: int, data: bytes) -> None:
        """Transmit a raw CAN frame on the given interface."""
        _check(
            _lib.ash_frame_tx(self._ctx, _enc(iface), can_id,
                              len(data), 0, data),
            "ash_frame_tx",
        )

    def poll(self, timeout: float = 0.0) -> Optional[AshEvent]:
        """Poll for an incoming event.

        :param timeout: Seconds to wait; ``0`` = non-blocking,
                        negative = block indefinitely.
        :returns: :class:`AshEvent` on success, ``None`` on timeout.
        """
        ev = _Event()
        timeout_ms = int(timeout * 1000) if timeout >= 0 else -1
        ret = _lib.ash_poll(self._ctx, ctypes.byref(ev), timeout_ms)
        if ret == 0:
            return None
        if ret < 0:
            raise AshError(-ret, f"ash_poll failed (errno {-ret})")
        return _decode_event(ev)

    # ------------------------------------------------------------------
    # Persistence
    # ------------------------------------------------------------------

    def cfg_save(self, name: str) -> None:
        """Save the current configuration to persistent storage."""
        _check(_lib.ash_cfg_save(self._ctx, _enc(name)), "ash_cfg_save")

    def cfg_load(self, name: str) -> None:
        """Load a named configuration from persistent storage."""
        _check(_lib.ash_cfg_load(self._ctx, _enc(name)), "ash_cfg_load")
