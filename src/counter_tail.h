#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Vehicular counter-tail.
//
// A tracker in a pocket is one thing; a CAR tailing you is another. A following
// vehicle radiates persistent BLE (infotainment, TPMS, the driver's phone) and
// WiFi (its hotspot) the whole drive. Threat Radar's co-movement engine can
// already catch any MAC that travels with you — the only reason it doesn't catch
// cars is that the five built-in detectors only match narrow tracker signatures.
//
// This bridges the gap: it watches the wardriver's full ambient BLE/WiFi feed,
// promotes devices that are persistent and close enough to matter into Threat
// Radar as TR_CAT_VEHICLE, and lets the existing spatio-temporal scoring decide
// whether one is actually following. A learned "familiar" set — devices that
// co-move with you on two or more separate days (your own car, phone, earbuds)
// — is auto-suppressed and never broadcast, so only an UNFAMILIAR co-moving
// vehicle raises the alarm. The familiar tally is persisted to the SD card.
// ---------------------------------------------------------------------------

void counter_tail_set_enabled(bool on);
bool counter_tail_enabled();

// Prefilter — called from the wardriver scan feed for every ambient device, on
// the BT/WiFi task. Promotes a device into Threat Radar (TR_CAT_VEHICLE) once it
// has been seen enough times at usable signal. No-op while disabled.
void counter_tail_observe_ble(const uint8_t *mac6, int8_t rssi);
void counter_tail_observe_wifi(const uint8_t *bssid, int8_t rssi);

// Called by Threat Radar (main task) the first time a TR_CAT_VEHICLE contact is
// found co-moving: records that this device travelled with you today and, on a
// new day, bumps + persists its day tally.
void counter_tail_mark_comover(const uint8_t *mac6);

// True once a device has co-moved with you on >= 2 distinct days — i.e. it is
// one of your own daily vehicles/gear, not a tail. Used to suppress the alert.
bool counter_tail_is_familiar(const uint8_t *mac6);
