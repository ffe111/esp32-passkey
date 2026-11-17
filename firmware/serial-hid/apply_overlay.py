#!/usr/bin/env python3
"""Patch a *copy* of pico-fido: common fixes for every target, plus (serial mode)
the changes that let it build for ESP32 chips without USB OTG.

The upstream submodule is never modified. Every replacement must match exactly
once; if upstream changes, this fails loudly instead of building something odd.

Usage: apply_overlay.py <pico-fido copy dir> <serial-hid dir> <usb|serial>
"""

from __future__ import annotations

import sys
from pathlib import Path


def patch(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"overlay: expected 1 match in {path}, found {count}:\n{old}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def patch_all(path: Path, old: str, new: str, expect: int) -> None:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    count = text.count(old)
    if count != expect:
        raise SystemExit(f"overlay: expected {expect} matches in {path}, found {count}:\n{old}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def apply_common(root_p: Path) -> None:
    """Fixes for every target (USB and serial)."""
    sdk = root_p / "pico-keys-sdk"
    # Upstream starts the 15 s "button held too long" timer when the wait begins,
    # so any press after 15 s is rejected as a timeout and the real window is 15 s,
    # not 30. Restart the clock at the moment the press is first seen.
    patch(sdk / "src" / "button.c",
          "    if (!async_button_pressed && pressed) {\n        async_button_pressed = true;\n    }\n",
          "    if (!async_button_pressed && pressed) {\n        async_button_pressed = true;\n"
          "        async_button_started = now;\n    }\n")
    print("overlay: common fixes applied")


def main(root: str, overlay: str, mode: str) -> None:
    root_p = Path(root)
    apply_common(root_p)
    if mode != "serial":
        return
    sdk = root_p / "pico-keys-sdk"
    ov = Path(overlay).resolve().as_posix()

    # 1. Use the UART transport instead of the TinyUSB descriptors.
    patch(
        sdk / "picokeys_sdk_import.cmake",
        "else()\n    list(APPEND PICOKEYS_SOURCES\n        ${CMAKE_CURRENT_LIST_DIR}/src/usb/usb_descriptors.c\n    )\nendif()",
        "elseif(PICOKEYS_SERIAL_HID)\n"
        f"    list(APPEND PICOKEYS_SOURCES {ov}/serial_hid.c)\n"
        "    add_compile_definitions(PICOKEYS_SERIAL_HID=1)\n"
        "    if(PICOKEYS_TOUCH_PRESENCE)\n"
        f"        list(APPEND PICOKEYS_SOURCES {ov}/touch_presence.c)\n"
        "        add_compile_definitions(PICOKEYS_TOUCH_PRESENCE=1)\n"
        "    endif()\n"
        "else()\n    list(APPEND PICOKEYS_SOURCES\n        ${CMAKE_CURRENT_LIST_DIR}/src/usb/usb_descriptors.c\n    )\nendif()",
    )

    # 1a. Report completion synchronously, exactly like the upstream emulator does.
    #     (A UART write is "done" once queued; async completion raced with hid.c
    #     marking the write pending and could deadlock the CTAPHID state.)
    patch(
        sdk / "src" / "usb" / "hid" / "hid.c",
        "#ifdef ENABLE_EMULATION\n    tud_hid_report_complete_cb(ITF_HID_CTAP, buffer.data, buffer_len);\n#endif",
        "#if defined(ENABLE_EMULATION) || defined(PICOKEYS_SERIAL_HID)\n    tud_hid_report_complete_cb(ITF_HID_CTAP, buffer.data, buffer_len);\n#endif",
    )

    # 1b. Original ESP32: keep device keys in NVS (no eFuse key-purpose blocks).
    patch_all(
        sdk / "picokeys_sdk_import.cmake",
        "if(ESP_PLATFORM)\n    list(APPEND PICOKEYS_SOURCES\n        ${CMAKE_CURRENT_LIST_DIR}/src/otp/otp_esp32.c\n    )\n",
        "if(ESP_PLATFORM AND PICOKEYS_OTP_NVS)\n"
        f"    list(APPEND PICOKEYS_SOURCES {ov}/otp_esp32_nvs.c)\n"
        "elseif(ESP_PLATFORM)\n    list(APPEND PICOKEYS_SOURCES\n        ${CMAKE_CURRENT_LIST_DIR}/src/otp/otp_esp32.c\n    )\n",
        expect=2,
    )

    # 1c. Device keys (otp_esp32_nvs.c) get their own NVS partition in the free gap
    #     between the app (ends 0x120000) and part0 (0x200000). A merged .bin only
    #     covers 0x1000..end of app, so firmware updates no longer wipe the keys.
    patch(sdk / "config" / "esp32" / "partitions.csv",
          "part0,    0x40, 0x1,     0x200000,  1M,\n",
          "pk_keys,  data, nvs,     0x1F0000,  0x6000\n"
          "part0,    0x40, 0x1,     0x200000,  1M,\n")

    # 2. SDK component: no esp_tinyusb; add the shim headers and the UART driver.
    comp = sdk / "config" / "esp32" / "components" / "pico-keys-sdk"
    patch(comp / "CMakeLists.txt",
          "    ${PICOKEYS_SDK_DIR}/src/led\n",
          f"    ${{PICOKEYS_SDK_DIR}}/src/led\n    {ov}/shim\n    {ov}\n")
    patch(comp / "CMakeLists.txt",
          "    esp_tinyusb\n",
          "    esp_driver_uart\n    esp_driver_gpio\n    esp_driver_touch_sens\n    esp_timer\n    nvs_flash\n")

    # 2a. Touching the touch pad counts as pressing BOOT (PICOKEYS_TOUCH_PRESENCE).
    patch(sdk / "src" / "button.c",
          "#ifdef ESP_PLATFORM\nstatic bool picok_board_button_read(void) {\n"
          "    int boot_state = gpio_get_level(BOOT_PIN);\n    return boot_state == 0;\n}\n",
          "#ifdef ESP_PLATFORM\n#ifdef PICOKEYS_TOUCH_PRESENCE\n#include \"touch_presence.h\"\n#endif\n"
          "static bool picok_board_button_read(void) {\n"
          "    int boot_state = gpio_get_level(BOOT_PIN);\n"
          "#ifdef PICOKEYS_TOUCH_PRESENCE\n    if (touch_presence_read()) {\n        return true;\n    }\n#endif\n"
          "    return boot_state == 0;\n}\n")
    patch(comp / "idf_component.yml", '  espressif/esp_tinyusb: "^1.7.6"\n', "")

    # 3. Classic ESP32 has the "parallel engine" SHA block, which ships no include dir.
    patch(sdk / "config" / "esp32" / "components" / "mbedtls" / "CMakeLists.txt",
          "    list(APPEND PICO_KEYS_MBEDTLS_PORT_INCLUDE_DIRS ${PICO_KEYS_MBEDTLS_PORT_DIR}/sha/${PICO_KEYS_MBEDTLS_SHA_PERIPHERAL_DIR}/include)\n",
          "    if(EXISTS ${PICO_KEYS_MBEDTLS_PORT_DIR}/sha/${PICO_KEYS_MBEDTLS_SHA_PERIPHERAL_DIR}/include)\n"
          "        list(APPEND PICO_KEYS_MBEDTLS_PORT_INCLUDE_DIRS ${PICO_KEYS_MBEDTLS_PORT_DIR}/sha/${PICO_KEYS_MBEDTLS_SHA_PERIPHERAL_DIR}/include)\n"
          "    endif()\n")

    # 3b. C3/C6 have no SHA-512 hardware: use mbedTLS software SHA-512 there.
    patch(sdk / "config" / "esp32" / "components" / "mbedtls" / "CMakeLists.txt",
          "            ${PICO_KEYS_MBEDTLS_PORT_DIR}/sha/${PICO_KEYS_MBEDTLS_SHA_PERIPHERAL_DIR}/esp_sha512.c\n        )\n",
          "        )\n"
          "        if(CONFIG_SOC_SHA_SUPPORT_SHA512)\n"
          "            list(APPEND PICO_KEYS_MBEDTLS_PORT_SOURCES ${PICO_KEYS_MBEDTLS_PORT_DIR}/sha/${PICO_KEYS_MBEDTLS_SHA_PERIPHERAL_DIR}/esp_sha512.c)\n"
          "        endif()\n")
    patch(sdk / "picokeys_sdk_import.cmake",
          "    add_compile_definitions(MBEDTLS_SHA512_ALT=1)\n",
          "    if(NOT PICOKEYS_NO_HW_SHA512)\n        add_compile_definitions(MBEDTLS_SHA512_ALT=1)\n    endif()\n")

    # 3c. C3/C6 boards: BOOT button is GPIO9 and the RGB LED is GPIO8.
    patch(sdk / "src" / "button.h",
          "#if defined(ESP_PLATFORM)\n#define BOOT_PIN GPIO_NUM_0\n#endif",
          "#if defined(ESP_PLATFORM)\n#include \"sdkconfig.h\"\n"
          "#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)\n"
          "#define BOOT_PIN GPIO_NUM_9\n#else\n#define BOOT_PIN GPIO_NUM_0\n#endif\n#endif")
    for led_file in ("led.c", "led_neopixel.c"):
        patch(sdk / "src" / "led" / led_file,
              "defined(CONFIG_IDF_TARGET_ESP32C6)\n",
              "defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32C3)\n")

    # 4. The fido component also asked for esp_tinyusb.
    patch(root_p / "src" / "fido" / "CMakeLists.txt",
          "REQUIRES esp_tinyusb mbedtls",
          "REQUIRES esp_driver_uart mbedtls")
    print("overlay: serial HID transport applied")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[3] not in ("usb", "serial"):
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
