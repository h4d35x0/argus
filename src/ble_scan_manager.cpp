#include "ble_scan_manager.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include <WiFi.h>

#define BLE_SCAN_MAX_CONSUMERS 8   // headroom: 5 detectors + wardriver + BT-toggle no-op

static ble_scan_cb_t s_consumers[BLE_SCAN_MAX_CONSUMERS] = {};
static int           s_consumer_count = 0;
static ble_scan_err_t s_last_error    = BLE_SCAN_OK;

// True while the WiFi radio is powered in any mode. On this board (ESP32-S3,
// T-Watch Ultra) the WiFi and BLE controllers together exceed the free
// CONTIGUOUS internal SRAM, and — worse — esp_bt_controller_enable() HANGS
// (never returns) when WiFi already holds that RAM. So we must know WiFi's
// state BEFORE we touch the BLE controller. WiFi.getMode() == WIFI_MODE_NULL
// only when the radio is fully off (STA/AP/promiscuous all report non-NULL).
static bool wifi_is_active()
{
    return WiFi.getMode() != WIFI_MODE_NULL;
}

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
    // Defense in depth: never call esp_bt_controller_enable() with WiFi up — it
    // hangs. ble_scan_add() already guards this, but guard here too so any future
    // caller can't reintroduce the freeze.
    if (wifi_is_active())
        return false;

    bool ok = true;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ok = (esp_bt_controller_init(&bt_cfg) == ESP_OK);
    }
    if (ok && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
        ok = (esp_bt_controller_enable(ESP_BT_MODE_BLE) == ESP_OK);
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
        ok = (esp_bluedroid_init() == ESP_OK);
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
        ok = (esp_bluedroid_enable() == ESP_OK);
    if (!ok) return false;

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
    if (!cb) { s_last_error = BLE_SCAN_ERR_CONTROLLER; return false; }

    // Idempotent: already-registered cbs aren't added twice.
    for (int i = 0; i < s_consumer_count; i++)
        if (s_consumers[i] == cb) { s_last_error = BLE_SCAN_OK; return true; }

    if (s_consumer_count >= BLE_SCAN_MAX_CONSUMERS) {
        s_last_error = BLE_SCAN_ERR_NO_SLOTS;
        return false;
    }

    bool first = (s_consumer_count == 0);

    // COEXISTENCE GUARD (the reason this whole enum exists): only the FIRST
    // consumer triggers a controller bring-up. If WiFi is up at that moment,
    // esp_bt_controller_enable() inside bring_up_controller() would HANG and
    // freeze the watch (hard restart needed). Refuse cleanly BEFORE the call so
    // the caller can tell the user "turn WiFi off first". Additional consumers
    // (first == false) run against an already-live controller and are safe.
    if (first && wifi_is_active()) {
        s_last_error = BLE_SCAN_ERR_WIFI_ACTIVE;
        return false;
    }

    s_consumers[s_consumer_count++] = cb;

    if (first) {
        if (!bring_up_controller()) {
            s_consumer_count--;
            s_last_error = BLE_SCAN_ERR_CONTROLLER;
            return false;
        }
    }
    s_last_error = BLE_SCAN_OK;
    return true;
}

ble_scan_err_t ble_scan_last_error()
{
    return s_last_error;
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

// Permanent keep-alive consumer: registered once at boot and never removed, so
// the BT controller comes up a single time (at boot, before WiFi, so it's fast
// and coexistence-safe) and stays up. All later add/remove calls (BT toggle,
// detectors) then run against an already-live controller — instant, no UI freeze.
static void keepalive_cb(esp_ble_gap_cb_param_t *) { /* passive-scan keep-alive */ }

void ble_scan_boot_keepalive()
{
    // NOTE: NOT currently invoked — calling this from setup() crashed boot (BLE
    // brought up too early). Kept as the intended proper fix for the WiFi/BT
    // coexistence freeze, to be wired at a safe, verified init point later. The
    // interim mitigation is UI ordering (Bluetooth screen before WiFi).
    (void)ble_scan_add(keepalive_cb);
}
