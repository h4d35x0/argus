#include "flock.h"
#include "wifi_beacon_manager.h"
#include "threat_radar.h"
#include "hexhound.h"       // feed HexHound on a confirmed detector hit

void clock_screen_get_local_time(struct tm *out);
#include "ble_scan_manager.h"
#include "esp_gap_ble_api.h"
#include "gps_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

// OUI table: packed as (b0<<16)|(b1<<8)|b2. An OUI is interface-agnostic, so
// this same table is matched against both BLE addresses and WiFi BSSIDs.
// Sections must stay in order — oui_to_vendor() relies on boundary indices.
static const uint32_t FLOCK_OUIS[] = {
    // Ring (0–10)
    0x187F88, 0x242BD6, 0x343EA4, 0x54E019, 0x5C475E,
    0x649A63, 0x90486C, 0x9C7613, 0xAC9FC3, 0xC4DBAD, 0xCC3BFB,
    // Axon (11)
    0x0025DF,
    // Flock Safety (12–42)
    0xB41E52, 0x70C94E, 0x3C9180, 0xD8F3BC, 0x803049,
    0xB83532, 0x145AFC, 0x744CA1, 0x083A88, 0x9C2F9D,
    0xC03532, 0x940853, 0xE4AAEA, 0xF46ADD, 0xF8A2D6,
    0x24B2B9, 0x00F48D, 0xD03957, 0xE8D0FC, 0xE04F43,
    0xB81EA4, 0x700894, 0x588E81, 0xEC1BBD, 0x3C71BF,
    0x5800E3, 0x9035EA, 0x5C93A2, 0x646E69, 0x4827EA, 0xA4CF12,
    // DJI (43–50)
    0x0C9AE6, 0x8C5823, 0x04A85A, 0x58B858, 0xE47A2C,
    0x60601F, 0x481CB9, 0x34D262,
    // Parrot (51–55)
    0x00121C, 0x00267E, 0x9003B7, 0x903AE6, 0xA0143D,
    // Skydio (56)
    0x381D14,
    // Meta/Ray-Ban (57–61)
    0x7C2A9E, 0xCC660A, 0xF40343, 0x5CE91E, 0x985949,
};
#define FLOCK_OUI_COUNT (int)(sizeof(FLOCK_OUIS) / sizeof(FLOCK_OUIS[0]))

static const char *oui_to_vendor(int idx)
{
    if (idx <= 10)  return "Ring";
    if (idx == 11)  return "Axon";
    if (idx <= 42)  return "Flock Safety";
    if (idx <= 50)  return "DJI";
    if (idx <= 55)  return "Parrot";
    if (idx == 56)  return "Skydio";
    return "Meta/Ray-Ban";
}

// Device-name patterns. Matched case-sensitively as a PREFIX of the broadcast
// name / SSID — surveillance gear and drones lead their advertised name with
// the brand, so a prefix match keeps false positives low.
struct NamePattern { const char *prefix; const char *vendor; };

static const NamePattern FLOCK_NAMES[] = {
    {"Ring",      "Ring"},
    {"Axon",      "Axon"},
    {"Flock",     "Flock Safety"},
    {"Falcon",    "Flock Safety"},
    {"DJI",       "DJI"},
    {"Mavic",     "DJI"},
    {"Phantom",   "DJI"},
    {"Inspire",   "DJI"},
    {"Avata",     "DJI"},
    {"Tello",     "DJI"},
    {"Parrot",    "Parrot"},
    {"Anafi",     "Parrot"},
    {"Bebop",     "Parrot"},
    {"Skydio",    "Skydio"},
    {"Ray-Ban",   "Meta/Ray-Ban"},
    {"Meta View", "Meta/Ray-Ban"},
};
#define FLOCK_NAME_COUNT (int)(sizeof(FLOCK_NAMES) / sizeof(FLOCK_NAMES[0]))

struct FlockHit {
    uint8_t     mac[6];
    int8_t      rssi;
    const char *vendor;   // static string literal — pointer is always valid
    const char *method;   // "OUI" or "Name" — static string literal
    char        name[33];
    char        time_str[16]; // "YYYYMMDD_HHMMSS"
    char        source;   // 'W' = WiFi beacon, 'L' = BLE
};

#define FLOCK_DEDUP_SLOTS  32
#define FLOCK_DEDUP_MS     (5 * 60 * 1000u)
#define FLOCK_QUEUE_LEN    16

struct DedupSlot {
    uint8_t  mac[6];
    uint32_t last_ms;
    bool     valid;
};

static DedupSlot     s_dedup[FLOCK_DEDUP_SLOTS] = {};
static int           s_count = 0;
static QueueHandle_t s_queue = nullptr;

static int oui_match(const uint8_t *mac)
{
    uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    for (int i = 0; i < FLOCK_OUI_COUNT; i++) {
        if (FLOCK_OUIS[i] == oui) return i;
    }
    return -1;
}

static const char *name_match(const char *name)
{
    for (int i = 0; i < FLOCK_NAME_COUNT; i++) {
        size_t plen = strlen(FLOCK_NAMES[i].prefix);
        if (strncmp(name, FLOCK_NAMES[i].prefix, plen) == 0)
            return FLOCK_NAMES[i].vendor;
    }
    return nullptr;
}

static bool dedup_check_and_add(const uint8_t *mac)
{
    uint32_t now = millis();
    int      oldest_idx = 0;
    uint32_t oldest_ms  = UINT32_MAX;

    for (int i = 0; i < FLOCK_DEDUP_SLOTS; i++) {
        if (s_dedup[i].valid && memcmp(s_dedup[i].mac, mac, 6) == 0) {
            if (now - s_dedup[i].last_ms < FLOCK_DEDUP_MS) return false;
            s_dedup[i].last_ms = now;
            return true;
        }
        if (!s_dedup[i].valid || s_dedup[i].last_ms < oldest_ms) {
            oldest_ms  = s_dedup[i].last_ms;
            oldest_idx = i;
        }
    }

    memcpy(s_dedup[oldest_idx].mac, mac, 6);
    s_dedup[oldest_idx].last_ms = now;
    s_dedup[oldest_idx].valid   = true;
    return true;
}

bool flock_check(const uint8_t *mac6, int8_t rssi, const char *name, char source)
{
    int         oui_idx   = oui_match(mac6);
    const char *nm_vendor = (name && name[0]) ? name_match(name) : nullptr;

    if (oui_idx < 0 && !nm_vendor) return false;

    // OUI is the higher-confidence signal — prefer it for the method label.
    const char *vendor = (oui_idx >= 0) ? oui_to_vendor(oui_idx) : nm_vendor;
    const char *method = (oui_idx >= 0) ? "OUI" : "Name";

    if (!dedup_check_and_add(mac6)) return false;

    s_count++;

    if (!s_queue)
        s_queue = xQueueCreate(FLOCK_QUEUE_LEN, sizeof(FlockHit));

    FlockHit hit = {};
    memcpy(hit.mac, mac6, 6);
    hit.rssi   = rssi;
    hit.vendor = vendor;
    hit.method = method;
    hit.source = source;

    if (name && name[0]) {
        int j = 0;
        for (int i = 0; name[i] && j < (int)sizeof(hit.name) - 1; i++) {
            char c = name[i];
            hit.name[j++] = (c >= 0x20 && c < 0x7F) ? c : '.';
        }
        hit.name[j] = '\0';
    }

    struct tm t;
    clock_screen_get_local_time(&t);
    snprintf(hit.time_str, sizeof(hit.time_str), "%04d%02d%02d_%02d%02d%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    xQueueSend(s_queue, &hit, 0);
    threatradar_observe(mac6, rssi, TR_CAT_FLOCK);
    hexhound_note_detector(HEX_DET_FLOCK);   // feed the pet a big meal
    return true;
}

static bool s_flock_running = false;

static void flock_beacon_cb(const WifiBeacon *b)
{
    flock_check(b->bssid, b->rssi, b->ssid, 'W');
}

static void flock_ble_cb(esp_ble_gap_cb_param_t *param)
{
    auto &res = param->scan_rst;

    char name[33] = {};
    const uint8_t *adv       = res.ble_adv;
    int            total_len = (int)res.adv_data_len + (int)res.scan_rsp_len;
    for (int pos = 0; pos < total_len; ) {
        uint8_t seg_len = adv[pos];
        if (seg_len == 0) break;
        if (pos + 1 + (int)seg_len > total_len) break;
        uint8_t ad_type    = adv[pos + 1];
        int     ad_data_len = (int)seg_len - 1;
        if ((ad_type == 0x08 || ad_type == 0x09) &&
            ad_data_len > 0 && ad_data_len <= 32) {
            if (ad_type == 0x09 || name[0] == '\0') {
                memcpy(name, adv + pos + 2, ad_data_len);
                name[ad_data_len] = '\0';
            }
        }
        pos += 1 + (int)seg_len;
    }

    flock_check(res.bda, (int8_t)res.rssi, name, 'L');
}

bool flock_start()
{
    if (s_flock_running) return true;
    bool wifi_ok = wifi_beacon_add(flock_beacon_cb);
    bool ble_ok  = ble_scan_add(flock_ble_cb);
    if (!wifi_ok && !ble_ok) return false;
    s_flock_running = true;
    return true;
}

void flock_stop()
{
    if (!s_flock_running) return;
    s_flock_running = false;
    wifi_beacon_remove(flock_beacon_cb);
    ble_scan_remove(flock_ble_cb);
}

bool flock_is_running() { return s_flock_running; }

int flock_get_count() { return s_count; }

void flock_reset_count()
{
    s_count = 0;
    memset(s_dedup, 0, sizeof(s_dedup));
}

void flock_bg_tick()
{
    if (!s_queue || !instance.isCardReady()) return;

    // One hit per tick - same pattern as the other detectors. A burst of
    // Flock-OUI matches (every dashcam-equipped car nearby) would otherwise
    // turn into N back-to-back SD.open calls on the main thread, each
    // ~10-30 ms, blocking lv_task_handler long enough to freeze touch and
    // redraws on the wardriver screen.
    FlockHit hit;
    if (xQueueReceive(s_queue, &hit, 0) != pdTRUE) return;

    SD.mkdir("/Flock");

    char path[48];
    snprintf(path, sizeof(path), "/Flock/%s.txt", hit.time_str);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return;

    f.printf("Time:   %s\n", hit.time_str);
    f.printf("MAC:    %02X:%02X:%02X:%02X:%02X:%02X\n",
             hit.mac[0], hit.mac[1], hit.mac[2],
             hit.mac[3], hit.mac[4], hit.mac[5]);
    f.printf("Source: %s\n", hit.source == 'W' ? "WiFi" : "BLE");
    f.printf("Vendor: %s\n", hit.vendor);
    f.printf("Method: %s\n", hit.method);
    if (hit.name[0])
        f.printf("Name:   %s\n", hit.name);
    f.printf("RSSI:   %d dBm\n", (int)hit.rssi);
    // Match the GPS-tagging that airtag / flipper / skimmer / evil_twin
    // already do. Sampled at drain time, not enqueue time - matches
    // the other detectors and keeps the FlockHit struct unchanged.
    // The queue typically drains within a tick, so the position is
    // effectively "where we were when we saw it".
    if (gps_screen_has_lock() && instance.gps.location.isValid()) {
        f.printf("GPS:    %.6f,%.6f\n",
            instance.gps.location.lat(), instance.gps.location.lng());
        if (instance.gps.altitude.isValid())
            f.printf("Alt:    %.1fm\n", instance.gps.altitude.meters());
    }
    f.close();
}
