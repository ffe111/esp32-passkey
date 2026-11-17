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
#include "soc/uart_pins.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "button.h"
#ifdef PICOKEYS_TOUCH_PRESENCE
#include "touch_presence.h"
#endif
#ifdef PICOKEYS_BLE
#include "ble_fido.h"
#endif

static const char *TAG = "serial_hid";

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
#ifdef PICOKEYS_BLE
    if (ble_fido_take_report(report, len)) {
        return true; /* response for the Bluetooth channel */
    }
#endif
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

    TickType_t last_byte = xTaskGetTickCount();
#if CONFIG_LOG_DEFAULT_LEVEL >= 3
    /* Debug builds: report the BOOT pin so button problems can be seen. */
    TickType_t last_gpio_log = 0;
    int last_level = -1;
    gpio_dump_io_configuration(stdout, 1ULL << BOOT_PIN);
#endif

    while (1) {
#ifdef PICOKEYS_BLE
        ble_fido_poll(); /* Bluetooth requests enter the CTAP core on this task too */
#endif
        int n = uart_read_bytes(SERIAL_HID_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
        TickType_t now = xTaskGetTickCount();
#if CONFIG_LOG_DEFAULT_LEVEL >= 3
        int level = gpio_get_level(BOOT_PIN);
        if (level != last_level || now - last_gpio_log > pdMS_TO_TICKS(2000)) {
            ESP_LOGI(TAG, "BOOT pin GPIO%d level=%d", (int)BOOT_PIN, level);
            if (level != last_level && last_level != -1) {
                gpio_dump_io_configuration(stdout, 1ULL << BOOT_PIN);
            }
            last_level = level;
            last_gpio_log = now;
        }
#endif
        if (n <= 0) {
            /* A frame is 68 bytes (~6 ms at 115200). If one stalls halfway, bytes
             * were lost (e.g. FIFO overrun); drop it so the next frame can sync. */
            if (state != RX_MAGIC0 && now - last_byte > pdMS_TO_TICKS(50)) {
                ESP_LOGW(TAG, "dropped partial frame (%u/%u bytes)", (unsigned)got, (unsigned)REPORT_SIZE);
                state = RX_MAGIC0;
            }
            continue;
        }
        last_byte = now;
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
    /* With CONFIG_ESP_CONSOLE_NONE nothing else routes UART0 to its pins, so on
     * real silicon the port stays silent (QEMU ignores pin muxing). Route it here. */
    err = uart_set_pin(SERIAL_HID_UART, U0TXD_GPIO_NUM, U0RXD_GPIO_NUM,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
#ifdef PICOKEYS_TOUCH_PRESENCE
    touch_presence_init();
#endif
#ifdef PICOKEYS_BLE
    ble_fido_init();
#endif
    xTaskCreatePinnedToCore(serial_hid_task, "serial_hid", 8192, NULL, CONFIG_TINYUSB_TASK_PRIORITY, NULL, tskNO_AFFINITY);
    return ESP_OK;
}
