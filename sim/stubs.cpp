// Link-time stubs for the parts of ARGUS the simulator deliberately does not
// build. SIM ONLY.
//
// Two rules for anything in this file:
//   1. A stub must never make the UI show something the firmware would not.
//      These are navigation entry points and one threat accessor; none of them
//      invent content, they just do nothing or return a value the harness set.
//   2. If a stub starts needing real behaviour to make a demo look right, that
//      is the signal to compile the real file instead, not to fake it here.
#include "Arduino.h"
#include "LilyGoLib.h"
#include "clock_time.h"

// ---- the clock pair, implemented for real rather than stubbed ---------------
// main.cpp owns these on the device. They are three lines of civil arithmetic
// and world_clock_screen.cpp's LOCAL row depends on them, so faking them would
// mean the one scene that demonstrates the DST work could not be trusted.
// tm_utc_to_local() is the SAME function the firmware calls, so the LOCAL row
// in a captured frame is computed exactly as it is on the watch.
static int s_utc_offset = -4;               // EDT, matching the device default

void sim_set_utc_offset(int hours) { s_utc_offset = hours; }
int  clock_screen_get_utc_offset(void) { return s_utc_offset; }

void clock_screen_get_local_time(struct tm *out)
{
    if (!out) return;
    instance.rtc.getDateTime(out);          // UTC, as the RTC is documented
    clocktime::tm_utc_to_local(out, s_utc_offset);
}

// ---- threat state ----------------------------------------------------------
// theme.cpp reads this to decide whether the accent goes HADES-red. The harness
// sets it so we can capture the real threat-state chrome (which IS the
// firmware's own rendering) rather than mocking up a red screen.
static int s_top_level = 0;
void sim_set_threat_level(int lvl) { s_top_level = lvl; }
int  threatradar_top_level(void)   { return s_top_level; }

// ---- navigation targets ----------------------------------------------------
// time_screen.cpp's tiles call these. In the sim they are no-ops: the harness
// drives which screen is shown directly, so a stray tap cannot navigate us into
// an unbuilt screen mid-capture.
void clock_screen_show(void)         {}
void alarm_screen_show(void)         {}
void sun_moon_screen_show(void)      {}
void flashlight_screen_show(void)    {}
void meshtastic_screen_show(void)    {}
void settings_screen_show(void)      {}
void notifications_screen_show(void) {}

// ---- alarm audio -----------------------------------------------------------
// timer_screen.cpp rings the chime through these. alarm.cpp is not compiled
// here (it needs ESP-IDF driver/i2s.h), and a simulator has no speaker, so
// silence is the honest behaviour rather than a fake.
void alarm_play_chime_loop(unsigned char) {}
void alarm_stop_chime_loop(void)          {}
