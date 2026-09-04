// test_clock_sync.cpp - "when was this clock last actually right?"
//
// The defect this guards is the one that hid for weeks: on 2026-09-02 the watch
// read seven hours slow because the RTC had carried Pacific wall clock since
// DEF CON, and nothing on the device could tell that its own clock was
// unvouched-for. Every SD log stamp written in that period is misdated.
//
// So the cases that matter are the ones where a WRONG answer would silence the
// warning: a malformed stamp read as a real date, a stamp in the future treated
// as fresh, or a missing stamp treated as "fine". All three must fail loudly
// toward "stale", because a fabricated sync time is worse than an admitted
// absence.

#include "wl_test.h"
#include "clock_sync.h"
#include <string.h>

using clocksync::Stamp;
using clocksync::Source;

static clocktime::DateTime dt(int y, int mo, int d, int h = 12, int mi = 0)
{
    clocktime::DateTime t;
    t.year = y; t.mon = mo; t.day = d; t.hour = h; t.min = mi; t.sec = 0;
    return t;
}

WL_TEST(clock_sync_parses_a_real_line)
{
    const Stamp s = clocksync::parse("offset=-4 v=2 synced=2026-09-03T13:41 src=1");
    WL_CHECK(s.valid);
    WL_CHECK(s.utc.year == 2026 && s.utc.mon == 9 && s.utc.day == 3);
    WL_CHECK(s.utc.hour == 13 && s.utc.min == 41);
    WL_CHECK(s.src == Source::Gps);
}

WL_TEST(clock_sync_round_trips)
{
    Stamp s;
    s.utc = dt(2026, 9, 3, 13, 41);
    s.src = Source::Ntp;
    s.valid = true;

    char buf[64];
    const int n = clocksync::format(buf, sizeof(buf), s);
    WL_CHECK(n > 0);
    WL_CHECK(strcmp(buf, "synced=2026-09-03T13:41 src=2") == 0);

    const Stamp back = clocksync::parse(buf);
    WL_CHECK(back.valid);
    WL_CHECK(back.utc.year == 2026 && back.utc.mon == 9 && back.utc.day == 3);
    WL_CHECK(back.utc.hour == 13 && back.utc.min == 41);
    WL_CHECK(back.src == Source::Ntp);
}

WL_TEST(clock_sync_rejects_anything_it_cannot_fully_read)
{
    // Each of these must read as "never synced" rather than as a partial date.
    const char *bad[] = {
        "offset=-4 v=2",                          // no stamp at all
        "synced=",                                // truncated
        "synced=2026-09",                         // truncated mid-date
        "synced=2026-09-03",                      // no time part
        "synced=2026-09-03T13",                   // no minutes
        "synced=20260903T1341 src=1",             // wrong separators
        "synced=2026-9-3T13:41 src=1",            // unpadded, so not 4-2-2
        "synced=abcd-09-03T13:41 src=1",          // non-numeric
        "synced=1999-09-03T13:41 src=1",          // year out of range
        "synced=2026-13-03T13:41 src=1",          // month out of range
        "synced=2026-09-32T13:41 src=1",          // day out of range
        "synced=2026-09-03T24:41 src=1",          // hour out of range
        "synced=2026-09-03T13:60 src=1",          // minute out of range
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const Stamp s = clocksync::parse(bad[i]);
        WL_CHECK(!s.valid);
        // And an invalid stamp must always read as stale, whatever "now" is.
        WL_CHECK(clocksync::is_stale(s, dt(2026, 9, 3)));
    }
    WL_CHECK(!clocksync::parse(NULL).valid);
}

WL_TEST(clock_sync_unknown_source_keeps_the_date)
{
    // The source is context, not proof. An unreadable src must not throw away a
    // perfectly good sync time - that would raise a false staleness warning.
    const Stamp s = clocksync::parse("synced=2026-09-03T13:41 src=99");
    WL_CHECK(s.valid);
    WL_CHECK(s.src == Source::None);
    WL_CHECK(!clocksync::is_stale(s, dt(2026, 9, 4)));

    const Stamp t = clocksync::parse("synced=2026-09-03T13:41");
    WL_CHECK(t.valid);
    WL_CHECK(t.src == Source::None);
}

WL_TEST(clock_sync_age_in_days)
{
    Stamp s;
    s.utc = dt(2026, 9, 3, 13, 41);
    s.src = Source::Gps;
    s.valid = true;

    WL_CHECK(clocksync::age_days(s, dt(2026, 9, 3)) == 0);
    WL_CHECK(clocksync::age_days(s, dt(2026, 9, 4)) == 1);
    WL_CHECK(clocksync::age_days(s, dt(2026, 10, 3)) == 30);

    // Across a year boundary and a leap day, where naive day-of-year maths
    // breaks.
    Stamp y;
    y.utc = dt(2023, 12, 31); y.src = Source::Gps; y.valid = true;
    WL_CHECK(clocksync::age_days(y, dt(2024, 1, 1)) == 1);
    WL_CHECK(clocksync::age_days(y, dt(2024, 3, 1)) == 61);   // 2024 is a leap year
}

WL_TEST(clock_sync_staleness_boundary)
{
    Stamp s;
    s.utc = dt(2026, 9, 1);
    s.src = Source::Gps;
    s.valid = true;

    // Exactly at the threshold is still fresh; one day past it is not.
    WL_CHECK(!clocksync::is_stale(s, dt(2026, 9, 1 + clocksync::kStaleDays)));
    WL_CHECK(clocksync::is_stale(s, dt(2026, 9, 1 + clocksync::kStaleDays + 1)));
}

WL_TEST(clock_sync_future_stamp_is_not_freshness)
{
    // A clock corrected BACKWARDS leaves a stamp in the future. Reporting that
    // as "synced 0 days ago" would vouch for a clock nothing has vouched for -
    // this is the DEF CON failure shape, where the RTC was confidently wrong.
    Stamp s;
    s.utc = dt(2026, 12, 25);
    s.src = Source::Manual;
    s.valid = true;

    WL_CHECK(clocksync::age_days(s, dt(2026, 9, 3)) < 0);
    WL_CHECK(clocksync::is_stale(s, dt(2026, 9, 3)));
}

WL_TEST(clock_sync_format_never_overruns)
{
    Stamp s;
    s.utc = dt(2026, 9, 3, 13, 41);
    s.src = Source::Gps;
    s.valid = true;

    char small[8];
    memset(small, 'x', sizeof(small));
    const int n = clocksync::format(small, sizeof(small), s);
    WL_CHECK(n < (int)sizeof(small));
    WL_CHECK(small[sizeof(small) - 1] == '\0' || strlen(small) < sizeof(small));

    // An invalid stamp writes an empty string rather than junk.
    Stamp bad;
    bad.valid = false;
    char buf[32];
    WL_CHECK(clocksync::format(buf, sizeof(buf), bad) == 0);
    WL_CHECK(buf[0] == '\0');

    clocksync::format(NULL, 0, s);   // must not crash
}

WL_TEST(clock_sync_source_names_are_present)
{
    const Source all[] = { Source::None, Source::Gps, Source::Ntp, Source::Manual };
    for (int i = 0; i < 4; i++) {
        WL_CHECK(clocksync::source_name(all[i])[0] != '\0');
        WL_CHECK(strcmp(clocksync::source_name(all[i]), "?") != 0);
    }
}
