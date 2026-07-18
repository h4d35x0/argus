#include "evil_twin.h"
#include "threat_radar.h"

void clock_screen_get_local_time(struct tm *out);
#include "wifi_beacon_manager.h"
#include "usb_sd.h"
#include "gps_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include "freertos/queue.h"

// Sized for a typical noisy urban survey — most environments have well under
// 64 unique SSIDs in earshot at once. Older entries get evicted by least-
// recently-seen when the table fills, which keeps cellular hotspots and
// other one-off names from crowding out the recurring SSIDs we actually
// want to watch for impostors against.
#define ET_SSID_TABLE_SIZE  64
// Per-conflict dedup so the same impostor BSSID under the same SSID isn't
// re-logged on every beacon (~10/sec from an active rogue).
#define ET_PAIR_TABLE_SIZE  32
#define ET_RELOG_MS         (5 * 60 * 1000u)   // 5 minutes

// First-seen entry for an SSID. The auth string here is the one we accepted
// as "legitimate" because we saw it first; any later BSSID broadcasting the
// same SSID with a different auth is the suspect.
struct SsidEntry {
    char    ssid[33];
    uint8_t bssid[6];
    char    auth[48];
    uint32_t last_seen_ms;
    bool    valid;
};

// One per (ssid, impostor-bssid) so we re-fire the log only every ET_RELOG_MS.
struct PairEntry {
    char    ssid[33];
    uint8_t bssid[6];
    uint32_t last_log_ms;
    bool    valid;
};

struct EvilTwinHit {
    char    ssid[33];
    uint8_t bssid_legit[6];     // first BSSID we saw on this SSID
    uint8_t bssid_rogue[6];     // the conflicting BSSID that triggered the flag
    char    auth_legit[48];
    char    auth_rogue[48];
    int8_t  rssi;
    uint8_t channel;
};

static SsidEntry    s_ssid_table[ET_SSID_TABLE_SIZE] = {};
static PairEntry    s_pair_table[ET_PAIR_TABLE_SIZE] = {};
static int          s_count = 0;
static QueueHandle_t s_queue = nullptr;

// Returns true if the (ssid, bssid) pair has been logged within ET_RELOG_MS;
// otherwise records it and returns false. The caller treats true as "skip
// this hit, the user already knows about it".
static bool pair_seen_recently_or_mark(const char *ssid, const uint8_t *bssid)
{
    uint32_t now = millis();
    int free_slot   = -1;
    int oldest_slot = 0;
    for (int i = 0; i < ET_PAIR_TABLE_SIZE; i++) {
        if (!s_pair_table[i].valid) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strncmp(s_pair_table[i].ssid, ssid, 32) == 0 &&
            memcmp(s_pair_table[i].bssid, bssid, 6) == 0) {
            if (now - s_pair_table[i].last_log_ms < ET_RELOG_MS) return true;
            s_pair_table[i].last_log_ms = now;
            return false;
        }
        if (s_pair_table[i].last_log_ms < s_pair_table[oldest_slot].last_log_ms)
            oldest_slot = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest_slot;
    strncpy(s_pair_table[slot].ssid, ssid, 32);
    s_pair_table[slot].ssid[32] = '\0';
    memcpy(s_pair_table[slot].bssid, bssid, 6);
    s_pair_table[slot].last_log_ms = now;
    s_pair_table[slot].valid = true;
    return false;
}

// Looks up the SSID's first-seen record, creating one if absent. Returns the
// slot index; never fails because the table evicts the LRU entry when full.
static int ssid_table_get_or_create(const char *ssid, const uint8_t *bssid,
                                    const char *auth)
{
    uint32_t now = millis();
    int free_slot   = -1;
    int oldest_slot = 0;
    for (int i = 0; i < ET_SSID_TABLE_SIZE; i++) {
        if (!s_ssid_table[i].valid) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strncmp(s_ssid_table[i].ssid, ssid, 32) == 0)
            return i;
        if (s_ssid_table[i].last_seen_ms < s_ssid_table[oldest_slot].last_seen_ms)
            oldest_slot = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest_slot;
    strncpy(s_ssid_table[slot].ssid, ssid, 32);
    s_ssid_table[slot].ssid[32] = '\0';
    memcpy(s_ssid_table[slot].bssid, bssid, 6);
    strncpy(s_ssid_table[slot].auth, auth, 47);
    s_ssid_table[slot].auth[47] = '\0';
    s_ssid_table[slot].last_seen_ms = now;
    s_ssid_table[slot].valid = true;
    return slot;
}

bool evil_twin_check(const uint8_t *bssid, const char *ssid, const char *auth,
                     int8_t rssi, uint8_t channel)
{
    // Hidden / empty SSID can't be the target of a same-name impostor.
    if (!ssid || ssid[0] == '\0') return false;
    if (!bssid || !auth)         return false;

    int slot = ssid_table_get_or_create(ssid, bssid, auth);
    SsidEntry &e = s_ssid_table[slot];
    e.last_seen_ms = millis();

    // Same BSSID — that's just the same AP we already recorded.
    if (memcmp(e.bssid, bssid, 6) == 0) return false;

    // Same SSID + same auth across BSSIDs = enterprise/mesh deployment, not
    // an attack. Only flag when the auth mode actually differs.
    if (strncmp(e.auth, auth, 47) == 0) return false;

    // Conflict! Same SSID, different BSSID, different auth.
    if (pair_seen_recently_or_mark(ssid, bssid)) return false;

    if (!s_queue) s_queue = xQueueCreate(8, sizeof(EvilTwinHit));

    EvilTwinHit hit = {};
    strncpy(hit.ssid, ssid, 32);
    hit.ssid[32] = '\0';
    memcpy(hit.bssid_legit, e.bssid, 6);
    memcpy(hit.bssid_rogue, bssid,    6);
    strncpy(hit.auth_legit, e.auth, 47);
    hit.auth_legit[47] = '\0';
    strncpy(hit.auth_rogue, auth, 47);
    hit.auth_rogue[47] = '\0';
    hit.rssi    = rssi;
    hit.channel = channel;

    if (s_queue) xQueueSend(s_queue, &hit, 0);
    threatradar_observe(bssid, rssi, TR_CAT_EVILTWIN);
    s_count++;
    return true;
}

static bool s_et_running = false;

static void et_beacon_cb(const WifiBeacon *b)
{
    evil_twin_check(b->bssid, b->ssid, b->auth, b->rssi, b->channel);
}

bool evil_twin_start()
{
    if (s_et_running) return true;
    if (!wifi_beacon_add(et_beacon_cb)) return false;
    s_et_running = true;
    return true;
}

void evil_twin_stop()
{
    if (!s_et_running) return;
    s_et_running = false;
    wifi_beacon_remove(et_beacon_cb);
}

bool evil_twin_is_running() { return s_et_running; }

int  evil_twin_get_count() { return s_count; }

void evil_twin_reset_count()
{
    s_count = 0;
    memset(s_ssid_table, 0, sizeof(s_ssid_table));
    memset(s_pair_table, 0, sizeof(s_pair_table));
    if (s_queue) {
        EvilTwinHit dummy;
        while (xQueueReceive(s_queue, &dummy, 0) == pdTRUE) {}
    }
}

void evil_twin_bg_tick()
{
    if (!s_queue) return;

    EvilTwinHit hit;
    if (xQueueReceive(s_queue, &hit, 0) != pdTRUE) return;

    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/EvilTwin")) SD.mkdir("/EvilTwin");

    File f = SD.open("/EvilTwin/discovered.txt", FILE_APPEND);
    if (!f) return;

    struct tm t;
    clock_screen_get_local_time(&t);

    f.printf("%04d-%02d-%02d %02d:%02d:%02d",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    f.print("\tSource WiFi");
    f.printf("\tSSID \"%s\"", hit.ssid);
    f.printf("\tLEGIT %02X:%02X:%02X:%02X:%02X:%02X %s",
        hit.bssid_legit[0], hit.bssid_legit[1], hit.bssid_legit[2],
        hit.bssid_legit[3], hit.bssid_legit[4], hit.bssid_legit[5],
        hit.auth_legit);
    f.printf("\tROGUE %02X:%02X:%02X:%02X:%02X:%02X %s",
        hit.bssid_rogue[0], hit.bssid_rogue[1], hit.bssid_rogue[2],
        hit.bssid_rogue[3], hit.bssid_rogue[4], hit.bssid_rogue[5],
        hit.auth_rogue);
    f.printf("\tRSSI %d\tCH %u", hit.rssi, (unsigned)hit.channel);

    if (gps_screen_has_lock() && instance.gps.location.isValid()) {
        f.printf("\tGPS %.6f,%.6f",
            instance.gps.location.lat(), instance.gps.location.lng());
        if (instance.gps.altitude.isValid())
            f.printf("\tAlt %.1fm", instance.gps.altitude.meters());
    }

    f.print("\n");
    f.close();
}
