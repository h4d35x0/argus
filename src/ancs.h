// ancs.h - iOS notification mirroring via ANCS (Apple Notification Center Service).
//
// The watch is the ANCS "Notification Consumer": it advertises with the ANCS
// service in its solicitation list, lets the iPhone connect and bond, then acts
// as a GATT client against the iPhone's ANCS service to receive notifications
// and fetch their attributes (app / title / message). Parsed notifications are
// pushed into notify::center().
//
// No companion app is needed on iOS - ANCS is built into the OS.
//
// Radio ownership: like the BLE scan manager, this brings up the Bluedroid
// controller itself and must NOT run while WiFi is up (esp_bt_controller_enable()
// hangs otherwise). The Daily-wear / Field-tool mode owner guarantees ANCS and
// the scanners are never active at the same time (they share the single GAP
// callback slot).
#pragma once
#include <cstdint>

namespace ancs {

// Bring up BLE (guarded against WiFi), start advertising the ANCS solicitation,
// and register the GAP/GATTS/GATTC handlers. Returns false and does nothing if
// WiFi is active or the controller fails to come up. Idempotent.
bool start();

// Stop advertising / disconnect / tear the stack back down. Idempotent.
void stop();

bool is_running();

// True once an iPhone has connected, bonded, and ANCS is subscribed.
bool is_connected();

// True once the controller has confirmed advertising is on the air
// (ESP_GAP_BLE_ADV_START_COMPLETE_EVT with success). Diagnostic for "is the
// watch actually discoverable".
bool is_advertising();

// Dismiss a notification on the iPhone (ANCS Perform Notification Action,
// negative). No-op if not connected or the notification is unknown. Used by the
// UI so clearing on the watch also clears it on the phone.
void dismiss(uint32_t uid);

}  // namespace ancs
