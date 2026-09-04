#include "clock_sync.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace clocksync {

// Read exactly `n` digits into *out. Returns the position after them, or NULL
// if any character is not a digit. Deliberately strict: a partially-parsed
// stamp is a fabricated sync time, which would silence the staleness warning.
static const char *take_digits(const char *p, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return NULL;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return p + n;
}

Stamp parse(const char *line)
{
    Stamp s;
    s.utc.year = 0; s.utc.mon = 0; s.utc.day = 0;
    s.utc.hour = 0; s.utc.min = 0; s.utc.sec = 0;
    s.src   = Source::None;
    s.valid = false;
    if (!line) return s;

    const char *p = strstr(line, "synced=");
    if (!p) return s;
    p += 7;

    clocktime::DateTime d;
    d.sec = 0;
    if (!(p = take_digits(p, 4, &d.year))) return s;
    if (*p++ != '-') return s;
    if (!(p = take_digits(p, 2, &d.mon)))  return s;
    if (*p++ != '-') return s;
    if (!(p = take_digits(p, 2, &d.day)))  return s;
    if (*p++ != 'T') return s;
    if (!(p = take_digits(p, 2, &d.hour))) return s;
    if (*p++ != ':') return s;
    if (!(p = take_digits(p, 2, &d.min)))  return s;

    // Range-check rather than trust the card. A corrupt file must read as
    // "never synced", not as a date that quietly poisons the age arithmetic.
    if (d.year < 2000 || d.year > 2199) return s;
    if (d.mon  < 1 || d.mon  > 12)      return s;
    if (d.day  < 1 || d.day  > 31)      return s;
    if (d.hour < 0 || d.hour > 23)      return s;
    if (d.min  < 0 || d.min  > 59)      return s;

    Source src = Source::None;
    const char *sp = strstr(line, "src=");
    if (sp) {
        const int v = atoi(sp + 4);
        if (v >= (int)Source::None && v <= (int)Source::Manual)
            src = (Source)v;
    }
    // A stamp with no recognisable source is still a real sync time; the source
    // is context, not proof. Keep the date and report the source as unknown.
    s.utc   = d;
    s.src   = src;
    s.valid = true;
    return s;
}

int format(char *buf, size_t cap, const Stamp &s)
{
    if (!buf || cap == 0) return 0;
    if (!s.valid) { buf[0] = '\0'; return 0; }
    const int n = snprintf(buf, cap, "synced=%04d-%02d-%02dT%02d:%02d src=%d",
                           s.utc.year, s.utc.mon, s.utc.day,
                           s.utc.hour, s.utc.min, (int)s.src);
    return (n < 0) ? 0 : ((size_t)n >= cap ? (int)cap - 1 : n);
}

int age_days(const Stamp &s, const clocktime::DateTime &now_utc)
{
    if (!s.valid) return 0;
    const long long then = clocktime::days_from_civil(s.utc.year, s.utc.mon, s.utc.day);
    const long long now  = clocktime::days_from_civil(now_utc.year, now_utc.mon, now_utc.day);
    return (int)(now - then);
}

bool is_stale(const Stamp &s, const clocktime::DateTime &now_utc)
{
    if (!s.valid) return true;             // never synced
    const int age = age_days(s, now_utc);
    if (age < 0)  return true;             // stamp in the future: not vouched for
    return age > kStaleDays;
}

const char *source_name(Source s)
{
    switch (s) {
    case Source::None:   return "never";
    case Source::Gps:    return "GPS";
    case Source::Ntp:    return "NTP";
    case Source::Manual: return "manual";
    }
    return "?";
}

}   // namespace clocksync
