#pragma once

// Master switch for BLE + WiFi + LoRa COEXISTENCE.
//
// 1 = bring the BLE (Bluedroid) controller up ONCE at boot, before WiFi is ever
//     enabled (the coexistence-safe order), and let WiFi and BLE run at the same
//     time. This relies on the Arduino-ESP32 core's built-in software coexistence
//     (on by default), exactly like upstream r3dfish/13-37, which runs all three
//     radios together. LoRa is a separate SPI chip and never contends.
//
// 0 = the original MUTUAL-EXCLUSION guards: WiFi and BLE refuse to be up at once
//     (the safe fallback we shipped because bringing the 2nd controller up AFTER
//     the 1st grabbed the radio HANGS the watch). All guard code is preserved
//     behind this flag - flip to 0 and reflash to fall back if coexistence hangs.
//
// The mutual-exclusion guards live in ble_scan_manager.cpp, wifi_beacon_manager.cpp,
// and offense_wifi.cpp; the boot keepalive is wired in main.cpp setup().
//
// 2026-07-24: enabled after measurement proved the original hang was internal-
// RAM exhaustion. Moving the Port Scan and Ping Sweep result tables to lazy
// PSRAM storage recovered about 45 KB on hardware, raising the final largest
// internal block from 20,468 to 65,524 bytes with Bluedroid active. This predates
// notifications and occurs with no phone or ANCS connection, so ANCS was not the
// cause. See tasks/COEXIST-NOTES.md.
#define ARGUS_RADIO_COEXIST 1

// Measurement-only diagnostics: log boot-time internal-heap checkpoints and
// reset reasons to serial and /Settings/coexlog.txt, plus the pre-WiFi heap
// checkpoint. Keep this off in release builds. ARGUS_RADIO_COEXIST independently
// controls the production BLE keepalive and simultaneous-radio behavior above.
#define ARGUS_COEX_MEASURE 0
