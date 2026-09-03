#include "clock_time.h"

namespace clocktime {

// Civil-date arithmetic rather than mktime(). mktime() interprets its input as
// LOCAL time and is free to re-normalise tm_isdst, which on a host in a
// DST-observing zone shifts the hour out from under the caller - exactly the
// class of silent one-hour error this module exists to stop. These are days
// since 1970-01-01, proleptic Gregorian, no timezone anywhere.

static long long days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;                                  // [0, 399]
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097LL + doe - 719468LL;
}

static void civil_from_days(long long z, int *y, int *m, int *d)
{
    z += 719468LL;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;                               // [0, 146096]
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);        // [0, 365]
    const long long mp  = (5 * doy + 2) / 153;                            // [0, 11]
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

// Add whole hours and re-normalise every field, including the day/month/year
// rollover a shift across midnight produces.
static DateTime shift_hours(const DateTime &in, int hours)
{
    const long long secs = days_from_civil(in.year, in.mon, in.day) * 86400LL
                         + (long long)in.hour * 3600LL
                         + (long long)in.min * 60LL
                         + in.sec
                         + (long long)hours * 3600LL;

    long long day = secs / 86400LL;
    long long rem = secs % 86400LL;
    if (rem < 0) { rem += 86400LL; day -= 1; }   // C++11 truncates toward zero

    DateTime out;
    civil_from_days(day, &out.year, &out.mon, &out.day);
    out.hour = (int)(rem / 3600);
    out.min  = (int)((rem % 3600) / 60);
    out.sec  = (int)(rem % 60);
    return out;
}

bool offset_plausible(int offset_hours)
{
    return offset_hours >= kMinOffsetHours && offset_hours <= kMaxOffsetHours;
}

DateTime local_to_utc(const DateTime &local, int offset_hours)
{
    return shift_hours(local, -offset_hours);
}

DateTime utc_to_local(const DateTime &utc, int offset_hours)
{
    return shift_hours(utc, offset_hours);
}

void tm_utc_to_local(struct tm *t, int offset_hours)
{
    DateTime utc;
    utc.year = t->tm_year + 1900;
    utc.mon  = t->tm_mon + 1;
    utc.day  = t->tm_mday;
    utc.hour = t->tm_hour;
    utc.min  = t->tm_min;
    utc.sec  = t->tm_sec;

    const DateTime loc = utc_to_local(utc, offset_hours);

    t->tm_year = loc.year - 1900;
    t->tm_mon  = loc.mon - 1;
    t->tm_mday = loc.day;
    t->tm_hour = loc.hour;
    t->tm_min  = loc.min;
    t->tm_sec  = loc.sec;

    // The callers render the day name and the calendar off these, and mktime()
    // used to recompute them. Nothing else does, so this has to.
    const long long days = days_from_civil(loc.year, loc.mon, loc.day);
    t->tm_wday  = (int)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
    t->tm_yday  = (int)(days - days_from_civil(loc.year, 1, 1));
    t->tm_isdst = 0;                              // local time is already resolved
}

int effective_saved_offset(int saved_offset, int file_version, bool manual_active)
{
    if (!offset_plausible(saved_offset)) return 0;
    if (file_version >= kFileVersion)    return saved_offset;

    // v1 file. The offset on the card is the last GPS/WiFi-detected one, which
    // pairs with the RTC only while Manual Time is off. With Manual Time on, the
    // old firmware had written LOCAL wall clock into the RTC, so the offset that
    // pairs with it is 0 - the value it never got around to persisting.
    return manual_active ? 0 : saved_offset;
}

}   // namespace clocktime
