/*
 * FIDO over Bluetooth LE for the original ESP32 (no USB OTG).
 *
 * Implements the CTAP 2.x Bluetooth transport: GATT service 0xFFFD with
 *   fidoControlPoint            F1D0FFF1-...  write   (client -> authenticator)
 *   fidoStatus                  F1D0FFF2-...  notify  (authenticator -> client)
 *   fidoControlPointLength      F1D0FFF3-...  read    (max fragment size)
 *   fidoServiceRevisionBitfield F1D0FFF4-...  read/write (0x20 = FIDO2)
 * Every characteristic needs an encrypted link, so the host has to pair first
 * (Just Works, bonded).
 *
 * BLE frames: init = CMD(1) HLEN(1) LLEN(1) DATA, continuation = SEQ(1) DATA.
 * Commands: PING 0x81, KEEPALIVE 0x82, MSG 0x83, CANCEL 0xBE, ERROR 0xBF.
 *
 * The CTAP core only speaks CTAPHID, so a complete BLE request is rewritten as
 * CTAPHID packets on a private channel id (BLE_CID) and fed to the core from the
 * serial transport task. CTAPHID responses on that channel are reassembled and
 * sent back as BLE notifications; everything else still goes to the UART, so
 * passkey-cli keeps working over USB-serial at the same time.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "tinyusb.h"
#include "usb.h"
#include "ble_fido.h"

void ble_store_config_init(void);

static const char *TAG = "ble_fido";

#define DEVICE_NAME "ESP32 Passkey"
#define REPORT_SIZE 64
#define MAX_MSG 2048

/* BLE frame commands */
#define BLE_PING 0x81
#define BLE_KEEPALIVE 0x82
#define BLE_MSG 0x83
#define BLE_CANCEL 0xBE
#define BLE_ERROR 0xBF
#define BLE_ERR_INVALID_CMD 0x01
#define BLE_ERR_INVALID_LEN 0x03
#define BLE_ERR_INVALID_SEQ 0x04
#define BLE_ERR_BUSY 0x06

/* CTAPHID commands (ctap_hid.h) */
#define HID_PING 0x81
#define HID_MSG 0x83
#define HID_CBOR 0x90
#define HID_CANCEL 0x91
#define HID_KEEPALIVE 0xBB
#define HID_ERROR 0xBF

/* Private CTAPHID channel for BLE traffic (hid.c accepts any non-zero,
 * non-broadcast channel id). */
static const uint8_t BLE_CID[4] = { 0xB1, 0xE0, 0xF1, 0xD0 };

/* F1D0FFFx-DEAA-ECEE-B42F-C9BA7ED623BB, little-endian byte order */
#define FIDO_UUID(x) BLE_UUID128_INIT(0xBB, 0x23, 0xD6, 0x7E, 0xBA, 0xC9, 0x2F, 0xB4, \
                                      0xEE, 0xEC, 0xAA, 0xDE, (x), 0xFF, 0xD0, 0xF1)
static const ble_uuid128_t uuid_cp = FIDO_UUID(0xF1);
static const ble_uuid128_t uuid_status = FIDO_UUID(0xF2);
static const ble_uuid128_t uuid_cp_len = FIDO_UUID(0xF3);
static const ble_uuid128_t uuid_rev_bits = FIDO_UUID(0xF4);

static uint16_t status_handle;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool subscribed;
static uint16_t frag_len = 20; /* fidoControlPointLength */
static uint8_t own_addr_type;

/* BLE -> CTAP core: CTAPHID packets queued for the serial task */
static QueueHandle_t rx_queue;
/* request reassembly (NimBLE host task) */
static uint8_t req_buf[MAX_MSG];
static uint16_t req_len, req_got;
static uint8_t req_cmd;
static int req_seq = -1; /* -1: waiting for an init frame */

/* CTAP core -> BLE: response reassembly (CTAP task) */
static SemaphoreHandle_t tx_lock;
static uint8_t resp_buf[MAX_MSG];
static uint16_t resp_len, resp_got;
static uint8_t resp_cmd;
static bool resp_active;
static TickType_t last_keepalive;
static uint8_t last_keepalive_status;

static void advertise(void);

/* ---- sending ---- */

static void notify(const uint8_t *data, uint16_t len) {
    for (int tries = 0; tries < 200; tries++) {
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !subscribed) {
            return;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om && ble_gatts_notify_custom(conn_handle, status_handle, om) == 0) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5)); /* out of buffers: let the stack drain */
    }
    ESP_LOGW(TAG, "notification dropped");
}

static void ble_send(uint8_t cmd, const uint8_t *data, uint16_t len) {
    uint8_t frag[512];
    uint16_t fl = frag_len;
    uint16_t n = len < fl - 3 ? len : fl - 3;
    frag[0] = cmd;
    frag[1] = len >> 8;
    frag[2] = len & 0xFF;
    memcpy(frag + 3, data, n);
    notify(frag, 3 + n);
    uint16_t off = n;
    uint8_t seq = 0;
    while (off < len) {
        n = len - off < fl - 1 ? len - off : fl - 1;
        frag[0] = seq++ & 0x7F;
        memcpy(frag + 1, data + off, n);
        notify(frag, 1 + n);
        off += n;
    }
}

static void ble_error(uint8_t code) {
    ble_send(BLE_ERROR, &code, 1);
}

/* ---- BLE request -> CTAPHID packets ---- */

static void queue_ctaphid(uint8_t hid_cmd, const uint8_t *data, uint16_t len) {
    if (uxQueueSpacesAvailable(rx_queue) < 1 + (len > 57 ? (len - 57 + 58) / 59 : 0)) {
        ble_error(BLE_ERR_BUSY);
        return;
    }
    uint8_t pkt[REPORT_SIZE] = { 0 };
    memcpy(pkt, BLE_CID, 4);
    pkt[4] = hid_cmd;
    pkt[5] = len >> 8;
    pkt[6] = len & 0xFF;
    uint16_t n = len < 57 ? len : 57;
    memcpy(pkt + 7, data, n);
    xQueueSend(rx_queue, pkt, 0);
    uint16_t off = n;
    uint8_t seq = 0;
    while (off < len) {
        memset(pkt + 4, 0, REPORT_SIZE - 4);
        pkt[4] = seq++;
        n = len - off < 59 ? len - off : 59;
        memcpy(pkt + 5, data + off, n);
        xQueueSend(rx_queue, pkt, 0);
        off += n;
    }
}

static void request_complete(void) {
    ESP_LOGI(TAG, "request cmd 0x%02x, %u bytes, CTAP command 0x%02x", req_cmd, req_len,
             req_len ? req_buf[0] : 0);
    switch (req_cmd) {
        case BLE_PING:
            queue_ctaphid(HID_PING, req_buf, req_len);
            break;
        case BLE_MSG:
            /* U2F APDUs start with CLA 0x00; CTAP2 messages start with a command byte. */
            queue_ctaphid(req_len && req_buf[0] == 0x00 ? HID_MSG : HID_CBOR, req_buf, req_len);
            break;
        case BLE_CANCEL:
            queue_ctaphid(HID_CANCEL, NULL, 0);
            break;
        default:
            ble_error(BLE_ERR_INVALID_CMD);
    }
}

static void on_fragment(const uint8_t *f, uint16_t len) {
    if (len == 0) {
        return;
    }
    if (f[0] & 0x80) { /* init frame */
        if (len < 3) {
            ble_error(BLE_ERR_INVALID_LEN);
            return;
        }
        req_cmd = f[0];
        req_len = ((uint16_t)f[1] << 8) | f[2];
        if (req_len > MAX_MSG) {
            req_seq = -1;
            ble_error(BLE_ERR_INVALID_LEN);
            return;
        }
        req_got = len - 3 > req_len ? req_len : len - 3;
        memcpy(req_buf, f + 3, req_got);
        req_seq = 0;
    }
    else {
        if (req_seq < 0 || f[0] != (req_seq & 0x7F)) {
            req_seq = -1;
            ble_error(BLE_ERR_INVALID_SEQ);
            return;
        }
        req_seq++;
        uint16_t n = len - 1;
        if (req_got + n > req_len) {
            n = req_len - req_got;
        }
        memcpy(req_buf + req_got, f + 1, n);
        req_got += n;
    }
    if (req_got >= req_len) {
        req_seq = -1;
        request_complete();
    }
}

void ble_fido_poll(void) {
    uint8_t pkt[REPORT_SIZE];
    while (rx_queue && xQueueReceive(rx_queue, pkt, 0) == pdTRUE) {
        tud_hid_set_report_cb(ITF_HID_CTAP, 0, HID_REPORT_TYPE_OUTPUT, pkt, REPORT_SIZE);
    }
}

/* ---- CTAPHID response -> BLE ---- */

static void response_complete(void) {
    ESP_LOGI(TAG, "response 0x%02x, %u bytes, status 0x%02x", resp_cmd, resp_len,
             resp_len ? resp_buf[0] : 0);
    switch (resp_cmd) {
        case HID_CBOR:
        case HID_MSG:
            ble_send(BLE_MSG, resp_buf, resp_len);
            break;
        case HID_PING:
            ble_send(BLE_PING, resp_buf, resp_len);
            break;
        case HID_ERROR:
            ble_send(BLE_ERROR, resp_buf, resp_len);
            break;
        default:
            break; /* INIT, WINK, ... have no BLE equivalent */
    }
}

bool ble_fido_take_report(const uint8_t *r, uint16_t len) {
    if (len < 7 || memcmp(r, BLE_CID, 4) != 0) {
        return false;
    }
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    if (r[4] & 0x80) {
        uint16_t total = ((uint16_t)r[5] << 8) | r[6];
        if (r[4] == HID_KEEPALIVE) {
            /* The core sends these often; BLE needs one at least every 500 ms. */
            TickType_t now = xTaskGetTickCount();
            if (r[7] != last_keepalive_status || now - last_keepalive >= pdMS_TO_TICKS(250)) {
                last_keepalive = now;
                last_keepalive_status = r[7];
                ble_send(BLE_KEEPALIVE, r + 7, 1);
            }
        }
        else if (total <= MAX_MSG) {
            resp_cmd = r[4];
            resp_len = total;
            resp_got = total < 57 ? total : 57;
            memcpy(resp_buf, r + 7, resp_got);
            resp_active = true;
        }
    }
    else if (resp_active) {
        uint16_t n = resp_len - resp_got < 59 ? resp_len - resp_got : 59;
        memcpy(resp_buf + resp_got, r + 5, n);
        resp_got += n;
    }
    if (resp_active && resp_got >= resp_len) {
        resp_active = false;
        last_keepalive_status = 0;
        response_complete();
    }
    xSemaphoreGive(tx_lock);
    return true;
}

/* ---- GATT ---- */

static uint16_t current_frag_len(void) {
    uint16_t mtu = conn_handle != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(conn_handle) : 23;
    uint16_t l = mtu > 3 ? mtu - 3 : 20;
    if (l < 20) {
        l = 20;
    }
    if (l > 512) {
        l = 512;
    }
    return l;
}

static int fido_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn;
    (void)attr;
    uintptr_t which = (uintptr_t)arg;
    switch (which) {
        case 1: { /* control point */
            if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            uint8_t buf[512];
            uint16_t n = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            on_fragment(buf, n);
            return 0;
        }
        case 3: { /* control point length */
            frag_len = current_frag_len();
            uint8_t v[2] = { frag_len >> 8, frag_len & 0xFF };
            return os_mbuf_append(ctxt->om, v, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case 4: { /* service revision bitfield: FIDO2 only */
            if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
                uint8_t v = 0x20;
                return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            return 0; /* client selects 0x20; nothing else is offered */
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static int dis_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn;
    (void)attr;
    const char *s = arg;
    return os_mbuf_append(ctxt->om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFFD),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &uuid_cp.u, .access_cb = fido_access, .arg = (void *)1,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &uuid_status.u, .access_cb = fido_access, .arg = (void *)2,
              .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &status_handle },
            { .uuid = &uuid_cp_len.u, .access_cb = fido_access, .arg = (void *)3,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { .uuid = &uuid_rev_bits.u, .access_cb = fido_access, .arg = (void *)4,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                       BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC },
            { 0 },
        },
    },
    {
        /* Device Information Service (required by the BLE transport) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = dis_access,
              .arg = (void *)"esp32-passkey", .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A24), .access_cb = dis_access,
              .arg = (void *)DEVICE_NAME, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A26), .access_cb = dis_access,
              .arg = (void *)"1.0", .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP ---- */

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                frag_len = current_frag_len();
                req_seq = -1;
                ESP_LOGI(TAG, "connected");
            }
            else {
                advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            subscribed = false;
            req_seq = -1;
            advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == status_handle) {
                subscribed = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "status notifications %s", subscribed ? "on" : "off");
            }
            break;
        case BLE_GAP_EVENT_MTU:
            frag_len = current_frag_len();
            ESP_LOGI(TAG, "MTU %u", event->mtu.value);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption change, status %d", event->enc_change.status);
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* The host lost its bond and pairs again: forget the old one. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        default:
            break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(0xFFFD) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    /* Service data: flags 0x80 = "in pairing mode" */
    static const uint8_t svc_data[3] = { 0xFD, 0xFF, 0x80 };
    fields.svc_data_uuid16 = svc_data;
    fields.svc_data_uuid16_len = sizeof(svc_data);
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(TAG, "adv fields failed");
        return;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "advertising failed: %d", rc);
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "advertising as \"%s\" %02x:%02x:%02x:%02x:%02x:%02x", DEVICE_NAME,
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_fido_init(void) {
    rx_queue = xQueueCreate(40, REPORT_SIZE);
    tx_lock = xSemaphoreCreateMutex();

    /* Bonds live in the default "nvs" partition. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(err));
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGW(TAG, "NimBLE init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0) {
        ESP_LOGW(TAG, "GATT setup failed");
        return;
    }
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_att_set_preferred_mtu(512);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
