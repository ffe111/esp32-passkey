"""Transport tests against a tiny in-process CTAPHID responder (no hardware needed)."""

from __future__ import annotations

import os
import struct
import threading

import pytest

from passkey_cli.transports import (
    REPORT_SIZE,
    SERIAL_MAGIC,
    DeviceSpec,
    FramedConnection,
    TransportError,
    open_device,
)

CTAPHID_PING = 0x81
CTAPHID_INIT = 0x86
CTAPHID_WINK = 0x88
BROADCAST = b"\xff\xff\xff\xff"


class Pipe:
    """One direction of a byte pipe."""

    def __init__(self):
        self.buf = bytearray()
        self.cv = threading.Condition()
        self.closed = False

    def write(self, data: bytes) -> int:
        with self.cv:
            self.buf += data
            self.cv.notify_all()
        return len(data)

    def read(self, n: int) -> bytes:
        with self.cv:
            while not self.buf and not self.closed:
                if not self.cv.wait(timeout=5):
                    return b""
            out = bytes(self.buf[:n])
            del self.buf[:n]
            return out


class Duplex:
    def __init__(self, rx: Pipe, tx: Pipe):
        self.rx, self.tx = rx, tx

    def read(self, n):
        return self.rx.read(n)

    def write(self, data):
        return self.tx.write(data)

    def close(self):
        with self.tx.cv:
            self.tx.closed = True
            self.tx.cv.notify_all()


class FakeAuthenticator(threading.Thread):
    """Answers CTAPHID INIT, PING and WINK; reassembles fragmented messages."""

    def __init__(self, conn: FramedConnection):
        super().__init__(daemon=True)
        self.conn = conn
        self.cid = os.urandom(4)
        self.winks = 0

    def send(self, cid: bytes, cmd: int, payload: bytes) -> None:
        first = cid + bytes([cmd]) + struct.pack(">H", len(payload)) + payload[: REPORT_SIZE - 7]
        self.conn.write_packet(first)
        rest, seq = payload[REPORT_SIZE - 7:], 0
        while rest:
            self.conn.write_packet(cid + bytes([seq]) + rest[: REPORT_SIZE - 5])
            rest, seq = rest[REPORT_SIZE - 5:], seq + 1

    def run(self):
        try:
            while True:
                pkt = self.conn.read_packet()
                cid, cmd = pkt[:4], pkt[4]
                length = struct.unpack(">H", pkt[5:7])[0]
                data = bytearray(pkt[7:7 + length])
                while len(data) < length:
                    cont = self.conn.read_packet()
                    data += cont[5:5 + length - len(data)]
                if cmd == CTAPHID_INIT:
                    # nonce(8) cid(4) proto=2 major minor build caps(WINK=0x01)
                    self.send(cid, CTAPHID_INIT, bytes(data[:8]) + self.cid + bytes([2, 1, 0, 0, 0x01]))
                elif cmd == CTAPHID_PING:
                    self.send(cid, CTAPHID_PING, bytes(data))
                elif cmd == CTAPHID_WINK:
                    self.winks += 1
                    self.send(cid, CTAPHID_WINK, b"")
        except TransportError:
            return


def make_pair(magic: bytes):
    a, b = Pipe(), Pipe()
    host = Duplex(rx=a, tx=b)
    dev = Duplex(rx=b, tx=a)
    return host, dev


@pytest.mark.parametrize("magic", [b"", SERIAL_MAGIC])
def test_init_ping_wink_through_python_fido2(magic):
    host_stream, dev_stream = make_pair(magic)
    fake = FakeAuthenticator(FramedConnection(dev_stream, magic))
    fake.start()

    if magic:
        dev = open_device(DeviceSpec("serial", "FAKE"), serial_opener=lambda _port: host_stream)
    else:
        # Same framing as the emulator, injected directly.
        from fido2.hid import CtapHidDevice
        from fido2.hid.base import HidDescriptor

        desc = HidDescriptor("fake", 0, 0, 64, 64, "fake", None)
        dev = CtapHidDevice(desc, FramedConnection(host_stream))

    assert dev.version == 2
    payload = os.urandom(200)  # forces several continuation packets each way
    assert dev.ping(payload) == payload
    dev.wink()
    assert fake.winks == 1
    dev.close()


def test_serial_sync_skips_boot_noise():
    host_stream, dev_stream = make_pair(SERIAL_MAGIC)
    # ROM bootloader text, a lone magic byte, then a real frame.
    dev_stream.write(b"ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n\xF1x")
    FramedConnection(dev_stream, SERIAL_MAGIC).write_packet(b"\x01\x02\x03")
    got = FramedConnection(host_stream, SERIAL_MAGIC).read_packet()
    assert got[:3] == b"\x01\x02\x03" and len(got) == REPORT_SIZE


def test_frame_layout_on_the_wire():
    a, b = Pipe(), Pipe()
    FramedConnection(Duplex(rx=a, tx=b), SERIAL_MAGIC).write_packet(b"\xAA")
    raw = b.read(100)
    assert raw[:2] == SERIAL_MAGIC
    assert raw[2:4] == (64).to_bytes(2, "big")
    assert raw[4] == 0xAA and len(raw) == 4 + 64


@pytest.mark.parametrize("text,kind,target", [
    ("usb", "usb", ""),
    ("", "usb", ""),
    ("serial:COM3", "serial", "COM3"),
    ("COM3", "serial", "COM3"),
    ("/dev/ttyUSB0", "serial", "/dev/ttyUSB0"),
    ("tcp:127.0.0.1:35962", "tcp", "127.0.0.1:35962"),
])
def test_device_spec_parse(text, kind, target):
    spec = DeviceSpec.parse(text)
    assert (spec.kind, spec.target) == (kind, target)
