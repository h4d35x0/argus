#pragma once

#include "clock_sync.h"

// Persisted timezone offset.
//
// INVARIANT: the RTC always holds UTC. clock_utc_offset (in main.cpp / the
// clock screen) shifts it to local time for display, and the world clock,
// Meshtastic and the SD log stamps all rely on the RTC half being UTC.
// Everything that writes the RTC -- GPS, NTP, the firmware-build-time
// fallback, and Manual Time -- must therefore write UTC and leave an offset
// behind that maps it back to local.
//
// The offset used to be RAM-only and reset to 0 on every boot, so after a
// reboot the watch showed UTC until the next GPS fix. This module persists it
// to the SD card so the correct local time survives reboots (the RTC is
// battery-backed and keeps UTC), and refreshes it automatically in the
// background whenever WiFi connects -- via NTP for the time and an
// IP-geolocation lookup for the offset, mirroring how a GPS lock sets both.
//
// Manual Time suppresses the GPS and WiFi SYNCS, but not the saved offset: the
// offset that was in force when the user set the time is persisted alongside
// it, so the restored pair still maps to the wall clock they typed in. (It did
// not used to be, which is what made a manually-set watch come up hours off
// after a reboot -- see the v1 migration in timezone.cpp.)

void timezone_init();                            // register WiFi hook + worker
void timezone_load_on_boot();                    // restore saved offset (+ migrate v1 files)
void timezone_note_detected(int offset_hours);   // persist a freshly-detected offset

// Record that the RTC was just set from a trusted source, and persist the
// stamp beside the offset. Call this instead of timezone_note_detected() from
// anything that actually WROTE the clock (GPS fix, NTP, Manual Time); the
// watch has no other way to know its own time is still vouched for. See
// clock_sync.h for what this cost when it was missing.
void timezone_note_synced(int offset_hours, clocksync::Source src);

// Last-sync stamp, or an invalid stamp when the clock has never been synced.
clocksync::Stamp timezone_last_sync();
void timezone_bg_tick();                         // main loop: apply background WiFi results

// Read the saved offset without touching the clock. Returns *fallback* when
// there is no usable saved value. Used during early boot, before
// timezone_load_on_boot() runs, by the firmware-build-time RTC seed.
int  timezone_peek_saved_offset(int fallback);
