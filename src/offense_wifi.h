#pragma once
#include <stdint.h>
#include <stddef.h>

// Shared WiFi-injection owner for the OFFENSE transmit tools (beacon flood,
// deauth, ...). One place carries the coexistence guard + a PINNED channel (the
// beacon manager hops every 200ms; injection tools must control the channel), so
// each tool does not re-implement WiFi.mode() bring-up.
//
// COEXISTENCE (identical rule to wifi_beacon_manager start_wifi()): WiFi.mode()
// HANGS the watch if the BLE controller is up, so claim() REFUSES when BLE is
// active. It also refuses when a beacon scan already owns WiFi (in hopping mode),
// so an injection tool never fights the detectors for the radio. Single-owner:
// only one offense TX tool holds WiFi at a time.

// Attempt to take WiFi for raw injection on `channel` (1-13). Returns false if
// BLE is up, a beacon scan already owns WiFi, or another offense tool holds it.
bool offense_wifi_claim(uint8_t channel);

// Transmit one raw 802.11 frame. Returns true on ESP_OK. No-op if not claimed.
bool offense_wifi_tx(const uint8_t *frame, size_t len);

// Re-pin the channel (for tools that sweep channels, e.g. beacon flood).
void offense_wifi_set_channel(uint8_t channel);

// Release WiFi (WiFi.mode(WIFI_OFF)). Safe to call when not claimed.
void offense_wifi_release();

// True while an offense tool holds WiFi.
bool offense_wifi_held();
