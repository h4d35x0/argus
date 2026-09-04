#pragma once
//
// clock_sync.h - when the RTC was last set from a trusted source, and by what.
//
// WHY THIS EXISTS. The watch could not tell that its own clock was wrong. On
// 2026-09-02 the face read seven hours slow because the RTC had been carrying
// Pacific wall clock since DEF CON: Manual Time had written LOCAL into an RTC
// documented as UTC, and nothing afterwards contradicted it. That defect is
// fixed (see clock_time.h), but the deeper problem is not: an RTC drifts, a
// coin cell dies, a user sets the time by hand in the wrong zone, and the watch
// keeps presenting whatever it holds with exactly as much confidence as a fresh
// GPS lock. There was no last-synced stamp and no warning, so a wrong clock
// rode home unnoticed for weeks and silently misdated every SD log stamp,
// wardrive row and detection record written in that time.
//
// So: record WHEN the RTC was last set and by WHAT, persist it beside the
// offset, and let the UI say "not synced in 45 days" instead of implying the
// time is trustworthy.
//
// The stamp is stored as the UTC civil date-time, not an epoch, because the
// only clock available to read it back is the very RTC whose trustworthiness is
// in question - a stored epoch would still need the same arithmetic and would
// be harder to eyeball on the card.
//
// C++11 only, no Arduino, no LVGL: host-tested in test/test_clock_sync.cpp.

#include <stddef.h>
#include <stdint.h>
#include "clock_time.h"

namespace clocksync {

// What set the clock. Ordered by how much it should be trusted, though nothing
// here branches on that - it is recorded so a stale-clock report can say which
// source went quiet.
enum class Source : uint8_t {
    None = 0,   // never synced, or unreadable
    Gps  = 1,   // GPS fix: UTC straight off the constellation
    Ntp  = 2,   // NTP over WiFi
    Manual = 3, // the user typed it in; correct only if they were correct
};

struct Stamp {
    clocktime::DateTime utc;   // when the sync happened, in UTC
    Source              src;
    bool                valid;
};

// Days beyond which a clock is called stale. Two weeks is chosen to be longer
// than any normal gap between a GPS fix or a WiFi association, and far shorter
// than the multi-week drift that actually went unnoticed.
static const int kStaleDays = 14;

// Parse "synced=YYYY-MM-DDTHH:MM src=<n>" out of a timezone.txt line. Missing
// or malformed keys yield an invalid stamp rather than a guess: a fabricated
// sync time is worse than an admitted absence, because it would silence the
// very warning this module exists to raise.
Stamp parse(const char *line);

// Render the two keys, without a leading space. Returns the number of
// characters written, always NUL-terminating.
int format(char *buf, size_t cap, const Stamp &s);

// Whole days from the stamp to `now_utc`. Negative when the stamp is in the
// future, which happens after a clock is corrected BACKWARDS and must not be
// reported as freshness.
int age_days(const Stamp &s, const clocktime::DateTime &now_utc);

// Should the UI warn? True when there is no stamp at all, when the stamp is
// older than kStaleDays, or when it sits in the future - all three mean the
// displayed time is not vouched for.
bool is_stale(const Stamp &s, const clocktime::DateTime &now_utc);

// Short human label for the source, for a Settings row.
const char *source_name(Source s);

}   // namespace clocksync
