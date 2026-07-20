#pragma once
#include <stdint.h>

// detect_pipeline - firmware glue that wires the PURE, host-tested WiFi-side
// detectors into the live promiscuous beacon stream, folds their verdicts into
// the shared ThreatState aggregator, records forensic edges to the ThreatLog,
// and drives the UI (HADES-red brand accent + HexHound mood). See the data-flow
// diagram and integration guide in src/detect/README.md.
//
// SCOPE THIS PASS: WiFi-side only, because the WiFi beacon path is boot-safe.
//   * evil_twin    (rogue-AP / evil-twin)   <- WiFi beacons
//   * beacon_flood (fake-AP / beacon flood)  <- WiFi beacons
// The BLE detectors (ble_spam, tracker_ident, tail_detect) and deauth_flood are
// wired SEPARATELY: they need ble_scan_add() / promiscuous mgmt hooks that bring
// up the BLE controller, which must be verified on-device on its own.
//
// It lives OUTSIDE src/detect/ on purpose: the src/detect/ library stays pure
// (no Arduino / LVGL / ESP-IDF / SD / clock), and this file is the only place
// the hardware-facing glue (WiFi callback, SD append, UI hooks) touches it.
//
// USAGE: call once per second from the 1Hz block in loop() - NOT from setup().
// The first call lazily registers the WiFi beacon callback; nothing here runs on
// the boot path. now_sec MUST be a MONOTONIC seconds source that never rewinds
// (the sliding windows + decay in the pure detectors assume a forward clock);
// millis()/1000 is ideal.
void detect_pipeline_tick(uint32_t now_sec);
