"""End-to-end tests against real Pico FIDO firmware.

Linux build of the firmware (TCP):
    docker run -d --rm --name emu -p 35962:35962 esp32-passkey-emulator
    PASSKEY_DEVICE=tcp:127.0.0.1:35962 pytest tests/test_e2e_emulator.py

ESP32 serial-transport firmware in QEMU (see emulator/qemu-esp32.sh):
    PASSKEY_DEVICE=socket://127.0.0.1:5555 pytest tests/test_e2e_emulator.py

A real board works too (PASSKEY_DEVICE=serial:COM3); press BOOT when asked.
The tests expect a freshly erased device and run in order:
info -> set PIN -> passkey -> manage.
"""

from __future__ import annotations

import hashlib
import os

import pytest
from fido2.ctap2 import ClientPin, Ctap2

from passkey_cli.cli import main
from passkey_cli.transports import close_all, open_device

DEVICE = os.environ.get("PASSKEY_DEVICE", "")
PIN = "123456"
RP = {"id": "example.com", "name": "Example"}

pytestmark = pytest.mark.skipif(not DEVICE, reason="set PASSKEY_DEVICE (tcp:host:port, socket://host:port, serial:COM3)")


@pytest.fixture(autouse=True)
def _close_devices():
    yield
    close_all()


def press_boot(capsys, times: int) -> None:
    """Tell a person at a real board to press BOOT (pytest hides the CLI prompt)."""
    with capsys.disabled():
        print(f"\n    >>> Tap BOOT (or touch GPIO4 on an ESP32) {times}x: touch ~1 s then let go,"
              f" each within 30 s <<<", flush=True)


def run(capsys, *argv) -> tuple[int, str]:
    close_all()  # a serial port allows only one open connection
    code = main(["--device", DEVICE, *argv])
    out = capsys.readouterr()
    return code, out.out + out.err


def test_01_info(capsys):
    code, out = run(capsys, "info")
    assert code == 0, out
    assert "FIDO_2_" in out


def test_02_set_pin(capsys):
    ctap = Ctap2(open_device(DEVICE))
    if ctap.info.options.get("clientPin"):
        pytest.skip("emulator flash already has a PIN (reused container)")
    code, out = run(capsys, "pin", "set", "--new-pin", PIN)
    assert code == 0, out
    code, out = run(capsys, "--pin", PIN, "info")
    assert "PIN retries:" in out


def test_03_selftest(capsys):
    # Show the CLI's own "Tap the button..." lines live: the second approval only
    # counts once the device asks for it, so a person needs to see each prompt.
    press_boot(capsys, 2)
    close_all()
    with capsys.disabled():
        code = main(["--device", DEVICE, "--pin", PIN, "selftest"])
    assert code == 0


def test_04_discoverable_passkey_then_list_and_delete(capsys):
    ctap = Ctap2(open_device(DEVICE))
    cp = ClientPin(ctap)
    cdh = hashlib.sha256(os.urandom(32)).digest()
    token = cp.get_pin_token(PIN, ClientPin.PERMISSION.MAKE_CREDENTIAL, RP["id"])
    user = {"id": b"user-0001", "name": "somchai", "displayName": "Somchai"}
    press_boot(capsys, 1)
    att = ctap.make_credential(
        cdh, RP, user, [{"type": "public-key", "alg": -7}],
        options={"rk": True},
        pin_uv_param=cp.protocol.authenticate(token, cdh),
        pin_uv_protocol=cp.protocol.VERSION,
    )
    cred_id = att.auth_data.credential_data.credential_id

    code, out = run(capsys, "--pin", PIN, "creds", "list")
    assert code == 0, out
    assert "example.com" in out and "somchai" in out and cred_id.hex() in out

    code, out = run(capsys, "--pin", PIN, "creds", "delete", cred_id.hex())
    assert code == 0, out
    code, out = run(capsys, "--pin", PIN, "creds", "list")
    assert cred_id.hex() not in out


def test_05_wrong_pin_is_rejected(capsys):
    code, out = run(capsys, "--pin", "000000", "creds", "list")
    assert code == 1
    assert "PIN_INVALID" in out
    code, out = run(capsys, "pin", "retries")
    assert out.strip().isdigit() and int(out.strip()) < 8
