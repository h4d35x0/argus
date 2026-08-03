#include "log_retention.h"

namespace detlog {

long days_from_civil(int y, unsigned m, unsigned d)
{
    // Shift the year so March is month 1; that puts the leap day at the end of
    // the year and makes the day-of-year polynomial below exact.
    y -= (m <= 2) ? 1 : 0;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    const unsigned mp  = (m + (m > 2 ? (unsigned)-3 : 9)) % 12;  // Mar=0 .. Feb=11
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;             // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
    return (long)era * 146097 + (long)doe - 719468;
}

// Read exactly n digits. Returns false on any non-digit, which is what makes
// "MAC 3C:..." and free-form continuation lines fail the parse cleanly.
static bool take_digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

bool parse_stamp(const char *line, long *out_sec)
{
    if (!line || !out_sec) return false;

    // "YYYY-MM-DD HH:MM:SS" - 19 chars, fixed width, as written by every
    // detector's f.printf("%04d-%02d-%02d %02d:%02d:%02d", ...).
    int Y, Mo, D, H, Mi, S;
    if (!take_digits(line + 0,  4, &Y))  return false;
    if (line[4]  != '-') return false;
    if (!take_digits(line + 5,  2, &Mo)) return false;
    if (line[7]  != '-') return false;
    if (!take_digits(line + 8,  2, &D))  return false;
    if (line[10] != ' ') return false;
    if (!take_digits(line + 11, 2, &H))  return false;
    if (line[13] != ':') return false;
    if (!take_digits(line + 14, 2, &Mi)) return false;
    if (line[16] != ':') return false;
    if (!take_digits(line + 17, 2, &S))  return false;

    if (Mo < 1 || Mo > 12 || D < 1 || D > 31) return false;
    if (H > 23 || Mi > 59 || S > 60) return false;   // 60 tolerates a leap second

    *out_sec = days_from_civil(Y, (unsigned)Mo, (unsigned)D) * kSecondsPerDay
             + (long)H * 3600L + (long)Mi * 60L + (long)S;
    return true;
}

bool is_expired(long stamp_sec, long now_sec, long max_age_sec)
{
    if (max_age_sec <= 0) return false;          // retention disabled
    long age = now_sec - stamp_sec;
    if (age < 0) return false;                   // future stamp: never delete
    return age > max_age_sec;
}

int first_kept(int n, int first_unexpired, int max_entries)
{
    if (n <= 0) return 0;

    if (first_unexpired < 0) first_unexpired = 0;
    if (first_unexpired > n) first_unexpired = n;

    int keep_from = first_unexpired;

    if (max_entries > 0 && (n - keep_from) > max_entries)
        keep_from = n - max_entries;

    return keep_from;
}

}  // namespace detlog
