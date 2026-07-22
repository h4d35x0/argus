#pragma once
#include <lvgl.h>

// Status / power-control screen for the Bluetooth (BLE) radio. Sits
// between the WiFi and NFC screens in the power-button rotation,
// mirroring the layout of the GPS / LoRa / WiFi radio screens.
//
// Note: this is *not* the BT scan-chart screen — that's `bt_analyze_screen`
// reached from the Tools menu. This screen only governs the radio's power
// state (via the ble_scan_manager refcount) and surfaces live state.

void bluetooth_screen_create();
void bluetooth_screen_show();
bool bluetooth_screen_is_active();
bool bluetooth_screen_is_powered();

// Bring BLE up at boot IF the user opted Bluetooth into "Enable at boot".
// Called from setup(); see the definition for the boot-loop caveat.
void bluetooth_screen_restore_power();
