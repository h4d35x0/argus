#pragma once
// Minimal host shim for ESP-IDF's esp_gap_ble_api.h. SIM ONLY.
//
// Needed because src/ble_scan_manager.h includes it, and tools_screen.cpp
// includes ble_scan_manager.h. tools_screen.cpp calls NO ble_scan_* function
// (verified by grep), so nothing here needs behaviour - only enough of the type
// for `typedef void (*ble_scan_cb_t)(esp_ble_gap_cb_param_t *)` to parse.
//
// Note the include is quoted in ble_scan_manager.h, so the real ../src header
// wins over anything placed in shim/ under that name; shimming the ESP-IDF
// header underneath it is what actually works.
//
// An opaque struct on purpose: if simulator code ever tries to READ a scan
// result it will fail to compile here rather than silently operate on a
// zero-filled fake BLE advert.
typedef struct esp_ble_gap_cb_param_t esp_ble_gap_cb_param_t;
