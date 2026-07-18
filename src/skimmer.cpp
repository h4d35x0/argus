#include "skimmer.h"
#include "threat_radar.h"

void clock_screen_get_local_time(struct tm *out);
#include "ble_scan_manager.h"
#include "gps_screen.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "freertos/queue.h"

// Names commonly seen on the HC-series Bluetooth modules card skimmers are
// built from. Matched as a PREFIX — many clones append a version suffix
// ("HC-05-V1.5", etc.) but the leading 5 chars are stable.
static const char *HC_PREFIXES[] = { "HC-03", "HC-05", "HC-06" };
#define HC_PREFIX_COUNT  (int)(sizeof(HC_PREFIXES) / sizeof(HC_PREFIXES[0]))
#define HC_PREFIX_LEN    5
#define SKIMMER_NAME_MAX 33

// Detection enqueued by the BT callback; drained by skimmer_bg_tick() on the
// main task so the SD I/O doesn't run inside the BT controller's context.
struct SkimmerHit {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t addr_type;
    char    name[SKIMMER_NAME_MAX];
};

static volatile bool s_running = false;
static int           s_count   = 0;
static QueueHandle_t s_queue   = nullptr;

// Dedup table — same shape as the AirTag / Flipper scanners. Touched only
// from the BT task in skimmer_check(), no locking needed.
#define SKIMMER_SEEN_SIZE 32
static struct { uint8_t mac[6]; uint32_t last_ms; } s_seen[SKIMMER_SEEN_SIZE];
static int s_seen_count = 0;
static const uint32_t SKIMMER_RELOG_MS = 300000;   // re-log after 5 min

static bool seen_recently_or_mark(const uint8_t *mac)
{
    uint32_t now = millis();
    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen[i].mac, mac, 6) == 0) {
            if (now - s_seen[i].last_ms < SKIMMER_RELOG_MS) return true;
            s_seen[i].last_ms = now;
            return false;
        }
    }
    if (s_seen_count < SKIMMER_SEEN_SIZE) {
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

// Walk AD records looking for an HC-0x local name. AD type 0x09 = complete
// local name, 0x08 = shortened — accept either; the bare modules normally
// emit 0x09 with the 5-char default name.
bool skimmer_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                   const uint8_t *adv, int adv_len)
{
    for (int pos = 0; pos < adv_len; ) {
        uint8_t seg_len = adv[pos];
        if (seg_len == 0) break;
        if (pos + 1 + (int)seg_len > adv_len) break;
        uint8_t        ad_type     = adv[pos + 1];
        const uint8_t *ad_data     = adv + pos + 2;
        int            ad_data_len = (int)seg_len - 1;

        if ((ad_type == 0x08 || ad_type == 0x09) &&
            ad_data_len >= HC_PREFIX_LEN) {

            bool matched = false;
            for (int i = 0; i < HC_PREFIX_COUNT; i++) {
                if (memcmp(ad_data, HC_PREFIXES[i], HC_PREFIX_LEN) == 0) {
                    matched = true;
                    break;
                }
            }
            if (!matched) { pos += 1 + (int)seg_len; continue; }

            if (seen_recently_or_mark(mac6)) return false;

            if (!s_queue) s_queue = xQueueCreate(8, sizeof(SkimmerHit));

            SkimmerHit hit = {};
            memcpy(hit.mac, mac6, 6);
            hit.rssi      = rssi;
            hit.addr_type = addr_type;
            int name_len = ad_data_len < (int)(sizeof(hit.name) - 1)
                             ? ad_data_len : (int)(sizeof(hit.name) - 1);
            memcpy(hit.name, ad_data, name_len);
            hit.name[name_len] = '\0';

            if (s_queue) xQueueSend(s_queue, &hit, 0);
            threatradar_observe(mac6, rssi, TR_CAT_SKIMMER);
            s_count++;
            return true;
        }
        pos += 1 + (int)seg_len;
    }
    return false;
}

// BLE scan-result consumer for the standalone Skimmers tile.
static void on_scan_result(esp_ble_gap_cb_param_t *param)
{
    if (!s_running) return;
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    skimmer_check(res.bda, (int8_t)res.rssi, res.ble_addr_type, res.ble_adv, total);
}

bool skimmer_start()
{
    if (s_running) return true;

    if (!s_queue) {
        s_queue = xQueueCreate(8, sizeof(SkimmerHit));
        if (!s_queue) return false;
    }

    // Shared BT lifecycle — coexists with wardriver + airtag + flipper.
    if (!ble_scan_add(on_scan_result)) return false;

    s_running    = true;
    s_seen_count = 0;
    return true;
}

void skimmer_stop()
{
    if (!s_running) return;
    s_running = false;
    ble_scan_remove(on_scan_result);
}

bool skimmer_is_running() { return s_running; }
int  skimmer_get_count()  { return s_count;   }

void skimmer_reset_count()
{
    s_count      = 0;
    s_seen_count = 0;
}

void skimmer_bg_tick()
{
    if (!s_queue) return;

    SkimmerHit hit;
    if (xQueueReceive(s_queue, &hit, 0) != pdTRUE) return;

    if (usb_sd_is_running() || !instance.isCardReady()) return;
    // Folder name matches the user spec ("Skimmers" / "Skimmmers" was a
    // typo in the request).
    if (!SD.exists("/Skimmers")) SD.mkdir("/Skimmers");

    File f = SD.open("/Skimmers/discovered.txt", FILE_APPEND);
    if (!f) return;

    struct tm t;
    clock_screen_get_local_time(&t);

    f.printf("%04d-%02d-%02d %02d:%02d:%02d",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    f.printf("\tMAC %02X:%02X:%02X:%02X:%02X:%02X (%s)",
        hit.mac[0], hit.mac[1], hit.mac[2], hit.mac[3], hit.mac[4], hit.mac[5],
        hit.addr_type == BLE_ADDR_TYPE_RANDOM ? "RAND" : "PUB");
    f.print("\tSource BLE");
    f.printf("\tRSSI %d", hit.rssi);
    f.printf("\tName %s", hit.name);

    if (gps_screen_has_lock() && instance.gps.location.isValid()) {
        f.printf("\tGPS %.6f,%.6f",
            instance.gps.location.lat(), instance.gps.location.lng());
        if (instance.gps.altitude.isValid())
            f.printf("\tAlt %.1fm", instance.gps.altitude.meters());
    }

    f.print("\n");
    f.close();
}
