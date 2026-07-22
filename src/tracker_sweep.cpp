#include "tracker_sweep.h"
#include "threat_radar.h"
#include "ble_scan_manager.h"
#include "detect/surveillance_device.h"
#include <Arduino.h>            // millis()
#include <string.h>
#include "esp_gap_ble_api.h"

static volatile bool s_running = false;
static int           s_count   = 0;

// Dedup table (mirrors airtag.cpp): a tracker re-advertises every ~1-2 s; without
// this the same MAC would flood the Threat Radar queue. Touched only from the BT
// scan task, so no locking is needed.
#define TRK_SEEN_SIZE 32
static struct { uint8_t mac[6]; uint32_t last_ms; } s_seen[TRK_SEEN_SIZE];
static int s_seen_count = 0;
static const uint32_t TRK_RELOG_MS = 300000;   // one clean sighting per MAC / 5 min

static bool seen_recently_or_mark(const uint8_t *mac)
{
    uint32_t now = millis();
    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen[i].mac, mac, 6) == 0) {
            if (now - s_seen[i].last_ms < TRK_RELOG_MS) return true;
            s_seen[i].last_ms = now;
            return false;
        }
    }
    if (s_seen_count < TRK_SEEN_SIZE) {
        memcpy(s_seen[s_seen_count].mac, mac, 6);
        s_seen[s_seen_count].last_ms = now;
        s_seen_count++;
    } else {
        int oldest = 0;
        for (int i = 1; i < s_seen_count; i++)
            if (s_seen[i].last_ms < s_seen[oldest].last_ms) oldest = i;
        memcpy(s_seen[oldest].mac, mac, 6);
        s_seen[oldest].last_ms = now;
    }
    return false;
}

// Classify one advert; on a non-Apple tracker signature, funnel it into the
// Threat Radar correlation store. Apple Find My is intentionally left to the
// AirTag detector, so the two never double-count the same tag.
static bool tracker_check(const uint8_t *mac6, int8_t rssi, const uint8_t *adv, int adv_len)
{
    detect::DeviceVerdict v = detect::classify_ble(adv, (size_t)adv_len);
    if (v.cls != detect::DeviceClass::BleTracker) return false;
    if (seen_recently_or_mark(mac6)) return false;
    threatradar_observe(mac6, rssi, TR_CAT_TRACKER);
    s_count++;
    return true;
}

static void on_scan_result(esp_ble_gap_cb_param_t *param)
{
    if (!s_running) return;
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    tracker_check(res.bda, (int8_t)res.rssi, res.ble_adv, total);
}

bool tracker_sweep_start()
{
    if (s_running) return true;
    if (!ble_scan_add(on_scan_result)) return false;   // shared BT lifecycle; fails if WiFi holds the radio
    s_running    = true;
    s_seen_count = 0;
    return true;
}

void tracker_sweep_stop()
{
    if (!s_running) return;
    s_running = false;
    ble_scan_remove(on_scan_result);
}

bool tracker_sweep_is_running() { return s_running; }
int  tracker_sweep_get_count()  { return s_count;   }
