#pragma once
#include <stdint.h>

// ble_detect_pipeline - firmware glue that wires the PURE, host-tested BLE
// anti-stalking detectors into the live BLE advertisement stream. Each scanned
// advert is gated by detect::is_unwanted_tracker() (a separated/unknown Apple
// Find My or third-party tracker), folded into a TailDetector keyed by MAC +
// coarse GPS cell, and the follow verdict is reported into the SHARED
// ThreatState via detect_pipeline_feed_tracker() (Airtag domain) - so BLE and
// WiFi threat signals drive one unified posture / log / HADES accent.
//
// It complements, and does not duplicate, the existing airtag -> threat_radar
// path (which drives the Radar screen). This path feeds the aggregator only.
//
// SAFETY / boot: like detect_pipeline, this lives outside src/detect/ and NEVER
// runs on the boot path. It PIGGYBACKS on ble_scan_manager: it attaches its scan
// consumer ONLY while another BLE consumer (AirTag / Flipper / Skimmer /
// wardriver) has already brought the controller up, so it can never be the first
// consumer and never triggers the esp_bt_controller_enable() that hangs/boot-
// loops when WiFi holds the SRAM. Detaches as soon as no other consumer remains.
//
// USAGE: call once per second from the 1Hz block in loop(), NOT from setup().
// now_sec MUST be a monotonic seconds source (millis()/1000).
void ble_detect_pipeline_tick(uint32_t now_sec);
