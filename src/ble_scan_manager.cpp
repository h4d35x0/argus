#include "ble_scan_manager.h"
#include <Arduino.h>          // Serial (BLE bring-up diagnostics)
#include "esp_bt.h"
#include "esp_bt_main.h"

#define BLE_SCAN_MAX_CONSUMERS 8   // headroom: 5 detectors + wardriver + BT-toggle no-op

static ble_scan_cb_t s_consumers[BLE_SCAN_MAX_CONSUMERS] = {};
static int           s_consumer_count = 0;

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    // Re-arm the scan after the controller acknowledges our params. If every
    // consumer left during the brief async window, do nothing.
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        if (s_consumer_count > 0)
            esp_ble_gap_start_scanning(0);
        return;
    }
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    // Fan out to every registered consumer. Each one applies its own filter.
    for (int i = 0; i < s_consumer_count; i++) {
        if (s_consumers[i]) s_consumers[i](param);
    }
}

// State-aware bring-up — mirrors the pattern wardriver_screen.cpp had before
// this refactor so we can re-attach gracefully if the controller was already
// initialised by something outside our control.
static bool bring_up_controller()
{
    esp_err_t e = ESP_OK;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        e = esp_bt_controller_init(&bt_cfg);
        if (e != ESP_OK) { Serial.printf("[BLE] controller_init FAIL: %s\n", esp_err_to_name(e)); return false; }
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        e = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (e != ESP_OK) { Serial.printf("[BLE] controller_enable FAIL: %s\n", esp_err_to_name(e)); return false; }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        e = esp_bluedroid_init();
        if (e != ESP_OK) { Serial.printf("[BLE] bluedroid_init FAIL: %s\n", esp_err_to_name(e)); return false; }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        e = esp_bluedroid_enable();
        if (e != ESP_OK) { Serial.printf("[BLE] bluedroid_enable FAIL: %s\n", esp_err_to_name(e)); return false; }
    }
    Serial.println("[BLE] controller up OK");

    esp_ble_gap_register_callback(gap_cb);

    esp_ble_scan_params_t scan_params = {
        BLE_SCAN_TYPE_PASSIVE,
        BLE_ADDR_TYPE_PUBLIC,
        BLE_SCAN_FILTER_ALLOW_ALL,
        0x50,
        0x30,
        BLE_SCAN_DUPLICATE_DISABLE
    };
    // esp_ble_gap_set_scan_params will trigger SCAN_PARAM_SET_COMPLETE_EVT,
    // which gap_cb above turns into a start_scanning call.
    esp_ble_gap_set_scan_params(&scan_params);
    return true;
}

static void tear_down_controller()
{
    esp_ble_gap_stop_scanning();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}

bool ble_scan_add(ble_scan_cb_t cb)
{
    if (!cb) return false;

    // Idempotent: already-registered cbs aren't added twice.
    for (int i = 0; i < s_consumer_count; i++)
        if (s_consumers[i] == cb) return true;

    if (s_consumer_count >= BLE_SCAN_MAX_CONSUMERS) return false;

    bool first = (s_consumer_count == 0);
    s_consumers[s_consumer_count++] = cb;

    if (first) {
        if (!bring_up_controller()) {
            s_consumer_count--;
            return false;
        }
    }
    return true;
}

void ble_scan_remove(ble_scan_cb_t cb)
{
    for (int i = 0; i < s_consumer_count; i++) {
        if (s_consumers[i] == cb) {
            for (int j = i; j < s_consumer_count - 1; j++)
                s_consumers[j] = s_consumers[j + 1];
            s_consumers[--s_consumer_count] = nullptr;
            if (s_consumer_count == 0)
                tear_down_controller();
            return;
        }
    }
}

bool ble_scan_active()
{
    return s_consumer_count > 0;
}

int ble_scan_consumer_count()
{
    return s_consumer_count;
}
