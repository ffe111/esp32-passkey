"""Ways to reach an ESP32 passkey.

* ``usb``      - a real USB FIDO key (ESP32-S2/S3/P4) through the OS HID stack.
* ``serial:``  - a board without USB OTG (ESP32, C3, C6, H2) running the serial
                 transport firmware. Frames are ``F1 D0 | len16be | 64-byte report``.
* ``tcp:``     - the Pico FIDO emulator (``pico_fido`` built with ENABLE_EMULATION),
                 which uses ``len16be | report`` frames on a TCP socket.

Every transport only moves raw 64-byte CTAPHID reports, so python-fido2's
``CtapHidDevice`` does the CTAPHID protocol (INIT, channel ids, fragmentation).
"""

from __future__ import annotations

import socket
import time
from dataclasses import dataclass
from typing import Callable, Protocol

from fido2.hid import ConnectionFailure, CtapHidDevice, list_descriptors, open_connection
from fido2.hid.base import CtapHidConnection, HidDescriptor

REPORT_SIZE = 64
SERIAL_MAGIC = b"\xF1\xD0"
DEFAULT_TCP = ("127.0.0.1", 35962)
SERIAL_BAUD = 115200


class TransportError(OSError):
    pass


class ByteStream(Protocol):
    def read(self, n: int) -> bytes: ...
    def write(self, data: bytes) -> int | None: ...
    def close(self) -> None: ...


def read_exact(stream: ByteStream, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise TransportError(f"device closed or timed out after {len(buf)}/{n} bytes")
        buf += chunk
    return bytes(buf)


class FramedConnection(CtapHidConnection):
    """CTAPHID reports carried as length-prefixed frames over a byte stream."""

    def __init__(self, stream: ByteStream, magic: bytes = b""):
        self.stream = stream
        self.magic = magic

    def write_packet(self, data: bytes) -> None:
        packet = bytes(data).ljust(REPORT_SIZE, b"\0")[:REPORT_SIZE]
        self.stream.write(self.magic + len(packet).to_bytes(2, "big") + packet)

    def _sync(self) -> None:
        """Skip boot logs or noise until the frame magic shows up."""
        matched = 0
        skipped = 0
        while matched < len(self.magic):
            b = read_exact(self.stream, 1)
            if b[0] == self.magic[matched]:
                matched += 1
            else:
                matched = 1 if b[0] == self.magic[0] else 0
                skipped += 1
                if skipped > 4096:
                    raise TransportError("no passkey frames on this port (wrong firmware?)")

    def read_packet(self) -> bytes:
        if self.magic:
            self._sync()
        size = int.from_bytes(read_exact(self.stream, 2), "big")
        if size == 0 or size > 4096:
            raise TransportError(f"bad frame length {size}")
        return read_exact(self.stream, size)

    def close(self) -> None:
        self.stream.close()


class _SocketStream:
    def __init__(self, sock: socket.socket):
        self.sock = sock

    def read(self, n: int) -> bytes:
        return self.sock.recv(n)

    def write(self, data: bytes) -> int:
        self.sock.sendall(data)
        return len(data)

    def close(self) -> None:
        self.sock.close()


def _drain(stream) -> None:
    reset = getattr(stream, "reset_input_buffer", None)
    if reset:
        reset()


def _open_serial(port: str, baud: int = SERIAL_BAUD, boot_wait: float = 1.5):
    import serial  # pyserial, imported lazily so USB-only users don't need it

    if "://" in port:
        # pyserial URL, e.g. socket://127.0.0.1:5555 for a QEMU-emulated ESP32 UART.
        ser = serial.serial_for_url(port, baudrate=baud, timeout=30)
        time.sleep(boot_wait)
        ser.reset_input_buffer()
        return ser

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 30  # CTAP waits for the button; keepalives arrive meanwhile
    # Keep DTR/RTS low so opening the port does not reset the board.
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(boot_wait)
    ser.reset_input_buffer()
    return ser


@dataclass
class DeviceSpec:
    kind: str  # usb | serial | tcp
    target: str = ""

    @classmethod
    def parse(cls, text: str) -> "DeviceSpec":
        text = text.strip()
        if text in ("", "usb", "auto"):
            return cls("usb")
        kind, _, target = text.partition(":")
        if "://" in text and kind not in ("serial",):
            return cls("serial", text)
        if kind not in ("usb", "serial", "tcp"):
            # Bare COM port / tty path is treated as serial.
            return cls("serial", text)
        return cls(kind, target)


_open_devices: list[CtapHidDevice] = []


def close_all() -> None:
    """Close every device opened through open_device (frees COM ports / sockets)."""
    while _open_devices:
        dev = _open_devices.pop()
        try:
            dev.close()
        except OSError:
            pass


def open_device(spec: str | DeviceSpec,
                serial_opener: Callable[[str], ByteStream] = _open_serial) -> CtapHidDevice:
    dev = _open_device(spec, serial_opener)
    _open_devices.append(dev)
    return dev


def _open_device(spec: str | DeviceSpec,
                 serial_opener: Callable[[str], ByteStream]) -> CtapHidDevice:
    if isinstance(spec, str):
        spec = DeviceSpec.parse(spec)

    if spec.kind == "usb":
        descriptors = list(list_descriptors())
        if spec.target:
            descriptors = [d for d in descriptors if str(d.path) == spec.target]
        if not descriptors:
            raise TransportError(
                "No USB FIDO key found. Plug in an ESP32-S2/S3 passkey, or use "
                "--device serial:COM3 for boards without native USB.")
        desc = descriptors[0]
        return CtapHidDevice(desc, open_connection(desc))

    if spec.kind == "serial":
        if not spec.target:
            raise TransportError("serial needs a port, e.g. serial:COM3 or serial:/dev/ttyUSB0")
        stream = serial_opener(spec.target)
        desc = HidDescriptor(spec.target, 0, 0, REPORT_SIZE, REPORT_SIZE, "ESP32 Passkey (serial)", None)
        conn = FramedConnection(stream, SERIAL_MAGIC)
        # A serial line can still hold replies from an earlier session that was cut
        # off; python-fido2 then sees a foreign INIT nonce. Retry the handshake.
        for attempt in range(3):
            try:
                return CtapHidDevice(desc, conn)
            except ConnectionFailure:
                if attempt == 2:
                    raise
                time.sleep(0.2)
                _drain(stream)

    if spec.kind == "tcp":
        host, _, port = spec.target.rpartition(":") if spec.target else ("", "", "")
        addr = (host or DEFAULT_TCP[0], int(port) if port else DEFAULT_TCP[1])
        sock = socket.create_connection(addr, timeout=30)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        desc = HidDescriptor(f"tcp:{addr[0]}:{addr[1]}", 0, 0, REPORT_SIZE, REPORT_SIZE, "Pico FIDO emulator", None)
        return CtapHidDevice(desc, FramedConnection(_SocketStream(sock)))

    raise TransportError(f"unknown device kind {spec.kind!r}")


def list_devices() -> list[str]:
    found = [f"usb:{d.path}  {d.product_name or ''} ({d.vid:04x}:{d.pid:04x})" for d in list_descriptors()]
    try:
        from serial.tools import list_ports

        for p in list_ports.comports():
            found.append(f"serial:{p.device}  {p.description} (needs serial-transport firmware)")
    except ImportError:
        pass
    return found
