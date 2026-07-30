#pragma once
#include <lvgl.h>

void gps_screen_create();
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
