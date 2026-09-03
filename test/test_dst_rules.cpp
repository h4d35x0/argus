// test_dst_rules.cpp - daylight saving, per zone.
//
// The defect these guard is not a crash, it is being QUIETLY AN HOUR OFF. The
// World Clock shipped fixed standard offsets and a comment claiming DST was
// "intentionally not modelled ... rather than silently wrong twice a year",
// which has it backwards: a standard offset is wrong for the roughly eight
// months a northern zone spends on summer time and right for the other four.
// Reported from the device on 2026-09-03 as "DST is still an hour off".
//
// So the cases that matter are the BOUNDARIES - the transition Sundays and the
// days either side of them - plus the southern-hemisphere window, which wraps
// the new year and therefore has the opposite shape to the northern rules.
//
// Every transition date below was computed from the calendar, not recalled.

#include "wl_test.h"
#include "dst_rules.h"

WL_TEST(dst_day_of_week_matches_known_dates)
{
    // 0 = Sunday. Anchors chosen far apart, including a leap-year February and
    // a century boundary, since Zeller's month/year shuffle is where it breaks.
    WL_CHECK(dst_day_of_week(2026, 9, 3) == 4);    // Thursday
    WL_CHECK(dst_day_of_week(2026, 1, 1) == 4);    // Thursday
    WL_CHECK(dst_day_of_week(2024, 2, 29) == 4);   // Thursday, leap day
    WL_CHECK(dst_day_of_week(2000, 1, 1) == 6);    // Saturday
    WL_CHECK(dst_day_of_week(2026, 3, 8) == 0);    // Sunday, US DST start
}

WL_TEST(dst_nth_and_last_weekday)
{
    // Second Sunday of March, then first Sunday of November.
    WL_CHECK(dst_nth_weekday(2024, 3, 0, 2) == 10);
    WL_CHECK(dst_nth_weekday(2025, 3, 0, 2) == 9);
    WL_CHECK(dst_nth_weekday(2026, 3, 0, 2) == 8);
    WL_CHECK(dst_nth_weekday(2027, 3, 0, 2) == 14);
    WL_CHECK(dst_nth_weekday(2024, 11, 0, 1) == 3);
    WL_CHECK(dst_nth_weekday(2026, 11, 0, 1) == 1);   // the 1st IS a Sunday

    // Last Sunday of March / October, the EU rule.
    WL_CHECK(dst_last_weekday(2024, 3, 0) == 31);
    WL_CHECK(dst_last_weekday(2026, 3, 0) == 29);
    WL_CHECK(dst_last_weekday(2027, 3, 0) == 28);
    WL_CHECK(dst_last_weekday(2024, 10, 0) == 27);
    WL_CHECK(dst_last_weekday(2026, 10, 0) == 25);
    WL_CHECK(dst_last_weekday(2027, 10, 0) == 31);   // the 31st IS a Sunday

    // February, both leap and common, since last_weekday walks back from the
    // month length and that is the only place the length matters.
    WL_CHECK(dst_last_weekday(2024, 2, 0) == 25);    // leap
    WL_CHECK(dst_last_weekday(2026, 2, 0) == 22);    // common
}

WL_TEST(dst_none_is_always_off)
{
    // Honolulu, UTC, Moscow, Dubai, Tokyo. Checked across a whole year so a
    // rule accidentally falling through to a northern branch would show up.
    for (int m = 1; m <= 12; m++)
        for (int d = 1; d <= 28; d++)
            WL_CHECK(!dst_active(DstRule::None, 2026, m, d));
}

WL_TEST(dst_us_boundaries)
{
    // 2026: starts Sunday 8 March, ends Sunday 1 November.
    WL_CHECK(!dst_active(DstRule::US, 2026, 3, 7));   // day before
    WL_CHECK(dst_active(DstRule::US, 2026, 3, 8));    // start day, on
    WL_CHECK(dst_active(DstRule::US, 2026, 3, 9));
    WL_CHECK(dst_active(DstRule::US, 2026, 10, 31));  // still on
    WL_CHECK(!dst_active(DstRule::US, 2026, 11, 1));  // end day, off
    WL_CHECK(!dst_active(DstRule::US, 2026, 11, 2));

    // 2027 shifts both dates by a week; a hardcoded day would fail here.
    WL_CHECK(!dst_active(DstRule::US, 2027, 3, 13));
    WL_CHECK(dst_active(DstRule::US, 2027, 3, 14));
    WL_CHECK(dst_active(DstRule::US, 2027, 11, 6));
    WL_CHECK(!dst_active(DstRule::US, 2027, 11, 7));

    // Deep winter and deep summer.
    WL_CHECK(!dst_active(DstRule::US, 2026, 1, 15));
    WL_CHECK(!dst_active(DstRule::US, 2026, 12, 15));
    WL_CHECK(dst_active(DstRule::US, 2026, 7, 4));

    // The reported symptom: 2026-09-03, New York and Los Angeles must be on
    // summer time, i.e. an hour ahead of their standard offset.
    WL_CHECK(dst_active(DstRule::US, 2026, 9, 3));

    // us_dst_active() is the same rule under its own name.
    WL_CHECK(us_dst_active(2026, 9, 3));
    WL_CHECK(!us_dst_active(2026, 12, 25));
}

WL_TEST(dst_eu_boundaries)
{
    // 2026: starts Sunday 29 March, ends Sunday 25 October. Note this differs
    // from the US by several weeks at BOTH ends - the fortnight where Paris and
    // New York disagree about DST is exactly where one shared rule would be
    // wrong, which is why the rules are separate.
    WL_CHECK(!dst_active(DstRule::EU, 2026, 3, 28));
    WL_CHECK(dst_active(DstRule::EU, 2026, 3, 29));
    WL_CHECK(dst_active(DstRule::EU, 2026, 10, 24));
    WL_CHECK(!dst_active(DstRule::EU, 2026, 10, 25));
    WL_CHECK(!dst_active(DstRule::EU, 2026, 11, 1));

    // Mid-March: the US is already on summer time, the EU is not yet.
    WL_CHECK(dst_active(DstRule::US, 2026, 3, 20));
    WL_CHECK(!dst_active(DstRule::EU, 2026, 3, 20));

    // Late October: the EU has fallen back, the US has not.
    WL_CHECK(!dst_active(DstRule::EU, 2026, 10, 28));
    WL_CHECK(dst_active(DstRule::US, 2026, 10, 28));

    // The reported symptom for Paris.
    WL_CHECK(dst_active(DstRule::EU, 2026, 9, 3));
}

WL_TEST(dst_au_window_wraps_the_new_year)
{
    // Sydney: starts first Sunday October, ends first Sunday April. Active
    // across the new year, which is the opposite shape to the northern rules
    // and the case a copy-pasted US branch would get exactly backwards.
    WL_CHECK(!dst_active(DstRule::AU, 2026, 10, 3));
    WL_CHECK(dst_active(DstRule::AU, 2026, 10, 4));    // start day, on
    WL_CHECK(dst_active(DstRule::AU, 2026, 12, 25));   // southern summer
    WL_CHECK(dst_active(DstRule::AU, 2026, 1, 15));    // still summer
    WL_CHECK(dst_active(DstRule::AU, 2026, 4, 4));     // day before the end
    WL_CHECK(!dst_active(DstRule::AU, 2026, 4, 5));    // end day, off
    WL_CHECK(!dst_active(DstRule::AU, 2026, 7, 1));    // southern winter

    // September: northern zones are ON, Sydney is OFF. Getting this backwards
    // is the whole reason the southern rule exists.
    WL_CHECK(!dst_active(DstRule::AU, 2026, 9, 3));
    WL_CHECK(dst_active(DstRule::US, 2026, 9, 3));
}

WL_TEST(dst_every_day_of_a_year_is_classifiable)
{
    // Not checking WHICH answer, only that no date trips an unhandled path and
    // that each northern rule turns on and off exactly once across the year.
    static const int kLen[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const DstRule rules[] = { DstRule::US, DstRule::EU };
    for (int r = 0; r < 2; r++) {
        int transitions = 0;
        bool prev = dst_active(rules[r], 2026, 1, 1);
        for (int m = 1; m <= 12; m++) {
            for (int d = 1; d <= kLen[m]; d++) {
                const bool cur = dst_active(rules[r], 2026, m, d);
                if (cur != prev) transitions++;
                prev = cur;
            }
        }
        WL_CHECK(transitions == 2);   // exactly one on, one off
    }

    // The southern rule wraps, so within a calendar year it also crosses twice
    // (off in April, back on in October) but STARTS the year active.
    WL_CHECK(dst_active(DstRule::AU, 2026, 1, 1));
    WL_CHECK(dst_active(DstRule::AU, 2026, 12, 31));
}
