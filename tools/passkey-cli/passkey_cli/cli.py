"""passkey-cli: free, open-source manager for ESP32 passkeys (Pico FIDO firmware).

Examples:
  passkey-cli list
  passkey-cli info
  passkey-cli --device serial:COM3 info
  passkey-cli pin set
  passkey-cli creds list
  passkey-cli selftest
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import os
import sys

from fido2.ctap import CtapError
from fido2.ctap2 import ClientPin, Ctap2, CredentialManagement
from fido2.webauthn import PublicKeyCredentialDescriptor

from . import __version__
from .transports import TransportError, close_all, list_devices, open_device


def _ctap2(args) -> Ctap2:
    dev = open_device(args.device)
    return Ctap2(dev)


def _pin(args, prompt: str = "PIN: ") -> str:
    if args.pin:
        return args.pin
    env = os.environ.get("PASSKEY_PIN")
    if env:
        return env
    return getpass.getpass(prompt)


def cmd_list(args) -> int:
    devices = list_devices()
    if not devices:
        print("No devices found.")
        return 1
    for d in devices:
        print(d)
    return 0


def cmd_info(args) -> int:
    ctap = _ctap2(args)
    info = ctap.info
    print(f"Versions:      {', '.join(info.versions)}")
    print(f"AAGUID:        {info.aaguid}")
    print(f"Extensions:    {', '.join(info.extensions) or '-'}")
    opts = ", ".join(f"{k}={v}" for k, v in sorted(info.options.items()))
    print(f"Options:       {opts}")
    print(f"Max msg size:  {info.max_msg_size}")
    if info.firmware_version is not None:
        fw = info.firmware_version
        print(f"Firmware:      {fw >> 8}.{fw & 0xFF}")
    if info.options.get("clientPin"):
        retries, _ = ClientPin(ctap).get_pin_retries()
        print(f"PIN retries:   {retries}")
    else:
        print("PIN:           not set (run: passkey-cli pin set)")
    return 0


def cmd_pin(args) -> int:
    ctap = _ctap2(args)
    cp = ClientPin(ctap)
    if args.action == "set":
        if ctap.info.options.get("clientPin"):
            print("A PIN is already set; use 'pin change'.", file=sys.stderr)
            return 1
        new = args.new_pin or getpass.getpass("New PIN (4-63 chars): ")
        cp.set_pin(new)
        print("PIN set.")
    elif args.action == "change":
        old = _pin(args, "Current PIN: ")
        new = args.new_pin or getpass.getpass("New PIN: ")
        cp.change_pin(old, new)
        print("PIN changed.")
    elif args.action == "retries":
        retries, _ = cp.get_pin_retries()
        print(retries)
    return 0


def _cred_mgmt(args, ctap: Ctap2) -> CredentialManagement:
    if not CredentialManagement.is_supported(ctap.info):
        raise SystemExit("This firmware does not support credential management.")
    cp = ClientPin(ctap)
    token = cp.get_pin_token(_pin(args), ClientPin.PERMISSION.CREDENTIAL_MGMT)
    return CredentialManagement(ctap, cp.protocol, token)


def cmd_creds(args) -> int:
    ctap = _ctap2(args)
    cm = _cred_mgmt(args, ctap)
    if args.action == "list":
        meta = cm.get_metadata()
        used = meta[CredentialManagement.RESULT.EXISTING_CRED_COUNT]
        free = meta[CredentialManagement.RESULT.MAX_REMAINING_COUNT]
        print(f"Passkeys stored: {used}, free slots: {free}")
        if not used:
            return 0
        for rp in cm.enumerate_rps():
            rp_ent = rp[CredentialManagement.RESULT.RP]
            rp_hash = rp[CredentialManagement.RESULT.RP_ID_HASH]
            print(f"\n{rp_ent.get('id', '?')}")
            for cred in cm.enumerate_creds(rp_hash):
                user = cred[CredentialManagement.RESULT.USER]
                cid = cred[CredentialManagement.RESULT.CREDENTIAL_ID]["id"]
                print(f"  user={user.get('name', '?')}  id={cid.hex()}")
    elif args.action == "delete":
        cm.delete_cred(PublicKeyCredentialDescriptor(type="public-key", id=bytes.fromhex(args.cred_id)))
        print("Deleted.")
    return 0


def cmd_reset(args) -> int:
    if not args.yes:
        answer = input("This deletes ALL passkeys and the PIN on the device. Type 'reset' to continue: ")
        if answer.strip() != "reset":
            print("Cancelled.")
            return 1
    ctap = _ctap2(args)
    print("Touch/press the button on the device now (reset must happen soon after power-up)...")
    ctap.reset()
    print("Device reset.")
    return 0


def cmd_wink(args) -> int:
    dev = open_device(args.device)
    dev.wink()
    print("Wink sent.")
    return 0


def cmd_selftest(args) -> int:
    """Register and use a throwaway credential against a fake RP (no browser needed)."""
    ctap = _ctap2(args)
    rp = {"id": "selftest.passkey.invalid", "name": "passkey-cli selftest"}
    user = {"id": os.urandom(16), "name": "selftest", "displayName": "Self Test"}
    params = [{"type": "public-key", "alg": -7}]  # ES256
    cdh = hashlib.sha256(b"passkey-cli-selftest" + os.urandom(16)).digest()

    pin_auth = pin_proto = None
    if ctap.info.options.get("clientPin"):
        cp = ClientPin(ctap)
        token = cp.get_pin_token(_pin(args), ClientPin.PERMISSION.MAKE_CREDENTIAL | ClientPin.PERMISSION.GET_ASSERTION, rp["id"])
        pin_proto = cp.protocol
        pin_auth = pin_proto.authenticate(token, cdh)

    print("Press the button on the device to approve registration...")
    att = ctap.make_credential(cdh, rp, user, params, pin_uv_param=pin_auth,
                               pin_uv_protocol=pin_proto.VERSION if pin_proto else None)
    cred = att.auth_data.credential_data
    print(f"  registered credential {cred.credential_id.hex()[:16]}... (fmt={att.fmt})")

    cdh2 = hashlib.sha256(os.urandom(32)).digest()
    if pin_proto:
        token = cp.get_pin_token(_pin(args), ClientPin.PERMISSION.GET_ASSERTION, rp["id"])
        pin_auth = pin_proto.authenticate(token, cdh2)
    print("Press the button again to approve sign-in...")
    assertion = ctap.get_assertion(rp["id"], cdh2,
                                   [{"type": "public-key", "id": cred.credential_id}],
                                   pin_uv_param=pin_auth,
                                   pin_uv_protocol=pin_proto.VERSION if pin_proto else None)
    assertion.verify(cdh2, cred.public_key)
    print(f"  signature verified, sign count={assertion.auth_data.counter}")
    print("Selftest OK.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="passkey-cli", description="Manage an ESP32 passkey (Pico FIDO firmware).")
    p.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    p.add_argument("-d", "--device", default=os.environ.get("PASSKEY_DEVICE", "usb"),
                   help="usb (default), serial:COM3, serial:/dev/ttyUSB0, or tcp:127.0.0.1:35962")
    p.add_argument("--pin", help="PIN (otherwise prompted, or env PASSKEY_PIN)")
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list connected devices").set_defaults(func=cmd_list)
    sub.add_parser("info", help="show authenticator info").set_defaults(func=cmd_info)
    sub.add_parser("wink", help="blink the LED").set_defaults(func=cmd_wink)
    sub.add_parser("selftest", help="register + sign in with a throwaway credential").set_defaults(func=cmd_selftest)

    pin = sub.add_parser("pin", help="set or change the PIN")
    pin.add_argument("action", choices=["set", "change", "retries"])
    pin.add_argument("--new-pin")
    pin.set_defaults(func=cmd_pin)

    creds = sub.add_parser("creds", help="list or delete stored passkeys")
    creds.add_argument("action", choices=["list", "delete"])
    creds.add_argument("cred_id", nargs="?", help="credential id (hex) for delete")
    creds.set_defaults(func=cmd_creds)

    reset = sub.add_parser("reset", help="factory reset (deletes everything)")
    reset.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    reset.set_defaults(func=cmd_reset)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "creds" and args.action == "delete" and not args.cred_id:
        print("creds delete needs a credential id", file=sys.stderr)
        return 2
    try:
        return args.func(args)
    except CtapError as e:
        print(f"Device error: {e.code.name} ({e})", file=sys.stderr)
        return 1
    except TransportError as e:
        print(f"Connection error: {e}", file=sys.stderr)
        return 1
    finally:
        close_all()


if __name__ == "__main__":
    sys.exit(main())
