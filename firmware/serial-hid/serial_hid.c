/*
 * CTAPHID over UART for ESP32 chips without USB OTG.
 *
 * Wire format (both directions), one CTAPHID report per frame:
 *     F1 D0 | length (2 bytes, big endian, always 64) | 64-byte report
 * The magic lets the host skip ROM boot messages. The board's USB-serial chip
 * (CP210x/CH340) carries the frames; passkey-cli and uhid-bridge speak this format.
 *
 * The console is disabled in this build (CONFIG_ESP_CONSOLE_NONE) so nothing else
 * writes to UART0.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "tinyusb.h"
#include "usb.h"

#ifndef SERIAL_HID_UART
#define SERIAL_HID_UART UART_NUM_0
#endif
#ifndef SERIAL_HID_BAUD
#define SERIAL_HID_BAUD 115200
#endif

#define REPORT_SIZE 64
#define MAGIC0 0xF1
#define MAGIC1 0xD0

/* ---- Descriptor symbols normally provided by usb_descriptors.c ---- */
tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .idVendor = USB_VID,
    .idProduct = USB_PID,
};
const uint8_t desc_config[1] = { 0 };
char *string_desc_arr[] = {
    "", "Pol Henarejos", "ESP32 Passkey", "", "", "", "FIDO", "Keyboard", "CCID", "WebCCID", "Net",
};
char *string_desc_itf[5] = { "FIDO", "", "", "", "" };
static const char *string_desc_slots[8] = { "", "Pol Henarejos", "ESP32 Passkey", "", "", "", "", "" };
tinyusb_config_t tusb_cfg = {
    .device_descriptor = &desc_device,
    .string_descriptor = string_desc_slots,
    .string_descriptor_count = 6,
    .configuration_descriptor = desc_config,
};

void usb_desc_setup(void) {}

/* ---- Transmit side ----
 * uart_write_bytes() copies the frame into the driver's TX ring buffer (blocking
 * only if it is full), so a report is "sent" as soon as this returns. hid.c then
 * reports completion synchronously (overlay patch, same as the upstream emulator). */
bool tud_hid_n_ready(uint8_t instance) {
    (void)instance;
    return true;
}

bool tud_hid_ready(void) {
    return true;
}

bool tud_hid_n_report(uint8_t instance, uint8_t report_id, const void *report, uint16_t len) {
    (void)instance;
    (void)report_id;
    if (len > REPORT_SIZE) {
        return false;
    }
    uint8_t frame[4 + REPORT_SIZE] = { MAGIC0, MAGIC1, 0x00, REPORT_SIZE };
    memcpy(frame + 4, report, len);
    return uart_write_bytes(SERIAL_HID_UART, (const char *)frame, sizeof(frame)) == (int)sizeof(frame);
}

/* ---- Receive side ---- */
typedef enum { RX_MAGIC0, RX_MAGIC1, RX_LEN0, RX_LEN1, RX_DATA } rx_state_t;

static void serial_hid_task(void *arg) {
    (void)arg;
    rx_state_t state = RX_MAGIC0;
    uint16_t expect = 0, got = 0;
    uint8_t rx_report[REPORT_SIZE];
    uint8_t chunk[128];

    while (1) {
        int n = uart_read_bytes(SERIAL_HID_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            switch (state) {
                case RX_MAGIC0:
                    state = (b == MAGIC0) ? RX_MAGIC1 : RX_MAGIC0;
                    break;
                case RX_MAGIC1:
                    state = (b == MAGIC1) ? RX_LEN0 : (b == MAGIC0 ? RX_MAGIC1 : RX_MAGIC0);
                    break;
                case RX_LEN0:
                    expect = (uint16_t)b << 8;
                    state = RX_LEN1;
                    break;
                case RX_LEN1:
                    expect |= b;
                    got = 0;
                    state = (expect == REPORT_SIZE) ? RX_DATA : RX_MAGIC0;
                    break;
                case RX_DATA:
                    rx_report[got++] = b;
                    if (got == REPORT_SIZE) {
                        tud_hid_set_report_cb(ITF_HID_CTAP, 0, HID_REPORT_TYPE_OUTPUT, rx_report, REPORT_SIZE);
                        state = RX_MAGIC0;
                    }
                    break;
            }
        }
    }
}

bool tusb_init(uint8_t rhport, const tusb_rhport_init_t *init) {
    (void)rhport;
    (void)init;
    return true;
}

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config) {
    (void)config;
    const uart_config_t uart_config = {
        .baud_rate = SERIAL_HID_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(SERIAL_HID_UART, 4096, 4096, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(SERIAL_HID_UART, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    xTaskCreatePinnedToCore(serial_hid_task, "serial_hid", 8192, NULL, CONFIG_TINYUSB_TASK_PRIORITY, NULL, tskNO_AFFINITY);
    return ESP_OK;
}
