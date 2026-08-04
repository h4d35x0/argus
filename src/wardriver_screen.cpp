#include "wardriver_screen.h"
#include "theme.h"

void clock_screen_get_local_time(struct tm *out);
#include <LilyGoLib.h>
#include <SD.h>
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "gps_screen.h"
#include "esp_gap_ble_api.h"
#include "ble_scan_manager.h"
#include "wifi_beacon_manager.h"
#include "flock.h"
#include "airtag.h"
#include "flipper.h"
#include "skimmer.h"
#include "evil_twin.h"
#include "counter_tail.h"

// Generic modal dialog (message + OK button), defined in main.cpp. Named for the
// low-mem warning it was first written for, but it takes an arbitrary message, so
// the wardriver reuses it to explain WHY Start is unavailable (missing GPS/SD/radio).
void low_mem_show_dialog(const char *msg);

void clock_screen_show();

// ─── Configuration ─────────────────────────────────────────────
// Per-session cap on unique devices held in the hash table. When this is
// reached, wardriver_bg_tick() rolls to a new CSV file and clears the table
// so capture continues without dropping packets.
#define WD_MAX_APS    10000
// 13-37 used 32768 buckets (~4.5 MB PSRAM for ap_table). ARGUS added a 2 MB LVGL
// image cache that 13-37 does not have, so that 4.5 MB alloc now fails on this
// build (heap_caps_calloc returns NULL -> start_wardriving() returned false and
// the Start button silently flashed STOP then reverted). 16384 buckets is ~2.36 MB
// and fits ARGUS's remaining PSRAM; load factor at WD_MAX_APS is 0.61, fine for the
// linear-probing table. This is the root-cause fix for "Wardriver won't start".
#define WD_BUCKETS    16384 // power-of-2, load factor ~0.61 at WD_MAX_APS
#define WD_QUEUE_LEN  256
#define WD_FLUSH_MS   30000 // rewrite CSV every 30 s

// ─── Structures ────────────────────────────────────────────────
struct RawAp {
    uint8_t mac[6];
    char    ssid[33];
    char    auth[48];
    char    mfgr[10];   // manufacturer ID hex string (BLE) or empty
    int8_t  rssi;
    uint8_t channel;
    char    type;       // 'W' = WiFi, 'L' = BLE
};

struct ApRecord {
    uint8_t mac[6];
    char    ssid[33];
    char    auth[48];
    char    mfgr[10];
    char    first_seen[20]; // "YYYY-MM-DD HH:MM:SS" UTC
    int8_t  rssi;
    uint8_t channel;
    double  lat;
    double  lng;
    float   alt;
    float   acc;        // WiGLE AccuracyMeters; 0 = position unknown (lat/lng 0,0)
    char    type;       // 'W' = WiFi, 'L' = BLE
    bool    valid;
};

// ─── State ─────────────────────────────────────────────────────
static ApRecord     *ap_table    = nullptr;
// Per-session counts — entries currently held in ap_table, used by the cap
// check and the per-session CSV.
static int           wifi_count  = 0;
static int           bt_count    = 0;
// Cumulative totals across rolled-over sessions — surfaced via the public
// wardriver_get_*_count() so the home-screen badge keeps climbing across
// rollovers instead of restarting from zero.
static int           total_wifi_count = 0;
static int           total_bt_count   = 0;
// Set from drain_queue() when the session table fills; wardriver_bg_tick()
// drains it on the next pass by rolling the session.
static volatile bool s_rollover_pending = false;
static QueueHandle_t ap_queue    = nullptr;
static volatile bool is_running  = false;
static bool          bt_ready    = false;
static bool          wd_wifi_en  = true;
static bool          wd_bt_en    = true;
static char          csv_path[48] = {};
static uint32_t      last_flush_ms = 0;

// ─── UI objects ────────────────────────────────────────────────
static lv_obj_t *wardriver_screen;
static lv_obj_t *s_ward_title = nullptr;   // heading; repainted per mode on show
static lv_obj_t *gps_status_icon;
static lv_obj_t *sd_status_icon;
static lv_obj_t *wifi_toggle_sw;
static lv_obj_t *bt_toggle_sw;
static lv_obj_t *start_btn;
static lv_obj_t *start_btn_label;
static lv_obj_t *device_count_label;

// ─── Hash table helpers ────────────────────────────────────────
static uint32_t mac_hash(const uint8_t *m) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; }
    return h;
}

static ApRecord *ap_find(const uint8_t *mac) {
    if (!ap_table) return nullptr;
    uint32_t idx = mac_hash(mac) & (WD_BUCKETS - 1);
    for (int i = 0; i < WD_BUCKETS; i++) {
        ApRecord *r = &ap_table[(idx + i) & (WD_BUCKETS - 1)];
        if (!r->valid) return nullptr;
        if (memcmp(r->mac, mac, 6) == 0) return r;
    }
    return nullptr;
}

static ApRecord *ap_get_or_create(const uint8_t *mac) {
    if (!ap_table || (wifi_count + bt_count) >= WD_MAX_APS) return nullptr;
    uint32_t idx = mac_hash(mac) & (WD_BUCKETS - 1);
    for (int i = 0; i < WD_BUCKETS; i++) {
        ApRecord *r = &ap_table[(idx + i) & (WD_BUCKETS - 1)];
        if (!r->valid) {
            memcpy(r->mac, mac, 6);
            r->valid = true;
            return r;  // caller increments the typed counter
        }
        if (memcmp(r->mac, mac, 6) == 0) return r;
    }
    return nullptr;
}

static int ch_to_freq(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return 2407 + ch * 5;
    if (ch == 14) return 2484;
    if (ch >= 36 && ch <= 165) return 5000 + ch * 5;
    return 0;
}

// ─── WiFi beacon consumer (dispatched by wifi_beacon_manager) ─
static void wd_beacon_cb(const WifiBeacon *b) {
    if (!is_running || !ap_queue) return;

    RawAp raw = {};
    memcpy(raw.mac,  b->bssid, 6);
    memcpy(raw.ssid, b->ssid,  sizeof(raw.ssid));
    memcpy(raw.auth, b->auth,  sizeof(raw.auth));
    raw.rssi    = b->rssi;
    raw.channel = b->channel;
    raw.type    = 'W';

    flock_check(raw.mac, raw.rssi, raw.ssid, 'W');
    evil_twin_check(raw.mac, raw.ssid, raw.auth, raw.rssi, raw.channel);
    counter_tail_observe_wifi(raw.mac, raw.rssi);
    xQueueSend(ap_queue, &raw, 0);
}

// ─── BLE scan-result consumer (BT task) ──────────────────────
// The shared scan manager pre-filters to inquiry responses and handles the
// controller lifecycle; we only need to act on each result.
static void ble_gap_cb(esp_ble_gap_cb_param_t *param) {
    if (!is_running || !ap_queue || !wd_bt_en) return;

    auto &res = param->scan_rst;
    RawAp raw = {};
    memcpy(raw.mac, res.bda, 6);
    raw.rssi    = (int8_t)res.rssi;
    raw.channel = 0;
    raw.type    = 'L';

    if (res.ble_addr_type == BLE_ADDR_TYPE_RANDOM)
        snprintf(raw.auth, sizeof(raw.auth), "[LE] [RANDOM]");
    else
        snprintf(raw.auth, sizeof(raw.auth), "[LE]");

    const uint8_t *adv       = res.ble_adv;
    int            total_len = (int)res.adv_data_len + (int)res.scan_rsp_len;
    for (int pos = 0; pos < total_len; ) {
        uint8_t seg_len = adv[pos];
        if (seg_len == 0) break;
        if (pos + 1 + (int)seg_len > total_len) break;
        uint8_t        ad_type    = adv[pos + 1];
        const uint8_t *ad_data    = adv + pos + 2;
        int            ad_data_len = (int)seg_len - 1;

        if ((ad_type == 0x08 || ad_type == 0x09) && ad_data_len > 0 && ad_data_len <= 32) {
            if (ad_type == 0x09 || raw.ssid[0] == '\0') {
                memcpy(raw.ssid, ad_data, ad_data_len);
                raw.ssid[ad_data_len] = '\0';
            }
        } else if (ad_type == 0xFF && ad_data_len >= 2 && raw.mfgr[0] == '\0') {
            uint16_t company_id = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8);
            snprintf(raw.mfgr, sizeof(raw.mfgr), "0x%04X", company_id);
        }
        pos += 1 + (int)seg_len;
    }

    flock_check(raw.mac, raw.rssi, raw.ssid, 'L');
    airtag_check(raw.mac, raw.rssi, res.ble_addr_type, res.ble_adv, total_len);
    flipper_check(raw.mac, raw.rssi, res.ble_addr_type, res.ble_adv, total_len);
    skimmer_check(raw.mac, raw.rssi, res.ble_addr_type, res.ble_adv, total_len);
    counter_tail_observe_ble(raw.mac, raw.rssi);
    xQueueSend(ap_queue, &raw, 0);
}

// ─── GPS stamp for a captured row ─────────────────────────────
// A WiGLE row claims "this AP was observed HERE". gps.location.isValid() does
// NOT support that claim: TinyGPSPlus keeps isValid() true holding the PREVIOUS
// POWER CYCLE's coordinates until fresh NMEA overwrites them (see the
// GPS_FRESH_MS note in gps_screen.cpp), so rows silently inherited a position
// the watch was nowhere near. The 2026-07-30 CSV proves it: 26 GPS lock drops
// during the session, yet not one row recorded an unknown position.
//
// gps_screen_has_stable_lock() is the honest gate - fresh NMEA, satellite
// hysteresis, and at most ~10 s of debounce staleness, which is ordinary survey
// accuracy. When it is false we write 0,0 / accuracy 0, WiGLE's convention for
// "position unknown", rather than inventing one.
struct GpsStamp {
    double lat, lng;
    float  alt, acc;
};

static GpsStamp gps_stamp_now() {
    GpsStamp g = { 0.0, 0.0, 0.0f, 0.0f };
    if (!gps_screen_has_stable_lock() || !instance.gps.location.isValid())
        return g;   // position unknown - say so

    g.lat = instance.gps.location.lat();
    g.lng = instance.gps.location.lng();
    if (instance.gps.altitude.isValid())
        g.alt = (float)instance.gps.altitude.meters();

    // AccuracyMeters from HDOP. HDOP is unitless dilution-of-precision; the
    // conventional consumer-GNSS estimate is HDOP x the receiver's nominal UERE
    // (~5 m here). Floored at 5 m so a bogus HDOP of 0 never claims a perfect
    // fix, and left at 0 only when HDOP is unavailable (0 = unknown to WiGLE).
    if (instance.gps.hdop.isValid()) {
        double h = instance.gps.hdop.hdop();
        if (h > 0.0) {
            double m = h * 5.0;
            g.acc = (float)(m < 5.0 ? 5.0 : m);
        }
    }
    return g;
}

// ─── Drain queue → ap_table (main-loop task) ──────────────────
static void drain_queue() {
    // RTC read is I2C (~1-2 ms) and was previously being done per new AP.
    // In a dense WiFi+BLE environment that turned into hundreds of I2C
    // round-trips per drain and dominated the main-thread budget. Sample
    // once per drain and reuse for every new entry in this batch - same
    // first_seen string accuracy at human scale, fraction of the cost.
    struct tm tm_now;
    bool tm_valid = false;

    RawAp raw;
    while (xQueueReceive(ap_queue, &raw, 0) == pdTRUE) {
        ApRecord *rec = ap_find(raw.mac);
        if (rec) {
            // Backfill a name we did not have at first sight. The SSID is only
            // carried by SOME of an AP's frames as far as we are concerned: a
            // beacon whose SSID element is zero-length (hidden mode) or whose
            // tag section is truncated leaves us with "", and whichever frame
            // happened to create the record used to decide the name for the
            // whole session. Later beacons from the same BSSID can carry it.
            // Only ever fill a blank - never let a later frame overwrite a
            // name we already have, which would let a malformed or spoofed
            // beacon rewrite survey data.
            if (rec->ssid[0] == '\0' && raw.ssid[0] != '\0') {
                strncpy(rec->ssid, raw.ssid, 32);
                rec->ssid[32] = '\0';
            }
            if (raw.rssi > rec->rssi) {
                rec->rssi = raw.rssi;
                // Re-stamp only when we actually know where we are; a dropout
                // must not overwrite a good position, nor keep a stale one.
                GpsStamp g = gps_stamp_now();
                if (g.acc > 0.0f || g.lat != 0.0 || g.lng != 0.0) {
                    rec->lat = g.lat;
                    rec->lng = g.lng;
                    rec->alt = g.alt;
                    rec->acc = g.acc;
                }
            }
        } else {
            rec = ap_get_or_create(raw.mac);
            if (!rec) {
                // Table is full — request a rollover and stop draining for
                // this tick. wardriver_bg_tick() flushes the current CSV,
                // clears the table, opens a new CSV, and resumes capture on
                // the next tick. Any remaining queued packets stay queued.
                s_rollover_pending = true;
                break;
            }

            strncpy(rec->ssid, raw.ssid, 32); rec->ssid[32] = '\0';
            strncpy(rec->auth, raw.auth, 47); rec->auth[47] = '\0';
            memcpy(rec->mfgr, raw.mfgr, sizeof(rec->mfgr));
            rec->rssi    = raw.rssi;
            rec->channel = raw.channel;
            rec->type    = raw.type;

            if (raw.type == 'L') bt_count++;
            else                 wifi_count++;

            if (!tm_valid) {
                clock_screen_get_local_time(&tm_now);
                tm_valid = true;
            }
            snprintf(rec->first_seen, sizeof(rec->first_seen),
                     "%04d-%02d-%02d %02d:%02d:%02d",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

            GpsStamp g = gps_stamp_now();
            rec->lat = g.lat;
            rec->lng = g.lng;
            rec->alt = g.alt;
            rec->acc = g.acc;
        }
    }
}

// ─── CSV flush (rewrites entire file from ap_table) ───────────
static void flush_to_sd() {
    if (!csv_path[0] || (wifi_count + bt_count) == 0) return;
    if (!instance.isCardReady()) return;

    File f = SD.open(csv_path, FILE_WRITE);
    if (!f) return;

    f.println("WigleWifi-1.6,appRelease=1.0,model=T-Watch-Ultra,release=1.0,"
              "device=twatch-ultra,display=,board=ESP32-S3,brand=LilyGo,"
              "star=Sol,body=3,subBody=0");
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,"
              "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
              "AccuracyMeters,RCOIs,MfgrId,Type");

    for (int i = 0; i < WD_BUCKETS; i++) {
        const ApRecord *r = &ap_table[i];
        if (!r->valid) continue;

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 r->mac[0], r->mac[1], r->mac[2],
                 r->mac[3], r->mac[4], r->mac[5]);

        char ssid_q[72];
        int qi = 0;
        ssid_q[qi++] = '"';
        for (const char *p = r->ssid; *p && qi < 68; p++) {
            if (*p == '"') ssid_q[qi++] = '"';
            ssid_q[qi++] = *p;
        }
        ssid_q[qi++] = '"';
        ssid_q[qi]   = '\0';

        // AccuracyMeters was hardcoded 0.0; it now carries the HDOP-derived
        // estimate, and stays 0.0 exactly when the position is unknown (0,0).
        if (r->type == 'L') {
            f.printf("%s,%s,%s,%s,0,,%d,%.6f,%.6f,%.1f,%.1f,,%s,BLE\n",
                     mac_str, ssid_q, r->auth, r->first_seen,
                     (int)r->rssi, r->lat, r->lng, (double)r->alt,
                     (double)r->acc, r->mfgr);
        } else {
            f.printf("%s,%s,%s,%s,%d,%d,%d,%.6f,%.6f,%.1f,%.1f,,,WIFI\n",
                     mac_str, ssid_q, r->auth, r->first_seen,
                     r->channel, ch_to_freq(r->channel), (int)r->rssi,
                     r->lat, r->lng, (double)r->alt, (double)r->acc);
        }
    }

    f.close();
}

// ─── New CSV file with WiGLE header ────────────────────────────
// Generates a fresh timestamped CSV path under /Wardrive and writes the
// header rows. Used by both start_wardriving() and the session rollover.
static void open_new_csv() {
    SD.mkdir("/Wardrive");
    struct tm t;
    clock_screen_get_local_time(&t);
    snprintf(csv_path, sizeof(csv_path),
             "/Wardrive/%04d%02d%02d_%02d%02d%02d.csv",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    File f = SD.open(csv_path, FILE_WRITE);
    if (!f) return;
    f.println("WigleWifi-1.6,appRelease=1.0,model=T-Watch-Ultra,release=1.0,"
              "device=twatch-ultra,display=,board=ESP32-S3,brand=LilyGo,"
              "star=Sol,body=3,subBody=0");
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,"
              "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
              "AccuracyMeters,RCOIs,MfgrId,Type");
    f.close();
}

// ─── Roll session ──────────────────────────────────────────────
// Called when the table hits WD_MAX_APS. Final-flushes the current session
// to its CSV, rolls the device counts into the cumulative totals, zeroes the
// table, and opens a fresh CSV file so capture keeps going without stopping.
static void roll_session() {
    // Final flush of the filled-up session — writes everything that's in the
    // table to the current CSV.
    flush_to_sd();

    // Move this session's counts into the running totals so the home-screen
    // badge keeps climbing across rollovers.
    total_wifi_count += wifi_count;
    total_bt_count   += bt_count;
    wifi_count = 0;
    bt_count   = 0;
    s_rollover_pending = false;

    // Clear the table so ap_get_or_create() accepts new entries again.
    memset(ap_table, 0, WD_BUCKETS * sizeof(ApRecord));

    // Start a new CSV for the next session.
    open_new_csv();
    last_flush_ms = millis();
}

// ─── Start / stop ──────────────────────────────────────────────
// Reason the last start_wardriving() failed, shown to the user by on_start_stop
// instead of a silent flash-then-revert. NULL when the last start succeeded.
static const char *s_wd_start_err = nullptr;

static bool start_wardriving() {
    s_wd_start_err = nullptr;
    if (!ap_table) {
        ap_table = (ApRecord *)heap_caps_calloc(
            WD_BUCKETS, sizeof(ApRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ap_table) {
            s_wd_start_err = "Out of memory.\nClose other tools, then retry.";
            return false;
        }
    }
    wifi_count = 0;
    bt_count   = 0;
    total_wifi_count = 0;
    total_bt_count   = 0;
    s_rollover_pending = false;
    memset(ap_table, 0, WD_BUCKETS * sizeof(ApRecord));
    flock_reset_count();
    airtag_reset_count();
    flipper_reset_count();
    skimmer_reset_count();
    evil_twin_reset_count();

    if (!ap_queue)
        ap_queue = xQueueCreate(WD_QUEUE_LEN, sizeof(RawAp));

    open_new_csv();

    if (wd_wifi_en) {
        if (!wifi_beacon_add(wd_beacon_cb)) {
            // WiFi init failed — clean up and bail
            if (wd_bt_en && bt_ready) { ble_scan_remove(ble_gap_cb); bt_ready = false; }
            s_wd_start_err = "WiFi could not start.\nIf Bluetooth is on, turn it\noff first (radios share memory).";
            return false;
        }
    }

    if (wd_bt_en) {
        // Hand BT lifecycle off to the shared manager so the AirTag sniffer
        // (and any future BLE consumer) can run in parallel without us
        // overwriting each other's GAP callback.
        bt_ready = ble_scan_add(ble_gap_cb);
    }

    last_flush_ms = millis();
    is_running    = true;
    return true;
}

static void stop_wardriving() {
    if (!is_running) return;
    is_running = false;

    if (wd_bt_en && bt_ready) {
        // Manager ref-counts consumers; controller stays up if AirTag (or
        // anything else) is still scanning.
        ble_scan_remove(ble_gap_cb);
        bt_ready = false;
    }

    if (wd_wifi_en) {
        wifi_beacon_remove(wd_beacon_cb);
    }

    drain_queue();
    flush_to_sd();
}

// ─── Toggle callbacks ──────────────────────────────────────────
static void on_wifi_toggle(lv_event_t *) {
    wd_wifi_en = lv_obj_has_state(wifi_toggle_sw, LV_STATE_CHECKED);
}

static void on_bt_toggle(lv_event_t *) {
    wd_bt_en = lv_obj_has_state(bt_toggle_sw, LV_STATE_CHECKED);
}

// ─── Button ────────────────────────────────────────────────────
static void on_start_stop(lv_event_t *) {
    if (!is_running) {
        // Readiness gate lives here (not on the widget) so a tap while not-ready
        // tells the user exactly what's missing instead of a silent dead press.
        // Wardriving geolocates every capture, so a GPS lock and an SD card to log
        // to are both required, plus at least one radio to scan.
        // Stable lock, matching what gps_stamp_now() will actually record. On the
        // instantaneous predicate this gate flickered with the fix, so a tap that
        // landed in a 1 s dropout was refused even though the survey was fine.
        bool gps_ok   = gps_screen_has_stable_lock();
        bool sd_ok    = instance.isCardReady();
        bool radio_ok = wd_wifi_en || wd_bt_en;
        if (!(gps_ok && sd_ok && radio_ok)) {
            // When GPS is the blocker, say WHY and for how long. A bare red X
            // here is what made a blocked sky ("in a car") indistinguishable
            // from a dead receiver on 2026-08-03, and cost three debugging
            // sessions. gps_screen_health() already knows the difference.
            char gps_detail[64] = "";
            if (!gps_ok) {
                char dur[16];
                gps_health_duration(gps_screen_health_secs(), dur, sizeof(dur));
                snprintf(gps_detail, sizeof(gps_detail), "\n    %s (%s)",
                         gps_health_hint(gps_screen_health()), dur);
            }

            char msg[256];
            snprintf(msg, sizeof(msg),
                     "WARDRIVE NEEDS:\n"
                     "%s GPS lock%s\n"
                     "%s SD card\n"
                     "%s WiFi or BT on",
                     gps_ok   ? "#00CC44 " LV_SYMBOL_OK "#" : "#FF3333 " LV_SYMBOL_CLOSE "#",
                     gps_detail,
                     sd_ok    ? "#00CC44 " LV_SYMBOL_OK "#" : "#FF3333 " LV_SYMBOL_CLOSE "#",
                     radio_ok ? "#00CC44 " LV_SYMBOL_OK "#" : "#FF3333 " LV_SYMBOL_CLOSE "#");
            low_mem_show_dialog(msg);
            return;
        }

        // wifi_beacon_add() inside start_wardriving() blocks the main loop for
        // ~250 ms (measured 2026-07-25 under BLE+WiFi+LoRa load; the old "~1 s"
        // figure here was stale). Flip the UI to STOP *before* that blocking
        // call (and force a flush) so the user sees instant feedback instead of
        // staring at a stale START.
        lv_obj_set_style_bg_color(start_btn, lv_color_make(0xCC, 0x00, 0x00), LV_PART_MAIN);
        lv_label_set_text(start_btn_label, "STOP");
        lv_obj_add_state(wifi_toggle_sw, LV_STATE_DISABLED);
        lv_obj_add_state(bt_toggle_sw,   LV_STATE_DISABLED);
        lv_refr_now(NULL);

        if (!start_wardriving()) {
            // Start failed (PSRAM alloc or WiFi bring-up) - undo the optimistic UI
            // flip AND tell the user why, instead of a silent flash-back to START.
            lv_obj_set_style_bg_color(start_btn, lv_color_make(0x00, 0xCC, 0x44), LV_PART_MAIN);
            lv_label_set_text(start_btn_label, "START");
            lv_obj_clear_state(wifi_toggle_sw, LV_STATE_DISABLED);
            lv_obj_clear_state(bt_toggle_sw,   LV_STATE_DISABLED);
            low_mem_show_dialog(s_wd_start_err ? s_wd_start_err
                                               : "Wardrive could not start.");
        }
    } else {
        // Flip the UI back to START *first* (before the teardown), exactly like the
        // start path flips to STOP first. stop_wardriving() does a final SD flush and
        // WiFi.mode(WIFI_OFF), which can block the main loop briefly; doing it before
        // the label update made the STOP tap look ignored ("doesn't stop when
        // pressed"). Now the button responds instantly, then teardown runs.
        lv_obj_clear_state(wifi_toggle_sw, LV_STATE_DISABLED);
        lv_obj_clear_state(bt_toggle_sw,   LV_STATE_DISABLED);
        bool gps_ok = gps_screen_has_stable_lock();   // match on_start_stop's gate
        bool sd_ok  = instance.isCardReady();
        bool ready  = gps_ok && sd_ok && (wd_wifi_en || wd_bt_en);
        lv_obj_set_style_bg_color(start_btn,
            ready ? lv_color_make(0x00, 0xCC, 0x44)
                  : lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
        lv_label_set_text(start_btn_label, "START");
        lv_refr_now(NULL);        // paint START before the (possibly blocking) teardown
        stop_wardriving();
    }
}

// ─── Gesture ───────────────────────────────────────────────────
static void on_gesture(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT)
        clock_screen_show();
}

// ─── UI helpers ────────────────────────────────────────────────
static void make_status_row(lv_obj_t *screen, const char *field,
                             lv_obj_t **icon_out, int32_t y)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_size(row, 380, 60);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lbl, field);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0x33, 0x33), LV_PART_MAIN);
    lv_label_set_text(icon, LV_SYMBOL_CLOSE);
    lv_obj_align(icon, LV_ALIGN_RIGHT_MID, 0, 0);

    *icon_out = icon;
}

static void make_toggle_row(lv_obj_t *screen, const char *field,
                             lv_obj_t **sw_out, int32_t y)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_size(row, 380, 55);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lbl, field);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 80, 40);
    lv_obj_set_style_bg_color(sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(sw, LV_STATE_CHECKED);  // default ON
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);

    *sw_out = sw;
}

// ─── Public API ────────────────────────────────────────────────
void wardriver_screen_create()
{
    wardriver_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wardriver_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(wardriver_screen, 0, LV_PART_MAIN);
    // Without this, swipes get interpreted as scroll attempts (children
    // fill the viewport edge-to-edge) and the gesture event is consumed
    // before on_gesture runs - the back-swipe stops working.
    lv_obj_clear_flag(wardriver_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(wardriver_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(wardriver_screen);
    s_ward_title = title;
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "WARDRIVER");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    make_status_row(wardriver_screen, "GPS",     &gps_status_icon, 88);
    make_status_row(wardriver_screen, "SD Card", &sd_status_icon,  152);

    make_toggle_row(wardriver_screen, "WiFi Scan",      &wifi_toggle_sw, 220);
    make_toggle_row(wardriver_screen, "Bluetooth Scan", &bt_toggle_sw,   278);

    lv_obj_add_event_cb(wifi_toggle_sw, on_wifi_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bt_toggle_sw,   on_bt_toggle,   LV_EVENT_VALUE_CHANGED, NULL);

    device_count_label = lv_label_create(wardriver_screen);
    lv_obj_set_style_text_color(device_count_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(device_count_label, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(device_count_label, "WiFi: 0  BT: 0");
    lv_obj_align(device_count_label, LV_ALIGN_TOP_MID, 0, 340);

    start_btn = lv_button_create(wardriver_screen);
    lv_obj_set_size(start_btn, 220, 60);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(start_btn, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_radius(start_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(start_btn, 0, LV_PART_MAIN);
    // Button is ALWAYS clickable: the readiness gate lives in the action
    // (on_start_stop), not the widget, so a tap while not-ready explains what's
    // missing instead of silently doing nothing. The grey/green fill is a visual
    // affordance only.
    lv_obj_add_event_cb(start_btn, on_start_stop, LV_EVENT_CLICKED, NULL);

    start_btn_label = lv_label_create(start_btn);
    lv_obj_set_style_text_color(start_btn_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(start_btn_label, &font_argus_label_48, LV_PART_MAIN);
    lv_label_set_text(start_btn_label, "START");
    lv_obj_center(start_btn_label);
}

void wardriver_screen_show()
{
    if (s_ward_title) lv_obj_set_style_text_color(s_ward_title, argus_base_accent(), LV_PART_MAIN);
    wardriver_screen_update();
    lv_scr_load(wardriver_screen);
}

bool wardriver_screen_is_active()
{
    return lv_screen_active() == wardriver_screen;
}

void wardriver_bg_tick()
{
    if (!is_running) return;

    // Only auto-stop on SD removal. GPS lock can come and go - especially
    // during start_wardriving()'s WiFi init, which blocks the main loop
    // long enough that TinyGPSPlus's age window expires. gps_stamp_now()
    // handles a lost fix by writing lat=lng=0 (WiGLE "unknown"), and existing
    // rows keep the fix they were captured with - so dropping GPS lock is no
    // reason to tear the whole session down.
    if (!instance.isCardReady()) {
        stop_wardriving();
        lv_obj_clear_state(wifi_toggle_sw, LV_STATE_DISABLED);
        lv_obj_clear_state(bt_toggle_sw,   LV_STATE_DISABLED);
        return;
    }

    drain_queue();

    // Session table is full → flush, archive the CSV, and start a fresh
    // session so capture continues without stopping.
    if (s_rollover_pending) {
        roll_session();
    }

    uint32_t now = millis();
    if (now - last_flush_ms >= WD_FLUSH_MS) {
        last_flush_ms = now;
        flush_to_sd();
    }
}

bool wardriver_is_running()     { return is_running;  }
// Public getters return cumulative counts across all sessions in this run so
// the home-screen badge keeps climbing past WD_MAX_APS after a rollover.
int  wardriver_get_wifi_count() { return total_wifi_count + wifi_count; }
int  wardriver_get_bt_count()   { return total_bt_count   + bt_count;   }

void wardriver_screen_update()
{
    // The status ICON reports the live truth (instantaneous predicate) so the
    // user can see the fix actually coming and going. The READY hint below uses
    // the debounced lock, so it agrees with on_start_stop's gate and with what
    // gps_stamp_now() will record.
    bool gps_ok = gps_screen_has_lock();
    lv_label_set_text(gps_status_icon, gps_ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(gps_status_icon,
        gps_ok ? lv_color_make(0x00, 0xCC, 0x44) : lv_color_make(0xFF, 0x33, 0x33),
        LV_PART_MAIN);

    bool sd_ok = instance.isCardReady();
    lv_label_set_text(sd_status_icon, sd_ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(sd_status_icon,
        sd_ok ? lv_color_make(0x00, 0xCC, 0x44) : lv_color_make(0xFF, 0x33, 0x33),
        LV_PART_MAIN);

    if (!is_running) {
        bool ready = gps_screen_has_stable_lock() && sd_ok
                     && (wd_wifi_en || wd_bt_en);
        // Grey when not ready, green when ready — a hint, not a lock. The button
        // stays tappable so the user gets a reason dialog (see on_start_stop).
        lv_obj_set_style_bg_color(start_btn,
            ready ? lv_color_make(0x00, 0xCC, 0x44)
                  : lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
        lv_label_set_text(start_btn_label, "START");
    }

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "WiFi: %d  BT: %d",
             total_wifi_count + wifi_count,
             total_bt_count + bt_count);
    lv_label_set_text(device_count_label, count_buf);
}
