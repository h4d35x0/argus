#pragma once
#include "esp_gap_ble_api.h"

// Multi-consumer wrapper around the ESP-IDF BLE scan API.
//
// The ESP32 BT controller exposes a single GAP callback slot — the last call
// to esp_ble_gap_register_callback() wins. This module owns that single slot
// and dispatches each scan-result event to every registered consumer, so the
// wardriver and AirTag sniffer (and future features) can scan in parallel
// without trampling each other.
//
// The BT controller and Bluedroid stack are brought up on the first add and
// torn down on the last remove (reference-counted). Consumers are only called
// for actual inquiry-response scan results (ESP_GAP_SEARCH_INQ_RES_EVT) — the
// manager handles the SCAN_PARAM_SET_COMPLETE → start_scanning hand-off.

typedef void (*ble_scan_cb_t)(esp_ble_gap_cb_param_t *param);

// Register a consumer. Idempotent — calling twice with the same cb is a no-op.
// Returns false if the controller fails to come up on the first add or the
// consumer table is full (capacity 8 — raise BLE_SCAN_MAX_CONSUMERS if needed).
bool ble_scan_add(ble_scan_cb_t cb);

// Unregister a consumer. The controller is torn down when the last consumer
// is removed.
void ble_scan_remove(ble_scan_cb_t cb);

// Bring the BLE controller up ONCE at boot and keep it up for the app's life.
// Call this early in setup() (before WiFi is used). It moves the slow, UI-freezing
// controller/stack init to boot, avoids WiFi/BT coexistence failures (BLE inits
// first), and makes the Bluetooth toggle + detectors instant — they just add/remove
// scan consumers on an already-running controller instead of re-initialising it.
void ble_scan_boot_keepalive();

// True if at least one consumer is registered.
bool ble_scan_active();

// How many consumers are currently registered. Useful for status UIs
// that want to surface "N scanners running" instead of just on/off.
int  ble_scan_consumer_count();
