#pragma once
#include <stdint.h>

// Forward-declare the pure follow-verdict flag so the BLE side can feed the
// shared aggregator without this header pulling in the whole detect library.
namespace detect { enum class TailFlag : uint8_t; }

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

// Feed an unwanted-tracker follow verdict into the SHARED ThreatState (Airtag
// domain), thread-safe against the WiFi beacon callback and the 1Hz tick. The
// BLE detect pipeline calls this so BLE + WiFi threat signals drive ONE unified
// posture / forensic log / HADES accent (driven by detect_pipeline_tick), rather
// than two pipelines stomping argus_set_threat() on each other. No-op-safe to
// call even before the first detect_pipeline_tick().
void detect_pipeline_feed_tracker(detect::TailFlag flag, uint32_t t_sec);

// --- Deauth-flood detector snapshot (for the Deauth status screen) -----------
// A live read of the passive deauth/disassoc detector, safe to call every UI
// refresh (main/LVGL task). The detector rides the existing WiFi promiscuous scan
// (piggyback-only, no new radio bring-up), so it only sees frames while some WiFi
// scan is running. flag: 0 None, 1 Elevated, 2 Flood (worst of per-AP / global).
struct DeauthSnapshot {
    uint8_t  flag;
    uint16_t global_rate;   // deauth+disassoc per minute, all APs, in-window
    uint8_t  tracked;       // distinct APs currently emitting disconnects
    uint8_t  top_bssid[6];  // the worst offender's BSSID (0s if none)
    uint16_t top_rate;      // that AP's per-minute rate
    int8_t   top_rssi;      // strongest RSSI seen for it (proximity hint)
};
void detect_pipeline_deauth_snapshot(DeauthSnapshot *out);
