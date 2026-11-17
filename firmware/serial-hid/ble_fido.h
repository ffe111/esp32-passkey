/* FIDO over Bluetooth LE (CTAP 2.x "BLE" transport) for the original ESP32.
 * SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Start the NimBLE host and advertise the FIDO service (0xFFFD). */
void ble_fido_init(void);

/* Called from the serial transport task: feed queued BLE requests to the CTAP core
 * (keeps every CTAPHID packet on one task). */
void ble_fido_poll(void);

/* tud_hid_n_report() hook: returns true if this 64-byte CTAPHID report belongs to the
 * BLE channel (it is then sent over BLE instead of the UART). */
bool ble_fido_take_report(const uint8_t *report, uint16_t len);
