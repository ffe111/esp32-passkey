/*
 * Minimal TinyUSB API surface for chips without USB OTG (ESP32, C3, C6, H2).
 * The CTAPHID code in pico-keys-sdk calls these; serial_hid.c implements them
 * over UART so the same firmware logic runs unchanged.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#ifndef PASSKEY_SERIAL_TUSB_SHIM_H
#define PASSKEY_SERIAL_TUSB_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef CONFIG_TINYUSB_TASK_PRIORITY
#define CONFIG_TINYUSB_TASK_PRIORITY 5
#endif
#define BOARD_TUD_RHPORT 0
#define CFG_TUD_HID_EP_BUFSIZE 64

typedef enum {
    HID_REPORT_TYPE_INVALID = 0,
    HID_REPORT_TYPE_INPUT,
    HID_REPORT_TYPE_OUTPUT,
    HID_REPORT_TYPE_FEATURE
} hid_report_type_t;

typedef enum { TUSB_ROLE_INVALID = 0, TUSB_ROLE_DEVICE, TUSB_ROLE_HOST } tusb_role_t;
typedef enum { TUSB_SPEED_FULL = 0, TUSB_SPEED_LOW, TUSB_SPEED_HIGH, TUSB_SPEED_AUTO = 0xFF } tusb_speed_t;
typedef struct {
    tusb_role_t role;
    tusb_speed_t speed;
} tusb_rhport_init_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} tusb_desc_device_t;

#define KEYBOARD_MODIFIER_LEFTSHIFT 0x02
/* Keyboard (OTP typing) is not available over serial; the table is only sized. */
#define HID_ASCII_TO_KEYCODE {0}

bool tusb_init(uint8_t rhport, const tusb_rhport_init_t *init);
static inline bool tud_init(uint8_t rhport) { (void)rhport; return true; }
static inline void tud_task(void) {}
static inline bool tud_suspended(void) { return false; }
static inline bool tud_remote_wakeup(void) { return false; }
static inline bool tud_mounted(void) { return true; }

bool tud_hid_ready(void);
bool tud_hid_n_ready(uint8_t instance);
bool tud_hid_n_report(uint8_t instance, uint8_t report_id, const void *report, uint16_t len);
static inline bool tud_hid_report(uint8_t report_id, const void *report, uint16_t len) {
    return tud_hid_n_report(0, report_id, report, len);
}
static inline bool tud_hid_n_keyboard_report(uint8_t instance, uint8_t report_id, uint8_t modifier, const uint8_t keycode[6]) {
    (void)instance; (void)report_id; (void)modifier; (void)keycode;
    return false;
}

/* Implemented by pico-keys-sdk hid.c */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len);
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize);
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen);

#endif
