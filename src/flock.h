#pragma once
#include <stdint.h>

// Checks a detected device against the surveillance-vendor OUI table and the
// device-name pattern list. `name` may be NULL or empty. Returns true if the
// device matched (and was newly logged after dedup).
bool flock_check(const uint8_t *mac6, int8_t rssi, const char *name, char source);

int  flock_get_count();
void flock_bg_tick();
void flock_reset_count();

// Standalone tile API -- starts/stops dedicated WiFi and BLE scans so the
// detector runs without the wardriver.
bool flock_start();
void flock_stop();
bool flock_is_running();

// Flock wants BOTH radios, but this board can only run one radio at a time, so
// a start may come up single-radio. These report which radios actually came up
// for the most recent flock_start(); both are false while the detector is
// stopped. The caller uses them to surface the degraded (single-radio) mode.
bool flock_wifi_active();
bool flock_ble_active();
