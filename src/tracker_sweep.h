#pragma once
#include <stdbool.h>

// Universal non-Apple BLE tracker sweep - Tile, Samsung SmartTag, Chipolo, via
// detect::classify_ble() (DeviceClass::BleTracker). Complements the AirTag
// detector (which owns Apple Find My). A hit feeds the Threat Radar correlation
// store as TR_CAT_TRACKER, so a tag that keeps re-appearing across the user's
// travel escalates to a "following you" alert. Defensive and PASSIVE: it only
// listens to advertisements the BLE scanner already receives and transmits
// nothing. Rides the shared ble_scan_manager (coexists with the other detectors).
//
// Correlation ceiling (surface honestly in UI): rotating-MAC tags (Find My)
// change identity ~every 15 min, capping the correlation window; static-MAC tags
// (Tile classic) correlate indefinitely.

bool tracker_sweep_start();
void tracker_sweep_stop();
bool tracker_sweep_is_running();
int  tracker_sweep_get_count();
