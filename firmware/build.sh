#!/usr/bin/env bash
# Build Pico FIDO passkey firmware for one ESP32 target inside the espressif/idf container.
#
#   USB targets (native USB security key):   esp32s3 esp32s2
#   Serial targets (no USB OTG in the chip): esp32 esp32c3 esp32c6
#
# Usage (inside container): firmware/build.sh <target> [out_dir]
# The source tree is copied to /work so the host checkout is never modified.
set -euo pipefail

TARGET="${1:?usage: build.sh <esp32s3|esp32s2|esp32|esp32c3|esp32c6> [out_dir]}"
OUT_DIR="${2:-/src/dist}"
SRC_ROOT="${SRC_ROOT:-/src}"

case "$TARGET" in
  esp32s3|esp32s2) MODE=usb ;;
  esp32|esp32c3|esp32c6) MODE=serial ;;
  *) echo "Unsupported target '$TARGET'." >&2; exit 2 ;;
esac

WORK="/work/$TARGET"
rm -rf "$WORK"
mkdir -p "$WORK"
cp -a "$SRC_ROOT/firmware/pico-fido/." "$WORK/"
cd "$WORK"
# The upstream build number comes from `git rev-list --count HEAD`. Point the copied
# submodule checkouts back at the host repo's git data so that still works.
echo "gitdir: $SRC_ROOT/.git/modules/firmware/pico-fido" > "$WORK/.git"
echo "gitdir: $SRC_ROOT/.git/modules/firmware/pico-fido/modules/pico-keys-sdk" > "$WORK/pico-keys-sdk/.git"
git config --global --add safe.directory '*'
git -C "$WORK" rev-list --count HEAD >/dev/null

# Optional overrides for the USB descriptor. Releases must use a VID/PID you own
# (for open-source projects: request one from https://pid.codes).
EXTRA_DEFS=()
if [[ -n "${USB_VID:-}" ]]; then EXTRA_DEFS+=("-DUSB_VID=${USB_VID}"); fi
if [[ -n "${USB_PID:-}" ]]; then EXTRA_DEFS+=("-DUSB_PID=${USB_PID}"); fi

# Upstream approves every request unless the board was commissioned with a button
# timeout, so any program on the PC could use the key silently. Always require a
# BOOT press (30 s timeout). REQUIRE_BUTTON=0 is only for QEMU tests (no button).
SUFFIX=""
if [[ "${REQUIRE_BUTTON:-1}" == 1 ]]; then
  EXTRA_DEFS+=("-DFORCE_BUTTON_WAIT=1")
else
  SUFFIX="_nobutton"
fi

OVERLAY="/work/serial-hid"
rm -rf "$OVERLAY"
cp -a "$SRC_ROOT/firmware/serial-hid" "$OVERLAY"
# Common fixes apply to every target; serial mode adds the UART transport.
python3 "$OVERLAY/apply_overlay.py" "$WORK" "$OVERLAY" "$MODE"

if [[ "$MODE" == serial ]]; then
  # OATH/OTP need CCID or keyboard HID, which a serial link cannot offer.
  EXTRA_DEFS+=("-DPICOKEYS_SERIAL_HID=1" "-DENABLE_OATH_APP=0" "-DENABLE_OTP_APP=0")
  # The original ESP32 has no eFuse key-purpose blocks; keep device keys in NVS.
  # It also gets capacitive touch (T0 = GPIO4) as a second way to confirm presence,
  # for boards whose BOOT button is unreliable.
  if [[ "$TARGET" == esp32 ]]; then
    EXTRA_DEFS+=("-DPICOKEYS_OTP_NVS=1")
    if [[ "${REQUIRE_BUTTON:-1}" == 1 ]]; then EXTRA_DEFS+=("-DPICOKEYS_TOUCH_PRESENCE=1"); fi
  fi
  # C3/C6 have no SHA-512 accelerator.
  if [[ "$TARGET" == esp32c3 || "$TARGET" == esp32c6 ]]; then EXTRA_DEFS+=("-DPICOKEYS_NO_HW_SHA512=1"); fi
  export SDKCONFIG_DEFAULTS="pico-keys-sdk/config/esp32/sdkconfig.defaults;$OVERLAY/sdkconfig.serial"
  if [[ "${SERIAL_DEBUG:-0}" == 1 ]]; then
    SDKCONFIG_DEFAULTS+=";$OVERLAY/sdkconfig.serial-debug"
    SUFFIX+=_debug
  fi
fi

idf.py "${EXTRA_DEFS[@]}" set-target "$TARGET"
idf.py "${EXTRA_DEFS[@]}" build

CHIP_NAME="$(echo "$TARGET" | sed 's/esp32/ESP32-/; s/-$//' | tr '[:lower:]' '[:upper:]')"
mkdir -p "$OUT_DIR"
cd build
OUT_BIN="$OUT_DIR/passkey_${TARGET}${SUFFIX}.bin"
python -m esptool --chip "$CHIP_NAME" merge_bin -o "$OUT_BIN" @flash_args
echo "Built $OUT_BIN ($MODE transport)"
