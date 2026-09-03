#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "gps_health.h"

void gps_screen_create();

// Drain the GPS UART into TinyGPSPlus AND the GSV accumulator. Call once per
// main-loop iteration while the radio is on; it no-ops when it is off. Replaces
// instance.gps.loop(), which reads the port itself and left no way to tee the
// stream for GSV. See the SATELLITES IN VIEW block in gps_screen.cpp.
void gps_screen_pump();
void gps_screen_restore_power();
void gps_screen_show();
bool gps_screen_is_active();
bool gps_screen_is_powered();
// Instantaneous fix predicate: true only if THIS tick's NMEA is fresh and >= 4
// satellites are in view. Use for UI and for one-shot reads where a momentary
// gap should read as "no fix".
bool gps_screen_has_lock();

// Debounced fix predicate for the DETECTION and SURVEY consumers (tail-detection
// cell trail, Threat Radar waypoints, wardriver rows). Rises instantly with the
// fix, but a loss must persist ~10 s before it goes false, and the satellite
// floor has hysteresis (acquire 4, hold 3). This exists because the raw
// predicate flapped 26 times on the 2026-07-30 field run, and every flap blanked
// the geo-cell trail the follow classifier depends on.
//
// While a drop is being debounced this vouches for a position up to ~10 s old -
// bounded and survey-grade, unlike gps.location.isValid(), which can return a
// fix from a previous power cycle. Never gate recorded coordinates on isValid()
// alone; use this.
bool gps_screen_has_stable_lock();

// WHY there is no fix, for callers that must explain themselves to the user
// (the GPS screen's Status row, the WarDrive readiness gate). Distinguishes a
// dead receiver from a blocked sky from normal acquisition - a distinction the
// watch used to render as "--" in all three cases, which is what turned a
// reception problem into three sessions of debugging on 2026-08-03.
GpsHealth gps_screen_health();

// Seconds WITHOUT A FIX - not seconds in the current health state. Resets only
// on an actual (stable) lock. "No satellites" for 8 s is normal; for 8 minutes
// it means move, or the antenna is faulty.
//
// Measuring time-in-state instead was tried and field-failed on 2026-08-04:
// satellites flickering 0 -> 1 -> 0 flap the classification, which reset the
// counter every few seconds, so a two-minute failure displayed as a timer that
// never climbed past a few seconds.
uint32_t gps_screen_health_secs();
