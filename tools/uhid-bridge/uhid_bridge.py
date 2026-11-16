#!/usr/bin/env python3
"""Linux bridge: make a serial-transport ESP32 (or the emulator) look like a USB FIDO key.

Boards without USB OTG (ESP32, C3, C6, H2) cannot be a HID device themselves.
This creates a virtual HID FIDO device with /dev/uhid and forwards each 64-byte
CTAPHID report to the board, so browsers and libfido2 see a normal security key.

    sudo python3 uhid_bridge.py serial:/dev/ttyUSB0
    sudo python3 uhid_bridge.py tcp:127.0.0.1:35962      # emulator

Needs write access to /dev/uhid (root, or a udev rule) and the hidraw node
must be readable by your user (see the udev rule in docs).
Linux only: Windows and macOS need a signed virtual-HID driver.
"""

from __future__ import annotations

import os
import selectors
import struct
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "passkey-cli"))
from passkey_cli.transports import (  # noqa: E402
    REPORT_SIZE, SERIAL_MAGIC, DeviceSpec, FramedConnection, _open_serial, _SocketStream,
)

UHID_DESTROY = 1
UHID_START = 2
UHID_STOP = 3
UHID_OPEN = 4
UHID_CLOSE = 5
UHID_OUTPUT = 6
UHID_CREATE2 = 11
UHID_INPUT2 = 12
UHID_DATA_MAX = 4096
UHID_EVENT_SIZE = 4 + 4372  # type + largest union member (create2_req)
BUS_USB = 0x03

# FIDO Alliance usage page 0xF1D0, 64-byte input and output reports, no report id.
FIDO_REPORT_DESCRIPTOR = bytes([
    0x06, 0xD0, 0xF1, 0x09, 0x01, 0xA1, 0x01,
    0x09, 0x20, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02,
    0x09, 0x21, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02,
    0xC0,
])


def create2_event(name: str, vid: int, pid: int, rd: bytes) -> bytes:
    body = struct.pack(
        "<128s64s64sHHIIII",
        name.encode()[:127], b"esp32-passkey-bridge", b"", len(rd), BUS_USB, vid, pid, 0x0100, 0,
    ) + rd.ljust(UHID_DATA_MAX, b"\0")
    return struct.pack("<I", UHID_CREATE2) + body.ljust(UHID_EVENT_SIZE - 4, b"\0")


def input2_event(report: bytes) -> bytes:
    body = struct.pack("<H", len(report)) + report.ljust(UHID_DATA_MAX, b"\0")
    return struct.pack("<I", UHID_INPUT2) + body.ljust(UHID_EVENT_SIZE - 4, b"\0")


def parse_output(event: bytes) -> bytes | None:
    (etype,) = struct.unpack_from("<I", event, 0)
    if etype != UHID_OUTPUT:
        return None
    data = event[4:4 + UHID_DATA_MAX]
    (size,) = struct.unpack_from("<H", event, 4 + UHID_DATA_MAX)
    report = data[:size]
    # hidraw writes start with the report number (0) for devices without report ids.
    if len(report) == REPORT_SIZE + 1 and report[0] == 0:
        report = report[1:]
    return report


def open_backend(spec_text: str) -> FramedConnection:
    spec = DeviceSpec.parse(spec_text)
    if spec.kind == "serial":
        return FramedConnection(_open_serial(spec.target), SERIAL_MAGIC)
    if spec.kind == "tcp":
        import socket

        host, _, port = spec.target.rpartition(":")
        sock = socket.create_connection((host or "127.0.0.1", int(port or 35962)))
        sock.settimeout(None)
        return FramedConnection(_SocketStream(sock))
    raise SystemExit("backend must be serial:<port> or tcp:<host>:<port>")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    backend = open_backend(argv[1])
    fd = os.open("/dev/uhid", os.O_RDWR | os.O_CLOEXEC)
    os.write(fd, create2_event("ESP32 Passkey (bridge)", 0x1209, 0x0001, FIDO_REPORT_DESCRIPTOR))
    print("Virtual FIDO key created. Ctrl+C to stop.")

    def device_to_host():
        while True:
            os.write(fd, input2_event(backend.read_packet()))

    threading.Thread(target=device_to_host, daemon=True).start()
    try:
        while True:
            event = os.read(fd, UHID_EVENT_SIZE)
            report = parse_output(event)
            if report is not None:
                backend.write_packet(report)
    except KeyboardInterrupt:
        pass
    finally:
        os.write(fd, struct.pack("<I", UHID_DESTROY).ljust(UHID_EVENT_SIZE, b"\0"))
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
