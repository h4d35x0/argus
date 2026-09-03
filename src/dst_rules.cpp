#include "dst_rules.h"

// Zeller's congruence. Moved verbatim from gps_screen.cpp, where it was a file
// static that only us_dst_active() could reach.
int dst_day_of_week(int year, int month, int day)
{
    if (month < 3) { month += 12; year--; }
    const int K = year % 100;
    const int J = year / 100;
    const int h = (day + (13 * (month + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    // Zeller returns 0 = Saturday; shift so 0 = Sunday.
    return (h + 6) % 7;
}

int dst_nth_weekday(int year, int month, int weekday, int nth)
{
    const int first_dow = dst_day_of_week(year, month, 1);
    // Days from the 1st to the first occurrence of `weekday`.
    const int shift = (weekday - first_dow + 7) % 7;
    return 1 + shift + (nth - 1) * 7;
}

int dst_last_weekday(int year, int month, int weekday)
{
    static const int kDays[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int len = kDays[month];
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) len = 29;
    }
    const int last_dow = dst_day_of_week(year, month, len);
    // Walk back from the last day to the most recent `weekday`.
    return len - ((last_dow - weekday + 7) % 7);
}

// Each branch is written as "before the start -> off, after the end -> off",
// with the months strictly inside handled first. The southern-hemisphere case
// is the one worth reading twice: its active window WRAPS the new year, so the
// early and late months are ON and the middle of the year is OFF - the opposite
// shape to the northern rules.
bool dst_active(DstRule rule, int year, int month, int day)
{
    switch (rule) {
    case DstRule::None:
        return false;

    case DstRule::US:
        // Second Sunday of March through the first Sunday of November.
        if (month < 3 || month > 11) return false;
        if (month > 3 && month < 11) return true;
        if (month == 3)
            return day >= dst_nth_weekday(year, 3, 0, 2);
        return day < dst_nth_weekday(year, 11, 0, 1);

    case DstRule::EU:
        // Last Sunday of March through the last Sunday of October. The real
        // switch is at 01:00 UTC simultaneously across the union; see the
        // granularity note in the header.
        if (month < 3 || month > 10) return false;
        if (month > 3 && month < 10) return true;
        if (month == 3)
            return day >= dst_last_weekday(year, 3, 0);
        return day < dst_last_weekday(year, 10, 0);

    case DstRule::AU:
        // First Sunday of October through the first Sunday of April, i.e. the
        // southern summer, which spans the year boundary.
        if (month > 4 && month < 10) return false;   // May..September: off
        if (month < 4 || month > 10) return true;    // Nov..March: on
        if (month == 10)
            return day >= dst_nth_weekday(year, 10, 0, 1);
        return day < dst_nth_weekday(year, 4, 0, 1);
    }
    return false;
}
