#pragma once
//
// dst_rules.h - daylight-saving rules as pure calendar arithmetic.
//
// WHY THIS EXISTS. The World Clock showed Los Angeles, Denver, New York and
// Paris an hour slow for most of the year. Its comment claimed DST was
// "intentionally not modelled ... rather than silently wrong twice a year",
// which inverts the truth: a fixed standard offset is wrong for the ~8 months
// those zones spend on summer time and right for the other four. The comment
// made a defect read as a decision.
//
// The US rule already existed, correct, as a static inside gps_screen.cpp where
// nothing else could reach it and no test could see it. It moves here whole, so
// there is ONE definition of "is the US on DST" rather than a copy per caller.
//
// GRANULARITY, stated because it is a real limit: these answer per DAY, not per
// hour. A real transition happens at 02:00 local (US), 01:00 UTC (EU) or 02:00
// local (AU), so on the changeover day itself a zone can read an hour off for
// part of that day. That is inherited from the original us_dst_active() and is
// deliberate for now - a reference clock that is right 363 days a year beats one
// that is wrong for eight months. Fixing it properly needs the zone's local
// wall-clock hour, which the World Clock does not currently carry.
//
// C++11 only: the ESP32 core builds this at -std=gnu++11 even though the host
// test suite is C++17. No Arduino, no LVGL - keep it host-testable.

#include <stdint.h>

// Which DST regime a zone follows. Named for the rule, not the place, because
// several places share each rule.
enum class DstRule : uint8_t {
    None,   // no daylight saving: Honolulu, UTC, Moscow, Dubai, Tokyo
    US,     // second Sunday March -> first Sunday November
    EU,     // last Sunday March -> last Sunday October
    AU,     // southern hemisphere: first Sunday October -> first Sunday April
};

// Day of week for a civil date. 0 = Sunday. Zeller's congruence; month is 1..12
// and day is 1..31. Exposed because the nth/last-weekday helpers below are the
// part most worth testing directly.
int dst_day_of_week(int year, int month, int day);

// Day-of-month of the nth given weekday (nth is 1-based, weekday 0 = Sunday).
// Does not check that the nth actually exists in that month; callers here only
// ask for 1st and 2nd, which always do.
int dst_nth_weekday(int year, int month, int weekday, int nth);

// Day-of-month of the LAST given weekday of a month.
int dst_last_weekday(int year, int month, int weekday);

// Is daylight saving in force for `rule` on this civil date?
//
// The date must be the date IN THAT ZONE, not UTC. A caller holding UTC should
// shift by the zone's standard offset first: near midnight the two differ, and
// on a transition day that is exactly when the answer flips.
bool dst_active(DstRule rule, int year, int month, int day);

// The US rule on its own, kept as a named entry point because gps_screen.cpp's
// longitude-to-timezone path asks this question directly and reads better for
// saying so.
inline bool us_dst_active(int year, int month, int day)
{
    return dst_active(DstRule::US, year, month, day);
}
