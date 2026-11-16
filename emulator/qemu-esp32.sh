#!/usr/bin/env bash
# Run the serial-transport firmware in Espressif's QEMU with UART0 exposed on TCP
# port 5555. Supports the original ESP32 (default) and ESP32-C3.
# Run inside espressif/idf:v5.5.4 with the repo mounted at /src:
#
#   docker run -d --name qemu -p 5555:5555 -v "$PWD:/src" espressif/idf:v5.5.4 \
#       bash -c "tr -d '\r' < /src/emulator/qemu-esp32.sh > /tmp/q.sh && QEMU_TARGET=esp32 bash /tmp/q.sh"
#   passkey-cli --device socket://127.0.0.1:5555 info
#
# QEMU does not model the BOOT button, so user presence behaves as if pressed.
set -euo pipefail
TARGET="${QEMU_TARGET:-esp32}"
FIRMWARE="${FIRMWARE:-/src/dist/passkey_${TARGET}.bin}"

case "$TARGET" in
  esp32)   TOOL=qemu-xtensa;  BIN=qemu-system-xtensa ;;
  esp32c3) TOOL=qemu-riscv32; BIN=qemu-system-riscv32 ;;
  *) echo "QEMU supports esp32 and esp32c3 only" >&2; exit 2 ;;
esac

python "$IDF_PATH/tools/idf_tools.py" install "$TOOL" >/dev/null
. "$IDF_PATH/export.sh" >/dev/null
cp "$FIRMWARE" /tmp/flash.bin
truncate -s 4M /tmp/flash.bin
exec "$BIN" -nographic -machine "$TARGET" \
    -drive file=/tmp/flash.bin,if=mtd,format=raw \
    -serial tcp::5555,server,nowait
