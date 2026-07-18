#include "threat_radar.h"

void clock_screen_get_local_time(struct tm *out);   // defined in main.cpp
#include "gps_screen.h"
#include "tracker_rep.h"
#include "counter_tail.h"
#include <LilyGoLib.h>   // provides `instance`, whose .gps is a TinyGPSPlus
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include "freertos/queue.h"

// --- tuning ---------------------------------------------------------------
//
// A sighting only becomes a NEW waypoint once it is at least TR_WP_MIN_M from
// the last counted waypoint. This filters GPS jitter and the case where you sit
// still and the same tracker keeps re-advertising from one spot. Distinct
// waypoints are the backbone of the follow-score: a fixture has 1, something
// riding along with you accrues many.
static const double  TR_WP_MIN_M     = 120.0;   // metres between waypoints
static const uint32_t TR_STALE_MS    = 15UL * 60 * 1000;  // "active" window
static const uint16_t TR_SPAN_CAP_M  = 65000;   // clamp span into uint16

#define TR_MAX_CONTACTS 48

// Detection enqueued by a *_check() on the BT/WiFi task; drained by
// threatradar_bg_tick() on the main task, where GPS is read and the store is
// mutated. Keeping ALL store writes on the main task is what lets the whole
// engine run lock-free, the same trick the individual detectors use.
struct TrSighting {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t category;
};

// One tracked device. Rather than store an array of waypoints we keep just the
// first, the last-counted, and the farthest-from-first waypoint: that captures
// the linear travel that "following" actually looks like, in ~50 bytes/contact.
// span = distance(first, far); waypoints = how many distinct ones we stepped
// through; the trio plus the count is enough to score co-movement robustly.
struct TrContact {
    bool     in_use;
    uint8_t  mac[6];
    uint8_t  category;
    uint8_t  level;
    bool     alerted;          // already buzzed for LIKELY+ (edge latch)
    bool     community;        // flagged as a stalker over the mesh (peer or us)
    bool     broadcast;        // we've already announced this tail to the mesh
    bool     familiar;         // a vehicle you co-move with daily (your own car)
    uint32_t flagged_by;       // peer node id that flagged it (0 = us / local)

    uint16_t nwp;              // distinct waypoints stepped through
    double   first_lat, first_lon;
    double   last_lat,  last_lon;   // last counted waypoint (for the >=Dmin test)
    double   far_lat,   far_lon;    // waypoint farthest from first
    double   span_m;                // distance(first, far)

    int8_t   best_rssi;
    uint32_t first_ms;
    uint32_t last_ms;
    char     first_time[6];   // "HH:MM"
};

static QueueHandle_t s_queue   = nullptr;
static TrContact     s_contacts[TR_MAX_CONTACTS];
static int           s_count   = 0;

// Pending alert edge (consumed by the screen) + a tiny non-blocking buzz
// sequencer so the alert is a distinctive triple-pulse felt without looking.
static bool      s_alert_pending = false;
static TrThreat  s_alert_snap;
static int       s_buzz_left = 0;
static uint32_t  s_buzz_last_ms = 0;

// ---------------------------------------------------------------------------

static const char *kCatNames[TR_CAT_COUNT] = {
    "AirTag", "Flipper", "Skimmer", "Flock", "Evil-Twin", "Vehicle"
};
static const char *kLvlNames[4] = { "—", "POSSIBLE", "LIKELY", "CONFIRMED" };

const char *threatradar_category_name(uint8_t c)
{ return c < TR_CAT_COUNT ? kCatNames[c] : "?"; }
const char *threatradar_level_name(uint8_t l)
{ return l < 4 ? kLvlNames[l] : "?"; }

// ---------------------------------------------------------------------------

void threatradar_observe(const uint8_t *mac6, int8_t rssi, uint8_t category)
{
    if (category >= TR_CAT_COUNT) return;
    if (!s_queue) {
        s_queue = xQueueCreate(16, sizeof(TrSighting));
        if (!s_queue) return;
    }
    TrSighting s;
    memcpy(s.mac, mac6, 6);
    s.rssi     = rssi;
    s.category = category;
    xQueueSend(s_queue, &s, 0);   // drop on overflow — next advert re-fires
}

// Re-derive a contact's follow-level from its accumulated geometry. Thresholds
// climb on all three axes together — count, distance and time — so a device has
// to genuinely ride along, not just be briefly strong.
static uint8_t score_level(const TrContact *c)
{
    uint32_t span_min = (c->last_ms - c->first_ms) / 60000UL;
    double   d        = c->span_m;
    uint16_t n        = c->nwp;

    if (n >= 4 && d >= 1500 && span_min >= 18) return TR_LVL_CONFIRMED;
    if (n >= 3 && d >=  600 && span_min >= 10) return TR_LVL_LIKELY;
    if (n >= 2 && d >=  250 && span_min >=  5) return TR_LVL_POSSIBLE;
    return TR_LVL_NONE;
}

static TrContact *find_or_alloc(const uint8_t *mac)
{
    int free_idx = -1;
    for (int i = 0; i < TR_MAX_CONTACTS; i++) {
        if (s_contacts[i].in_use) {
            if (memcmp(s_contacts[i].mac, mac, 6) == 0) return &s_contacts[i];
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) {
        TrContact *c = &s_contacts[free_idx];
        memset(c, 0, sizeof(*c));
        memcpy(c->mac, mac, 6);
        c->in_use = true;
        s_count++;
        return c;
    }
    // Store full — evict the least interesting contact (lowest level, then
    // oldest last-seen). A real tail keeps trackers; idle noise gets recycled.
    int victim = 0;
    for (int i = 1; i < TR_MAX_CONTACTS; i++) {
        const TrContact *a = &s_contacts[victim], *b = &s_contacts[i];
        if (b->level < a->level ||
            (b->level == a->level && b->last_ms < a->last_ms))
            victim = i;
    }
    TrContact *c = &s_contacts[victim];
    memset(c, 0, sizeof(*c));
    memcpy(c->mac, mac, 6);
    c->in_use = true;
    return c;
}

// Persist a confirmed tail to the SD card — the evidence trail. Called once per
// contact, the first time it crosses the LIKELY alert threshold, from the main
// task (threatradar_bg_tick), which already runs only while USB-SD is inactive.
// Mirrors the per-detector logging format (timestamp + GPS + headline fields).
static void log_tail_to_sd(const TrContact *c)
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/ThreatRadar")) SD.mkdir("/ThreatRadar");

    File f = SD.open("/ThreatRadar/discovered.txt", FILE_APPEND);
    if (!f) return;

    struct tm t;
    clock_screen_get_local_time(&t);

    f.printf("%04d-%02d-%02d %02d:%02d:%02d",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    f.printf("\t%s", threatradar_level_name(c->level));
    f.printf("\t%s", threatradar_category_name(c->category));
    f.printf("\tMAC %02X:%02X:%02X:%02X:%02X:%02X",
        c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
    f.printf("\tWaypoints %u", (unsigned)c->nwp);
    f.printf("\tSpan %.0fm", c->span_m);
    f.printf("\tDwell %lumin", (unsigned long)((c->last_ms - c->first_ms) / 60000UL));
    f.printf("\tRSSI %d", (int)c->best_rssi);
    f.printf("\tFirstSeen %s", c->first_time);
    f.printf("\tFirstGPS %.6f,%.6f", c->first_lat, c->first_lon);
    f.printf("\tFarGPS %.6f,%.6f", c->far_lat, c->far_lon);
    f.print("\n");
    f.close();
}

static void fill_threat(const TrContact *c, TrThreat *t, uint32_t now)
{
    memcpy(t->mac, c->mac, 6);
    t->category  = c->category;
    t->level     = c->level;
    t->waypoints = c->nwp;
    t->span_m    = c->span_m > TR_SPAN_CAP_M ? TR_SPAN_CAP_M : (uint16_t)c->span_m;
    t->span_min  = (uint16_t)((c->last_ms - c->first_ms) / 60000UL);
    t->best_rssi = c->best_rssi;
    t->active    = (now - c->last_ms) < TR_STALE_MS;
    t->community = c->community;
    t->familiar  = c->familiar;
    t->first_lat = (float)c->first_lat;
    t->first_lon = (float)c->first_lon;
    memcpy(t->first_time, c->first_time, sizeof(t->first_time));
}

static void fold_sighting(const TrSighting *s, bool has_fix,
                          double lat, double lon, uint32_t now)
{
    TrContact *c = find_or_alloc(s->mac);
    c->category = s->category;
    c->last_ms  = now;
    if (s->rssi > c->best_rssi || c->best_rssi == 0) c->best_rssi = s->rssi;

    if (has_fix) {
        if (c->nwp == 0) {
            c->first_lat = c->last_lat = c->far_lat = lat;
            c->first_lon = c->last_lon = c->far_lon = lon;
            c->first_ms  = now;
            c->nwp       = 1;
            struct tm tmv; clock_screen_get_local_time(&tmv);
            snprintf(c->first_time, sizeof(c->first_time), "%02d:%02d",
                     tmv.tm_hour, tmv.tm_min);
        } else {
            // distanceBetween() is a static TinyGPSPlus helper; reach it through
            // `instance.gps` so we don't depend on the library's header name.
            double step = instance.gps.distanceBetween(c->last_lat, c->last_lon,
                                                       lat, lon);
            if (step >= TR_WP_MIN_M) {
                c->nwp++;
                c->last_lat = lat;
                c->last_lon = lon;
                double dfar = instance.gps.distanceBetween(
                                  c->first_lat, c->first_lon, lat, lon);
                if (dfar > c->span_m) {
                    c->span_m  = dfar;
                    c->far_lat = lat;
                    c->far_lon = lon;
                }
            }
        }
    }

    // Distributed early warning: if a peer (or we) flagged this MAC over the
    // mesh, escalate it on sight — don't wait for local co-movement to prove it.
    if (!c->community) {
        uint8_t rcat; uint32_t rfrom;
        if (tracker_rep_is_flagged(c->mac, &rcat, &rfrom)) {
            c->community  = true;
            c->flagged_by = rfrom;
            if (c->level < TR_LVL_LIKELY) c->level = TR_LVL_LIKELY;
        }
    }

    uint8_t lvl = score_level(c);
    if (lvl > c->level) c->level = lvl;   // latch upward; recency handled at read

    // Edge: first time this contact reaches LIKELY, raise the alert + buzz.
    if (c->level >= TR_LVL_LIKELY && !c->alerted) {
        c->alerted = true;
        // A vehicle that co-moves with you on most days is your own — learn it
        // and stay silent. Only an UNFAMILIAR co-moving vehicle is a tail.
        bool suppress = false;
        if (c->category == TR_CAT_VEHICLE) {
            counter_tail_mark_comover(c->mac);
            suppress = c->familiar = counter_tail_is_familiar(c->mac);
        }
        if (!suppress) {
            log_tail_to_sd(c);      // evidence trail on the first alert
            if (!s_alert_pending) {
                fill_threat(c, &s_alert_snap, now);
                s_alert_pending = true;
            }
            s_buzz_left = 3;        // distinctive triple-pulse
        }
    }

    // Confirmed locally → announce this tail to the mesh so the whole group is
    // warned. tracker_rep dedups + gates on the LoRa radio being active. Never
    // broadcast one of your own familiar vehicles.
    if (c->level >= TR_LVL_CONFIRMED && !c->broadcast) {
        c->broadcast = true;
        if (!(c->category == TR_CAT_VEHICLE && counter_tail_is_familiar(c->mac)))
            tracker_rep_flag_local(c->mac, c->category);
    }
}

void threatradar_bg_tick()
{
    // Non-blocking buzz sequencer — one pulse every ~300 ms.
    if (s_buzz_left > 0) {
        uint32_t now = millis();
        if (now - s_buzz_last_ms >= 300) {
            instance.vibrator();
            s_buzz_last_ms = now;
            s_buzz_left--;
        }
    }

    if (!s_queue) return;

    bool   has_fix = gps_screen_has_lock() && instance.gps.location.isValid();
    double lat = 0, lon = 0;
    if (has_fix) {
        lat = instance.gps.location.lat();
        lon = instance.gps.location.lng();
    }

    // Drain a bounded batch per tick so a flood can't stall the loop.
    TrSighting s;
    for (int i = 0; i < 8; i++) {
        if (xQueueReceive(s_queue, &s, 0) != pdTRUE) break;
        fold_sighting(&s, has_fix, lat, lon, millis());
    }
}

// ---------------------------------------------------------------------------

int threatradar_threat_count()
{
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < TR_MAX_CONTACTS; i++) {
        const TrContact *c = &s_contacts[i];
        if (c->in_use && c->level >= TR_LVL_POSSIBLE &&
            (now - c->last_ms) < TR_STALE_MS)
            n++;
    }
    return n;
}

int threatradar_top_level()
{
    uint32_t now = millis();
    int top = TR_LVL_NONE;
    for (int i = 0; i < TR_MAX_CONTACTS; i++) {
        const TrContact *c = &s_contacts[i];
        if (c->in_use && (now - c->last_ms) < TR_STALE_MS && c->level > top)
            top = c->level;
    }
    return top;
}

int threatradar_get_threats(TrThreat *out, int max)
{
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < TR_MAX_CONTACTS && n < max; i++) {
        const TrContact *c = &s_contacts[i];
        if (!c->in_use || c->level < TR_LVL_POSSIBLE) continue;
        fill_threat(c, &out[n++], now);
    }
    // Sort: active first, then level desc, then span desc. n is small (<=48).
    for (int a = 0; a < n - 1; a++) {
        for (int b = a + 1; b < n; b++) {
            bool swap = false;
            if (out[b].active != out[a].active)       swap = out[b].active;
            else if (out[b].level != out[a].level)    swap = out[b].level > out[a].level;
            else if (out[b].span_m > out[a].span_m)    swap = true;
            if (swap) { TrThreat t = out[a]; out[a] = out[b]; out[b] = t; }
        }
    }
    return n;
}

bool threatradar_take_alert(TrThreat *out)
{
    if (!s_alert_pending) return false;
    if (out) *out = s_alert_snap;
    s_alert_pending = false;
    return true;
}

void threatradar_reset()
{
    memset(s_contacts, 0, sizeof(s_contacts));
    s_count = 0;
    s_alert_pending = false;
    s_buzz_left = 0;
}
