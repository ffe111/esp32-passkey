/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef PASSKEY_SERIAL_TINYUSB_SHIM_H
#define PASSKEY_SERIAL_TINYUSB_SHIM_H

#include "tusb.h"
#include "esp_err.h"

typedef struct {
    const tusb_desc_device_t *device_descriptor;
    const char **string_descriptor;
    int string_descriptor_count;
    const uint8_t *configuration_descriptor;
} tinyusb_config_t;

/* Starts the UART transport instead of the USB stack. */
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);

#endif
