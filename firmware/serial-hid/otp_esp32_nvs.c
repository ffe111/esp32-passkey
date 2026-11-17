/*
 * OTP key storage for the original ESP32, which lacks the eFuse key-purpose
 * blocks that pico-keys-sdk's otp_esp32.c expects (those exist on S2/S3/C3+).
 *
 * Two random 32-byte device keys are generated on first boot from the hardware
 * RNG and kept in NVS. Nothing is burned into eFuse, so this is reversible
 * (erase flash = new device). The trade-off: the keys are only as safe as the
 * flash; enable flash encryption for real use (see docs/SECURITY.md).
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <string.h>

#include "esp_random.h"
#include "bootloader_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "picokeys.h"
#include "otp_platform.h"

/* A dedicated NVS partition outside the flashed image (see apply_overlay.py).
 * The default "nvs" partition sits inside the 0x1000..app range of the merged
 * .bin, so a firmware update would wipe the keys and orphan every passkey. */
#define OTP_PART "pk_keys"
#define OTP_NS "passkey_otp"
#define OTP_KEY "keys"

static uint8_t otp_keys[64];

int otp_platform_enable_secure_boot(uint8_t bootkey, bool secure_lock) {
    (void)bootkey;
    (void)secure_lock;
    /* Secure boot on the original ESP32 is configured with espsecure/idf.py,
     * not from the authenticator at runtime. */
    return PICOKEYS_ERR_BLOCKED;
}

bool otp_platform_is_secure_boot_enabled(uint8_t *bootkey) {
    (void)bootkey;
    return false;
}

bool otp_platform_is_secure_boot_locked(void) {
    return false;
}

static bool load_or_create(void) {
    esp_err_t err = nvs_flash_init_partition(OTP_PART);
    if (err != ESP_OK) {
        /* Never erase and regenerate here: that would silently replace the
         * device keys and make every stored passkey undecryptable. */
        return false;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(OTP_PART, OTP_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(otp_keys);
    err = nvs_get_blob(h, OTP_KEY, otp_keys, &len);
    if (err != ESP_OK || len != sizeof(otp_keys)) {
        /* RF is off this early, so enable the entropy source explicitly. */
        bootloader_random_enable();
        esp_fill_random(otp_keys, sizeof(otp_keys));
        bootloader_random_disable();
        err = nvs_set_blob(h, OTP_KEY, otp_keys, sizeof(otp_keys));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err == ESP_OK;
}

void otp_platform_init(const uint8_t **otp_key_1_out, const uint8_t **otp_key_2_out) {
    if (!load_or_create()) {
        /* Without persistent keys every boot would look like a new device and
         * stored passkeys could not be decrypted, so refuse to provide keys. */
        *otp_key_1_out = NULL;
        *otp_key_2_out = NULL;
        return;
    }
    *otp_key_1_out = otp_keys;
    *otp_key_2_out = otp_keys + 32;
}
