// Host tests for src/clock_time.cpp - the RTC(UTC) <-> local-face arithmetic.
//
// The bug these exist to prevent: the RTC and the UTC offset are a PAIR, and a
// writer that updates one without the other leaves the face reading hours off.
// The round-trip tests below assert the pairing over a CROSS-PRODUCT of offsets
// and wall-clock hours rather than one illustrative case, because every real
// instance of this defect was at a boundary - a shift across midnight, a month
// end, or a manually-set watch whose zone had changed since the trip.

#include "wl_test.h"
#include "clock_time.h"
#include <string.h>

using namespace clocktime;

static DateTime dt(int y, int mo, int d, int h, int mi, int s)
{
    DateTime v; v.year = y; v.mon = mo; v.day = d; v.hour = h; v.min = mi; v.sec = s;
    return v;
}

static bool same(const DateTime &a, const DateTime &b)
{
    return a.year == b.year && a.mon == b.mon && a.day == b.day
        && a.hour == b.hour && a.min == b.min && a.sec == b.sec;
}

WL_TEST(clock_time_offset_plausible)
{
    WL_CHECK(offset_plausible(0));
    WL_CHECK(offset_plausible(-12));
    WL_CHECK(offset_plausible(14));
    WL_CHECK(!offset_plausible(-13));
    WL_CHECK(!offset_plausible(15));
}

// The invariant, over every offset the file format admits and every hour of the
// day: whatever local time the user typed in has to come back out of the face.
WL_TEST(clock_time_manual_entry_round_trips)
{
    bool all_ok = true;
    for (int off = kMinOffsetHours; off <= kMaxOffsetHours; off++) {
        for (int h = 0; h < 24; h++) {
            const DateTime local = dt(2026, 9, 2, h, 55, 0);
            if (!same(utc_to_local(local_to_utc(local, off), off), local)) all_ok = false;
        }
    }
    WL_CHECK(all_ok);
}

// Same round trip across a month end, a year end and a leap day, where the
// shift moves the date and not just the hour.
WL_TEST(clock_time_round_trips_across_boundaries)
{
    const DateTime cases[] = {
        dt(2026, 1,  1,  0,  0,  0),
        dt(2026, 1,  1, 23, 59, 59),
        dt(2026, 12, 31, 23, 30,  0),
        dt(2024, 2,  29, 0,  30,  0),   // leap day
        dt(2024, 3,  1,  0,  15,  0),
        dt(2026, 8,  31, 22, 10,  0),
    };
    bool all_ok = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (int off = kMinOffsetHours; off <= kMaxOffsetHours; off++)
            if (!same(utc_to_local(local_to_utc(cases[i], off), off), cases[i]))
                all_ok = false;
    WL_CHECK(all_ok);
}

// Concrete conversions, checked by hand, so a sign flip cannot hide behind a
// round trip that is wrong in both directions.
WL_TEST(clock_time_known_conversions)
{
    // 13:55 local at UTC-4 (US Eastern, DST) is 17:55 UTC, same day.
    WL_CHECK(same(local_to_utc(dt(2026, 9, 2, 13, 55, 0), -4),
                  dt(2026, 9, 2, 17, 55, 0)));
    WL_CHECK(same(utc_to_local(dt(2026, 9, 2, 17, 55, 0), -4),
                  dt(2026, 9, 2, 13, 55, 0)));
    WL_CHECK(same(local_to_utc(dt(2026, 9, 2, 0, 30, 0), -4),
                  dt(2026, 9, 2, 4, 30, 0)));
    // Backwards across midnight: 01:00 local at UTC+2 is 23:00 the day before.
    WL_CHECK(same(local_to_utc(dt(2026, 9, 2, 1, 0, 0), 2),
                  dt(2026, 9, 1, 23, 0, 0)));
    // Forwards across midnight: 23:00 UTC at +2 is 01:00 the next day.
    WL_CHECK(same(utc_to_local(dt(2026, 9, 2, 23, 0, 0), 2),
                  dt(2026, 9, 3, 1, 0, 0)));
    // Across a year end.
    WL_CHECK(same(utc_to_local(dt(2026, 12, 31, 23, 0, 0), 2),
                  dt(2027, 1, 1, 1, 0, 0)));
    // Across a leap day, backwards.
    WL_CHECK(same(local_to_utc(dt(2024, 3, 1, 0, 30, 0), 2),
                  dt(2024, 2, 29, 22, 30, 0)));
}

// The exact shape of the reported fault: a watch whose saved offset came from a
// trip (UTC-7) but whose RTC was later set by hand at home (UTC-4). Under the
// old pairing the face lost the whole difference on the next boot.
WL_TEST(clock_time_stale_offset_cannot_shift_a_manual_setting)
{
    const int trip_offset = -7;    // last detected on the road
    const int home_offset = -4;    // where the watch actually is

    // Manual entry converts with the offset in force and persists THAT offset,
    // so restoring the pair reproduces the entered time.
    const DateTime entered = dt(2026, 9, 2, 13, 55, 0);
    const DateTime in_rtc  = local_to_utc(entered, home_offset);
    WL_CHECK(same(utc_to_local(in_rtc, home_offset), entered));

    // Applying the stale trip offset instead is the defect: a three-hour error,
    // the difference between the two zones.
    WL_CHECK(utc_to_local(in_rtc, trip_offset).hour == 10);

    // And the old manual path was worse still: it wrote LOCAL into the RTC and
    // persisted nothing, so the next boot applied the trip offset to local time
    // and lost the full seven hours.
    WL_CHECK(utc_to_local(entered, trip_offset).hour == 6);
}

// v1 cards were written by firmware whose Manual Time path stored LOCAL time in
// the RTC and never persisted the matching 0.
WL_TEST(clock_time_v1_migration)
{
    // Manual Time off: a v1 offset does pair with the RTC (GPS/NTP wrote UTC).
    WL_CHECK(effective_saved_offset(-7, 1, false) == -7);
    // Manual Time on: the RTC holds local, so the pairing offset is 0.
    WL_CHECK(effective_saved_offset(-7, 1, true) == 0);
    // v2 cards are self-consistent either way - no migration.
    WL_CHECK(effective_saved_offset(-7, 2, false) == -7);
    WL_CHECK(effective_saved_offset(-7, 2, true)  == -7);
    WL_CHECK(effective_saved_offset(0,  2, true)  == 0);
    // A corrupt or out-of-range offset is never applied.
    WL_CHECK(effective_saved_offset(-99, 2, false) == 0);
    WL_CHECK(effective_saved_offset(15,  1, false) == 0);
}

// The readers render the day name and the calendar off tm_wday/tm_yday, which
// mktime() used to recompute. Dropping mktime() means clock_time.cpp owes them.
WL_TEST(clock_time_tm_shift_fills_wday_and_yday)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 9 - 1;
    t.tm_mday = 2;          // 2026-09-02 is a Wednesday
    t.tm_hour = 17;
    t.tm_min  = 55;

    tm_utc_to_local(&t, -4);
    WL_CHECK(t.tm_hour == 13);
    WL_CHECK(t.tm_mday == 2);
    WL_CHECK(t.tm_wday == 3);                        // Wednesday
    WL_CHECK(t.tm_yday == 244);                      // 2026 is not a leap year
    WL_CHECK(t.tm_year == 2026 - 1900);

    // A shift that crosses midnight has to roll the weekday with the date.
    memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 9 - 1;
    t.tm_mday = 2;
    t.tm_hour = 2;
    tm_utc_to_local(&t, -4);                         // -> 2026-09-01 22:00
    WL_CHECK(t.tm_mday == 1);
    WL_CHECK(t.tm_hour == 22);
    WL_CHECK(t.tm_wday == 2);                        // Tuesday

    // Offset 0 must still fill wday/yday, not leave them as the caller left them.
    memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 1 - 1;
    t.tm_mday = 1;                                   // 2026-01-01 is a Thursday
    tm_utc_to_local(&t, 0);
    WL_CHECK(t.tm_wday == 4);
    WL_CHECK(t.tm_yday == 0);
}
