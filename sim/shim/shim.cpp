// Host implementations for the ARGUS simulator shims. SIM ONLY.
#include "Arduino.h"
#include "SD.h"
#include "LilyGoLib.h"
#include "Preferences.h"
#include "clock_time.h"

#include <map>
#include <string>
#include <chrono>
#include <sys/stat.h>

// ---- Arduino ---------------------------------------------------------------
static std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();

uint32_t millis(void)
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - s_t0).count();
}
uint32_t micros(void)
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<microseconds>(steady_clock::now() - s_t0).count();
}
void delay(uint32_t) { /* the capture harness advances time itself */ }

// ---- SD --------------------------------------------------------------------
SimSD SD;
static std::string s_sd_root;

void sim_sd_set_root(const char *host_dir) { s_sd_root = host_dir ? host_dir : ""; }

// Arduino String::trim() semantics: strip leading and trailing whitespace in
// place. Written with named char constants rather than an escaped literal so
// no quoting layer can mangle it.
void String::trim()
{
    static const char ws[] = { ' ', '\t', '\r', '\n', '\0' };
    const size_t a = v.find_first_not_of(ws);
    if (a == std::string::npos) { v.clear(); return; }
    v = v.substr(a, v.find_last_not_of(ws) - a + 1);
}

bool SimSD::exists(const char *path)
{
    if (s_sd_root.empty() || !path) return false;   // no card: the honest branch
    std::string p = s_sd_root + path;
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// ---- LilyGoLib instance ----------------------------------------------------
SimWatch instance;

static struct tm s_now;
static bool      s_now_set = false;
static int       s_batt = 82;
static bool      s_chg = false, s_vbus = false;

void sim_set_clock(int year, int mon, int day, int hour, int min, int sec)
{
    memset(&s_now, 0, sizeof(s_now));
    s_now.tm_year = year - 1900;
    s_now.tm_mon  = mon - 1;
    s_now.tm_mday = day;
    s_now.tm_hour = hour;
    s_now.tm_min  = min;
    s_now.tm_sec  = sec;
    // Fill wday/yday with the SAME civil arithmetic the firmware uses
    // (src/clock_time.cpp), not with mktime(): mktime() reinterprets its input
    // as LOCAL time and may shift the hour, which is precisely the silent
    // one-hour error clock_time.cpp exists to prevent. timegm()/gmtime_r() are
    // not available on mingw either, so this is both the correct and the
    // portable choice.
    const long long days = clocktime::days_from_civil(year, mon, day);
    s_now.tm_wday = (int)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    s_now.tm_yday = (int)(days - clocktime::days_from_civil(year, 1, 1));
    s_now.tm_isdst = 0;
    s_now_set = true;
}

void sim_set_battery(int pct, bool charging, bool vbus)
{
    s_batt = pct; s_chg = charging; s_vbus = vbus;
}

void SimRtc::getDateTime(struct tm *out)
{
    if (!out) return;
    if (!s_now_set) sim_set_clock(2026, 9, 3, 21, 24, 0);
    *out = s_now;
}
void SimRtc::setDateTime(struct tm t) { s_now = t; s_now_set = true; }

int  SimPmu::getBatteryPercent(void) { return s_batt; }
bool SimPmu::isCharging(void)        { return s_chg; }
bool SimPmu::isVbusIn(void)          { return s_vbus; }

// ---- Preferences -----------------------------------------------------------
static std::map<std::string, uint32_t> s_prefs;

size_t   Preferences::putUChar(const char *k, uint8_t v)  { s_prefs[k] = v; return 1; }
uint8_t  Preferences::getUChar(const char *k, uint8_t d)  { auto i = s_prefs.find(k); return i == s_prefs.end() ? d : (uint8_t)i->second; }
size_t   Preferences::putBool(const char *k, bool v)      { s_prefs[k] = v ? 1 : 0; return 1; }
bool     Preferences::getBool(const char *k, bool d)      { auto i = s_prefs.find(k); return i == s_prefs.end() ? d : i->second != 0; }
size_t   Preferences::putUInt(const char *k, uint32_t v)  { s_prefs[k] = v; return 4; }
uint32_t Preferences::getUInt(const char *k, uint32_t d)  { auto i = s_prefs.find(k); return i == s_prefs.end() ? d : i->second; }
bool     Preferences::remove(const char *k)               { return s_prefs.erase(k) > 0; }
