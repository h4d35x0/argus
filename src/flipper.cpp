#include "flipper.h"
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

// Stock Flipper Zero firmware advertises a complete-local-name with this prefix.
#define FLIPPER_NAME_PREFIX     "Flipper "
#define FLIPPER_NAME_PREFIX_LEN 8
#define FLIPPER_NAME_MAX        33

// Custom firmwares (Momentum, Xtreme, Unleashed, …) can randomize the BLE name
// to dodge "Flipper " name matching, but still advertise the Flipper BLE
// service. This 16-bit service UUID is broadcast in the primary advert (seen on
// Momentum, name "Wankiand"); matching it catches those regardless of name.
#define FLIPPER_SERVICE_UUID    0x3082

// Detection enqueued by the BT callback; drained by flipper_bg_tick() on the
// main task so the SD I/O doesn't run inside the BT controller's interrupt
// context.
struct FlipperHit {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t addr_type;
    char    name[FLIPPER_NAME_MAX];
};

static volatile bool s_running = false;
static int           s_count   = 0;
static QueueHandle_t s_queue   = nullptr;

// Dedup table. Same shape as the AirTag scanner — Flippers advertise often
// enough that without this we'd append the same MAC to the log every few
// seconds. Touched only from the BT task in flipper_check(), no locking
// needed.
#define FLIPPER_SEEN_SIZE 32
static struct { uint8_t mac[6]; uint32_t last_ms; } s_seen[FLIPPER_SEEN_SIZE];
static int s_seen_count = 0;
static const uint32_t FLIPPER_RELOG_MS = 300000;   // re-log after 5 min

// Returns true if the MAC has been logged within FLIPPER_RELOG_MS and we
// should skip it. Otherwise records / updates the entry and returns false.
static bool seen_recently_or_mark(const uint8_t *mac)
{
    uint32_t now = millis();
    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen[i].mac, mac, 6) == 0) {
            if (now - s_seen[i].last_ms < FLIPPER_RELOG_MS)
                return true;
            s_seen[i].last_ms = now;
            return false;
        }
    }
    if (s_seen_count < FLIPPER_SEEN_SIZE) {
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

// Core detection: walk the BLE advertising AD records for either a local name
// starting with "Flipper " (stock firmware) OR the Flipper BLE service UUID
// 0x3082 (custom firmwares with a randomized name). Shared by the standalone
// scanner and the wardriver.
bool flipper_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                   const uint8_t *adv, int adv_len)
{
    bool name_hit = false;   // local name begins with "Flipper "
    bool uuid_hit = false;   // advertises the Flipper service UUID
    char name[FLIPPER_NAME_MAX] = {0};

    for (int pos = 0; pos < adv_len; ) {
        uint8_t seg_len = adv[pos];
        if (seg_len == 0) break;
        if (pos + 1 + (int)seg_len > adv_len) break;
        uint8_t        ad_type     = adv[pos + 1];
        const uint8_t *ad_data     = adv + pos + 2;
        int            ad_data_len = (int)seg_len - 1;

        // 0x08 = shortened local name, 0x09 = complete local name.
        if (ad_type == 0x08 || ad_type == 0x09) {
            if (!name[0] && ad_data_len > 0) {
                int nl = ad_data_len < (int)sizeof(name) - 1
                           ? ad_data_len : (int)sizeof(name) - 1;
                memcpy(name, ad_data, nl);
                name[nl] = '\0';
            }
            if (ad_data_len >= FLIPPER_NAME_PREFIX_LEN &&
                memcmp(ad_data, FLIPPER_NAME_PREFIX, FLIPPER_NAME_PREFIX_LEN) == 0)
                name_hit = true;
        }
        // 0x02 = incomplete / 0x03 = complete list of 16-bit service UUIDs.
        else if (ad_type == 0x02 || ad_type == 0x03) {
            for (int i = 0; i + 1 < ad_data_len; i += 2) {
                uint16_t uuid = (uint16_t)ad_data[i] | ((uint16_t)ad_data[i + 1] << 8);
                if (uuid == FLIPPER_SERVICE_UUID) uuid_hit = true;
            }
        }
        pos += 1 + (int)seg_len;
    }

    if (!name_hit && !uuid_hit) return false;
    if (seen_recently_or_mark(mac6)) return false;

    if (!s_queue) s_queue = xQueueCreate(8, sizeof(FlipperHit));

    FlipperHit hit = {};
    memcpy(hit.mac, mac6, 6);
    hit.rssi      = rssi;
    hit.addr_type = addr_type;
    // Log the advertised name if any; otherwise note it was a UUID-only match.
    if (name[0]) strncpy(hit.name, name, sizeof(hit.name) - 1);
    else         strncpy(hit.name, "(uuid 0x3082)", sizeof(hit.name) - 1);
    hit.name[sizeof(hit.name) - 1] = '\0';

    if (s_queue) xQueueSend(s_queue, &hit, 0);
    threatradar_observe(mac6, rssi, TR_CAT_FLIPPER);
    s_count++;
    return true;
}

// BLE scan-result consumer for the standalone Flipper tile. The shared scan
// manager already filters to inquiry responses and handles the BT controller
// lifecycle.
static void on_scan_result(esp_ble_gap_cb_param_t *param)
{
    if (!s_running) return;
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    flipper_check(res.bda, (int8_t)res.rssi, res.ble_addr_type, res.ble_adv, total);
}

bool flipper_start()
{
    if (s_running) return true;

    if (!s_queue) {
        s_queue = xQueueCreate(8, sizeof(FlipperHit));
        if (!s_queue) return false;
    }

    // Shared BT lifecycle — coexists with wardriver + airtag.
    if (!ble_scan_add(on_scan_result)) return false;

    s_running    = true;
    s_seen_count = 0;
    return true;
}

void flipper_stop()
{
    if (!s_running) return;
    s_running = false;
    ble_scan_remove(on_scan_result);
}

bool flipper_is_running() { return s_running; }
int  flipper_get_count()  { return s_count;   }

void flipper_reset_count()
{
    s_count      = 0;
    s_seen_count = 0;
}

void flipper_bg_tick()
{
    if (!s_queue) return;

    FlipperHit hit;
    if (xQueueReceive(s_queue, &hit, 0) != pdTRUE) return;

    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/Flipper")) SD.mkdir("/Flipper");

    File f = SD.open("/Flipper/discovered.txt", FILE_APPEND);
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
