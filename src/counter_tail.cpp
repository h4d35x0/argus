#include "counter_tail.h"
#include "threat_radar.h"   // TR_CAT_VEHICLE + threatradar_observe()
#include "tracker_rep.h"    // tracker_rep_hash() — shared MAC hashing
#include <LilyGoLib.h>      // instance, isCardReady
#include <SD.h>
#include <Arduino.h>        // millis()
#include <string.h>
#include <stdio.h>

void clock_screen_get_local_time(struct tm *out);   // defined in main.cpp

static bool s_enabled = true;
void counter_tail_set_enabled(bool on) { s_enabled = on; }
bool counter_tail_enabled()            { return s_enabled; }

// ── Prefilter ──────────────────────────────────────────────────────────────
// Ambient BLE/WiFi is dense; feeding every one-off device into Threat Radar
// would flood its 48-slot store. A device must be seen a few times at usable
// signal before it is promoted — that alone discards transient passers-by, and
// Threat Radar's spatial scoring then rejects anything that doesn't actually
// travel with you (roadside fixtures sit at one spot and never reach Likely).
// Touched only from the wardriver scan callbacks, matching how the built-in
// detectors run their own tables lock-free.
#define CT_CAND       96
#define CT_PROMOTE     3        // sightings before a device enters the radar
#define CT_MIN_RSSI  (-92)      // ignore very-far devices
static const uint32_t CT_FEED_MS = 30000;   // re-feed a promoted MAC at most this often

struct Cand { bool used; uint8_t mac[6]; uint16_t hits; uint32_t last_ms; uint32_t feed_ms; };
static Cand s_cand[CT_CAND];

static Cand *cand_get(const uint8_t *mac)
{
    uint32_t now = millis();
    int free_idx = -1, oldest = 0;
    for (int i = 0; i < CT_CAND; i++) {
        if (s_cand[i].used) {
            if (memcmp(s_cand[i].mac, mac, 6) == 0) return &s_cand[i];
            if (s_cand[i].last_ms < s_cand[oldest].last_ms) oldest = i;
        } else if (free_idx < 0) free_idx = i;
    }
    int idx = (free_idx >= 0) ? free_idx : oldest;   // reuse empty, else evict stalest
    Cand &c = s_cand[idx];
    memset(&c, 0, sizeof(c));
    c.used = true; memcpy(c.mac, mac, 6); c.last_ms = now;
    return &c;
}

static void observe(const uint8_t *mac, int8_t rssi)
{
    if (!s_enabled || rssi < CT_MIN_RSSI) return;
    uint32_t now = millis();
    Cand *c = cand_get(mac);
    c->last_ms = now;
    if (c->hits < 0xFFFF) c->hits++;
    if (c->hits >= CT_PROMOTE && now - c->feed_ms >= CT_FEED_MS) {
        c->feed_ms = now;
        threatradar_observe(mac, rssi, TR_CAT_VEHICLE);
    }
}

void counter_tail_observe_ble(const uint8_t *mac6, int8_t rssi)  { observe(mac6, rssi); }
void counter_tail_observe_wifi(const uint8_t *bssid, int8_t rssi){ observe(bssid, rssi); }

// ── Familiarity (your own daily vehicles/gear) ───────────────────────────────
// Keyed by MAC hash (compact + privacy). Persisted to /CounterTail/familiar.txt
// as "HEX8 YYYYMMDD DAYS". Touched only from the main task (Threat Radar).
#define CT_FAM 128
struct Fam { bool used; uint32_t hash; uint32_t last_day; uint16_t days; };
static Fam  s_fam[CT_FAM];
static bool s_loaded = false;

static uint32_t today_int()
{
    struct tm t; clock_screen_get_local_time(&t);
    return (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u + t.tm_mday;
}

static void fam_load()
{
    s_loaded = true;
    if (!instance.isCardReady()) return;
    File f = SD.open("/CounterTail/familiar.txt", FILE_READ);
    if (!f) return;
    char line[48];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (n <= 0) continue;
        line[n] = '\0';
        unsigned long h = 0, d = 0; unsigned days = 0;
        if (sscanf(line, "%lx %lu %u", &h, &d, &days) == 3) {
            for (int i = 0; i < CT_FAM; i++) if (!s_fam[i].used) {
                s_fam[i].used = true; s_fam[i].hash = (uint32_t)h;
                s_fam[i].last_day = (uint32_t)d; s_fam[i].days = (uint16_t)days;
                break;
            }
        }
    }
    f.close();
}

static void fam_save()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/CounterTail")) SD.mkdir("/CounterTail");
    File f = SD.open("/CounterTail/familiar.txt", FILE_WRITE);   // truncate + rewrite
    if (!f) return;
    for (int i = 0; i < CT_FAM; i++)
        if (s_fam[i].used)
            f.printf("%08lX %lu %u\n", (unsigned long)s_fam[i].hash,
                     (unsigned long)s_fam[i].last_day, (unsigned)s_fam[i].days);
    f.close();
}

static Fam *fam_find(uint32_t h)
{
    for (int i = 0; i < CT_FAM; i++)
        if (s_fam[i].used && s_fam[i].hash == h) return &s_fam[i];
    return nullptr;
}

void counter_tail_mark_comover(const uint8_t *mac6)
{
    if (!s_loaded) fam_load();
    uint32_t h = tracker_rep_hash(mac6), day = today_int();
    Fam *e = fam_find(h);
    if (!e) {
        for (int i = 0; i < CT_FAM; i++) if (!s_fam[i].used) { e = &s_fam[i]; break; }
        if (!e) return;                          // table full — give up quietly
        e->used = true; e->hash = h; e->last_day = day; e->days = 1;
        fam_save();
        return;
    }
    if (e->last_day != day) {                    // a new distinct day with us
        e->last_day = day;
        if (e->days < 0xFFFF) e->days++;
        fam_save();
    }
}

bool counter_tail_is_familiar(const uint8_t *mac6)
{
    if (!s_loaded) fam_load();
    Fam *e = fam_find(tracker_rep_hash(mac6));
    return e && e->days >= 2;
}
