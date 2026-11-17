/*
 * Capacitive touch as a second user-presence input (original ESP32).
 *
 * Some cheap boards have a BOOT button that does not reliably pull GPIO0 low.
 * Touching the touch pad (default: T0 = GPIO4, or a wire soldered to it) counts
 * as pressing BOOT. The baseline is measured at boot (if a finger is on the pin
 * then, it recovers within about a second of letting go).
 *
 * Detection: smoothed reading below 70% of the untouched baseline for 5 samples
 * in a row = touched; 5 samples above = released. The baseline follows slow
 * drift while untouched (see track_baseline). A single noisy sample can
 * therefore never count as a press.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/touch_sens.h"

#include "touch_presence.h"

#ifndef PICOKEYS_TOUCH_CHANNEL
#define PICOKEYS_TOUCH_CHANNEL 0 /* T0 = GPIO4 on the ESP32 */
#endif
#define TOUCH_RATIO_PCT 70
#define TOUCH_DEBOUNCE 5

static const char *TAG = "touch";
static touch_sensor_handle_t s_sens;
static touch_channel_handle_t s_chan;
static uint32_t s_baseline;
static bool s_ready;
static bool s_touched;
static int s_on, s_off;

/* Follow slow drift (temperature, humidity) without following a finger.
 * Polled ~200x/s, so rate-limit to 10 updates/s. Rising readings are followed
 * quickly (a finger on the pin at boot gives a low baseline; this recovers it).
 * Falling readings are followed slowly (time constant ~6 s), and not at all while
 * the reading sits in the "finger approaching" band, so a slow press cannot drag
 * the baseline down until the real touch no longer crosses the threshold. */
#define TOUCH_DRIFT_BAND_PCT 90
static void track_baseline(uint32_t v) {
    static TickType_t last;
    TickType_t now = xTaskGetTickCount();
    if (now - last < pdMS_TO_TICKS(100)) {
        return;
    }
    last = now;
    if (v > s_baseline) {
        s_baseline = (s_baseline * 3 + v) / 4;
    }
    else if ((uint64_t)v * 100 >= (uint64_t)s_baseline * TOUCH_DRIFT_BAND_PCT) {
        s_baseline = (s_baseline * 63 + v) / 64;
    }
}

static bool read_smooth(uint32_t *v) {
    return touch_channel_read_data(s_chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, v) == ESP_OK;
}

void touch_presence_init(void) {
    touch_sensor_sample_config_t sample[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7),
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample);
    if (touch_sensor_new_controller(&sens_cfg, &s_sens) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller unavailable");
        return;
    }
    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = { 0 }, /* hardware threshold unused; we compare in software */
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    if (touch_sensor_new_channel(s_sens, PICOKEYS_TOUCH_CHANNEL, &chan_cfg, &s_chan) != ESP_OK ||
        touch_sensor_config_filter(s_sens, &filter_cfg) != ESP_OK ||
        touch_sensor_enable(s_sens) != ESP_OK ||
        touch_sensor_start_continuous_scanning(s_sens) != ESP_OK) {
        ESP_LOGW(TAG, "touch channel setup failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the software filter settle */
    uint32_t sum = 0, v = 0;
    for (int i = 0; i < 8; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!read_smooth(&v)) {
            return;
        }
        sum += v;
    }
    s_baseline = sum / 8;
    s_ready = s_baseline > 0;
    touch_chan_info_t info = { 0 };
    touch_sensor_get_channel_info(s_chan, &info);
    ESP_LOGI(TAG, "touch presence on GPIO%d, baseline %u", info.chan_gpio, (unsigned)s_baseline);
}

bool touch_presence_read(void) {
    uint32_t v;
    if (!s_ready || !read_smooth(&v)) {
        return false;
    }
#if CONFIG_LOG_DEFAULT_LEVEL >= 3
    /* Debug builds: show the raw level twice a second so thresholds can be tuned. */
    static TickType_t last_log;
    TickType_t now = xTaskGetTickCount();
    if (now - last_log >= pdMS_TO_TICKS(500)) {
        last_log = now;
        ESP_LOGI(TAG, "level %u / baseline %u (%u%%)", (unsigned)v, (unsigned)s_baseline,
                 (unsigned)(s_baseline ? (uint64_t)v * 100 / s_baseline : 0));
    }
#endif
    bool below = (uint64_t)v * 100 < (uint64_t)s_baseline * TOUCH_RATIO_PCT;
    if (below) {
        s_off = 0;
        if (++s_on >= TOUCH_DEBOUNCE && !s_touched) {
            s_touched = true;
            ESP_LOGI(TAG, "touched (%u / baseline %u)", (unsigned)v, (unsigned)s_baseline);
        }
    }
    else {
        s_on = 0;
        if (++s_off >= TOUCH_DEBOUNCE && s_touched) {
            s_touched = false;
            ESP_LOGI(TAG, "released (%u)", (unsigned)v);
        }
        if (!s_touched) {
            track_baseline(v);
        }
    }
    return s_touched;
}
