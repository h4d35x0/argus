#include "timezone.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <time.h>
#include <limits.h>
#include "usb_sd.h"
#include "clock_time.h"

// Implemented in main.cpp (clock screen). The RTC holds UTC; clock_utc_offset
// shifts it to local for display.
void clock_screen_set_utc_offset(int offset_hours);
bool clock_screen_manual_time_active();
void clock_screen_refresh();

#include "clock_sync.h"

#define TZ_PATH "/Settings/timezone.txt"

// ---- persistence -----------------------------------------------------------
//
// File format (one line):  offset=<hours> v=<version> [synced=<utc> src=<n>]
//
// The synced/src keys record WHEN the RTC was last set from a trusted source
// and by what. They are optional: a file without them reads as "never synced",
// which is the honest answer for a card written by older firmware. See
// clock_sync.h for why the watch needs to be able to distrust its own clock.
//
// v1 files carry no "v=" key. They were written under the old model, in which
// Manual Time wrote LOCAL wall clock into the RTC and dropped the offset to 0
// in RAM ONLY. The 0 was never persisted, so the last GPS/WiFi-detected offset
// stayed in the file and was re-applied on the next boot to an RTC that no
// longer held UTC -- a watch set by hand in one zone after a trip to another
// came back up hours off. v2 always persists the offset that pairs with what
// is actually in the RTC, so the restored pair is self-consistent.

static int s_last_written = INT_MIN;   // avoid redundant SD writes

// Last-sync stamp, mirrored in RAM so the UI can ask without touching the card.
static clocksync::Stamp s_sync = { {0,0,0,0,0,0}, clocksync::Source::None, false };

// Unconditional write; callers dedupe. The stamp is emitted only when valid, so
// a migration rewrite cannot invent a sync that never happened.
static bool tz_write(int offset_hours, const clocksync::Stamp &stamp)
{
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(TZ_PATH, FILE_WRITE);   // FILE_WRITE = truncate
    if (!f) return false;
    char sync[48] = {0};
    clocksync::format(sync, sizeof(sync), stamp);
    f.printf("offset=%d v=%d%s%s\n", offset_hours, clocktime::kFileVersion,
             sync[0] ? " " : "", sync);
    f.close();
    s_last_written = offset_hours;
    return true;
}

// Read the saved line into *out_off / *out_ver. Returns false when there is no
// usable value on the card.
static bool tz_read(int *out_off, int *out_ver)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return false;
    if (!SD.exists(TZ_PATH)) return false;

    File f = SD.open(TZ_PATH, FILE_READ);
    if (!f) return false;
    char buf[48] = {0};
    int n = f.readBytesUntil('\n', (uint8_t *)buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0) return false;
    buf[n] = '\0';

    const char *p = strstr(buf, "offset=");
    if (!p) return false;
    int off = atoi(p + 7);
    if (!clocktime::offset_plausible(off)) return false;

    const char *v = strstr(buf, "v=");
    *out_ver = v ? atoi(v + 2) : 1;              // no "v=" key => v1
    *out_off = off;
    s_sync = clocksync::parse(buf);              // absent keys => never synced
    return true;
}

// Snapshot the RTC (UTC) into a stamp for `src`.
static clocksync::Stamp stamp_now(clocksync::Source src)
{
    struct tm t;
    instance.rtc.getDateTime(&t);
    clocksync::Stamp s;
    s.utc.year = t.tm_year + 1900; s.utc.mon = t.tm_mon + 1; s.utc.day = t.tm_mday;
    s.utc.hour = t.tm_hour;        s.utc.min = t.tm_min;     s.utc.sec = 0;
    s.src   = src;
    s.valid = true;
    return s;
}

clocksync::Stamp timezone_last_sync() { return s_sync; }

void timezone_note_synced(int offset_hours, clocksync::Source src)
{
    if (!clocktime::offset_plausible(offset_hours)) return;      // sanity

    // The RAM stamp updates even with no card, so the UI tells the truth on a
    // cardless watch; only the persistence needs the card.
    s_sync = stamp_now(src);

    if (!instance.isCardReady() || usb_sd_is_running()) return;
    // Deliberately NOT deduped on the offset. A sync that confirms the SAME
    // offset is still a sync, and skipping the write would leave the stamp
    // ageing on disk while the clock was in fact being maintained - exactly the
    // false "stale" this feature must not produce.
    tz_write(offset_hours, s_sync);
}

void timezone_note_detected(int offset_hours)
{
    if (offset_hours == s_last_written) return;                  // unchanged
    if (!clocktime::offset_plausible(offset_hours)) return;      // sanity
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    tz_write(offset_hours, s_sync);
}

int timezone_peek_saved_offset(int fallback)
{
    int off = 0, ver = 0;
    if (!tz_read(&off, &ver)) return fallback;
    return off;
}

void timezone_load_on_boot()
{
    int off = 0, ver = 0;
    if (!tz_read(&off, &ver)) {
        Serial.printf("[tz] no usable %s (card ready=%d, usb=%d)\n", TZ_PATH,
                      instance.isCardReady() ? 1 : 0, usb_sd_is_running() ? 1 : 0);
        return;
    }

    const bool manual = clock_screen_manual_time_active();
    const int  paired = clocktime::effective_saved_offset(off, ver, manual);

    // Print the card's state BEFORE the migration rewrites it - this is the only
    // moment the pre-upgrade evidence exists.
    Serial.printf("[tz] file offset=%d v=%d manual=%d -> paired=%d\n",
                  off, ver, manual ? 1 : 0, paired);

    if (ver < clocktime::kFileVersion) {
        s_last_written = INT_MIN;                // force the rewrite
        tz_write(paired, s_sync);                // best effort; card may be busy
    } else {
        s_last_written = paired;                 // already on disk; don't rewrite
    }

    clock_screen_set_utc_offset(paired);
    clock_screen_refresh();
}

// ---- WiFi NTP + IP-geolocation background sync ------------------------------
//
// NTP and HTTP both block for seconds, so the network work runs on a dedicated
// task; the results are applied back on the main loop (timezone_bg_tick) so the
// RTC / clock / SD are only touched from one context.

static volatile bool s_sync_pending = false;   // WiFi GOT_IP -> request a sync
static volatile bool s_result_ready = false;   // worker -> main: results ready
static volatile bool s_got_time     = false;
static volatile bool s_got_offset   = false;
static volatile int  s_offset_h     = 0;
static struct tm     s_utc_tm       = {};
static TaskHandle_t  s_task         = nullptr;

// Query ip-api.com for the current UTC offset (seconds, DST-aware). Free tier
// is plain HTTP. Returns true and fills *out_h on success.
static bool http_get_offset(int *out_h)
{
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin("http://ip-api.com/json/?fields=status,offset")) return false;

    bool ok = false;
    if (http.GET() == 200) {
        String body = http.getString();
        if (body.indexOf("\"status\":\"success\"") >= 0) {
            int i = body.indexOf("\"offset\":");
            if (i >= 0) {
                long secs = atol(body.c_str() + i + 9);
                *out_h = (int)(secs / 3600);
                ok = true;
            }
        }
    }
    http.end();
    return ok;
}

static void tz_worker(void *)
{
    for (;;) {
        if (!s_sync_pending) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
        s_sync_pending = false;
        if (WiFi.status() != WL_CONNECTED) continue;

        bool      got_time = false, got_off = false;
        int       off_h = 0;
        struct tm tmutc = {};

        // NTP: configTime(0,0,...) keeps system time in UTC.
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        if (getLocalTime(&tmutc, 8000)) got_time = true;

        // IP geolocation -> current UTC offset.
        if (http_get_offset(&off_h)) got_off = true;

        Serial.printf("[tz] wifi sync: ntp=%d geo=%d off=%d\n",
                      got_time ? 1 : 0, got_off ? 1 : 0, off_h);

        if (got_time) s_utc_tm = tmutc;
        s_got_time     = got_time;
        s_offset_h     = off_h;
        s_got_offset   = got_off;
        s_result_ready = true;
    }
}

static void on_wifi_got_ip(arduino_event_id_t, arduino_event_info_t)
{
    s_sync_pending = true;   // worker picks it up
}

void timezone_init()
{
    WiFi.onEvent(on_wifi_got_ip, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    if (!s_task)
        xTaskCreate(tz_worker, "tz_sync", 8192, nullptr, 3, &s_task);
}

void timezone_bg_tick()
{
    if (!s_result_ready) return;
    s_result_ready = false;

    // Manual Time wins: drop the results untouched.
    if (clock_screen_manual_time_active()) {
        s_got_time = s_got_offset = false;
        return;
    }

    if (s_got_time) {
        instance.rtc.setDateTime(s_utc_tm.tm_year + 1900, s_utc_tm.tm_mon + 1,
                                 s_utc_tm.tm_mday, s_utc_tm.tm_hour,
                                 s_utc_tm.tm_min, s_utc_tm.tm_sec);
        instance.rtc.hwClockRead();
        s_got_time = false;
    }
    if (s_got_offset) {
        clock_screen_set_utc_offset(s_offset_h);
        timezone_note_synced(s_offset_h, clocksync::Source::Ntp);
        s_got_offset = false;
    }
    clock_screen_refresh();
}
