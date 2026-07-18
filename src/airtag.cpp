#include "airtag.h"
#include "threat_radar.h"

void clock_screen_get_local_time(struct tm *out);
#include "ble_scan_manager.h"
#include "gps_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "freertos/queue.h"

// Cap the captured advertising payload at 31 bytes — the BLE advertising
// PDU payload max. Anything bigger isn't a valid advert.
#define AIRTAG_APPLE_DATA_MAX 31

// Detection enqueued by the BT callback; drained by airtag_bg_tick() on the
// main task so the SD I/O doesn't run inside the BT controller's interrupt
// context.
struct AirtagHit {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t addr_type;
    uint8_t apple_data[AIRTAG_APPLE_DATA_MAX];
    uint8_t apple_data_len;
};

static volatile bool s_running = false;
static int           s_count   = 0;
static QueueHandle_t s_queue   = nullptr;

// Dedup table. A single AirTag advertises every ~2 s — without this we'd be
// writing the same MAC to the SD many times a minute. Touched only from the
// BT task in ble_gap_cb(), so no locking needed.
#define AIRTAG_SEEN_SIZE 32
static struct { uint8_t mac[6]; uint32_t last_ms; } s_seen[AIRTAG_SEEN_SIZE];
static int s_seen_count = 0;
static const uint32_t AIRTAG_RELOG_MS = 300000;   // re-log after 5 min

// Returns true if the MAC has been logged within AIRTAG_RELOG_MS and we should
// skip it. Otherwise records / updates this MAC and returns false.
static bool seen_recently_or_mark(const uint8_t *mac)
{
    uint32_t now = millis();
    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen[i].mac, mac, 6) == 0) {
            if (now - s_seen[i].last_ms < AIRTAG_RELOG_MS)
                return true;
            s_seen[i].last_ms = now;
            return false;
        }
    }
    if (s_seen_count < AIRTAG_SEEN_SIZE) {
        memcpy(s_seen[s_seen_count].mac, mac, 6);
        s_seen[s_seen_count].last_ms = now;
        s_seen_count++;
    } else {
        // Table full — evict the oldest entry
        int oldest = 0;
        for (int i = 1; i < s_seen_count; i++)
            if (s_seen[i].last_ms < s_seen[oldest].last_ms) oldest = i;
        memcpy(s_seen[oldest].mac, mac, 6);
        s_seen[oldest].last_ms = now;
    }
    return false;
}

// Core detection: scan one advertisement for Apple Find My manufacturer data.
// Shared by the standalone scanner and the wardriver — both call this from the
// BT task, so the dedup table and counters need no locking.
bool airtag_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                  const uint8_t *adv, int adv_len)
{
    // Walk AD records looking for Apple Find My manufacturer data
    for (int pos = 0; pos < adv_len; ) {
        uint8_t seg_len = adv[pos];
        if (seg_len == 0) break;
        if (pos + 1 + (int)seg_len > adv_len) break;
        uint8_t        ad_type     = adv[pos + 1];
        const uint8_t *ad_data     = adv + pos + 2;
        int            ad_data_len = (int)seg_len - 1;

        if (ad_type == 0xFF && ad_data_len >= 3) {
            uint16_t company = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8);
            uint8_t  fm_type = ad_data[2];
            // Apple company id 0x004C + sub-type 0x12 = Find My / offline finding.
            //
            // 0x12 alone is way too loose — it's broadcast by every Apple device
            // participating in the Find My network: iPhones / iPads / Macs in
            // owned mode, AirPods Pro/Max/3+, Apple Watches when separated from
            // the paired phone, Beats earbuds, third-party Find My accessories
            // (Chipolo, Pebblebee, Belkin trackers, …) AND actual AirTags. Counts
            // pile up fast in any populated area.
            //
            // The cleanest extra filter is the payload-length byte. AirTag's
            // lost-mode "separated" advert is exactly 25 bytes of payload after
            // the length field (= 22-byte EC public key + status + hint), so
            // ad_data[3] == 0x19 AND the manufacturer-data record itself is at
            // least 27 bytes long. AirPods / iPhones in owned mode use a
            // shorter Find My advert (length byte is much smaller), and
            // pairing / nearby-action broadcasts use a different fm_type
            // entirely. This still catches non-Apple Find My accessories
            // (Chipolo Spot etc.) — those are "AirTag-shaped trackers" too,
            // which is closer to what the screen is actually trying to surface.
            // Lost-mode AirTag payload structure (from openhaystack RE):
            //   ad_data[0..1] company id (0x004C)
            //   ad_data[2]    fm_type   (0x12)
            //   ad_data[3]    length-of-following-payload (0x19 = 25 for AirTag)
            //   ad_data[4]    status byte (battery state etc)
            //   ad_data[5..26] 22 bytes of EC P-224 public key (bytes 1..22)
            //   ad_data[27]   public-key byte 0 (bits 6-7)
            //   ad_data[28]   hint byte
            // -> total ad_data_len = 4 header bytes + 25 payload bytes = 29.
            constexpr uint8_t kAirTagLostLen   = 0x19;
            constexpr int     kMinFindMyRecord = 29;
            if (company == 0x004C && fm_type == 0x12 &&
                ad_data_len >= kMinFindMyRecord &&
                ad_data[3] == kAirTagLostLen) {
                if (seen_recently_or_mark(mac6)) return false;

                // Lazily create the queue — the wardriver can drive detections
                // in without airtag_start() ever having run.
                if (!s_queue)
                    s_queue = xQueueCreate(8, sizeof(AirtagHit));

                AirtagHit hit = {};
                memcpy(hit.mac, mac6, 6);
                hit.rssi      = rssi;
                hit.addr_type = addr_type;
                hit.apple_data_len = ad_data_len > AIRTAG_APPLE_DATA_MAX
                                       ? AIRTAG_APPLE_DATA_MAX
                                       : (uint8_t)ad_data_len;
                memcpy(hit.apple_data, ad_data, hit.apple_data_len);

                if (s_queue) xQueueSend(s_queue, &hit, 0);
                threatradar_observe(mac6, rssi, TR_CAT_AIRTAG);
                s_count++;
                return true;
            }
        }
        pos += 1 + (int)seg_len;
    }
    return false;
}

// BLE scan-result consumer for the standalone AirTag tile. The shared manager
// already filters to inquiry responses and handles the controller lifecycle.
static void on_scan_result(esp_ble_gap_cb_param_t *param)
{
    if (!s_running) return;
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    airtag_check(res.bda, (int8_t)res.rssi, res.ble_addr_type, res.ble_adv, total);
}

bool airtag_start()
{
    if (s_running) return true;

    if (!s_queue) {
        s_queue = xQueueCreate(8, sizeof(AirtagHit));
        if (!s_queue) return false;
    }

    // Hand BT lifecycle off to the shared manager — coexists with wardriver.
    if (!ble_scan_add(on_scan_result)) return false;

    s_running    = true;
    s_seen_count = 0;
    return true;
}

void airtag_stop()
{
    if (!s_running) return;
    s_running = false;
    ble_scan_remove(on_scan_result);
}

bool airtag_is_running() { return s_running; }
int  airtag_get_count()  { return s_count;   }

void airtag_reset_count()
{
    s_count      = 0;
    s_seen_count = 0;
}

void airtag_bg_tick()
{
    if (!s_queue) return;

    AirtagHit hit;
    if (xQueueReceive(s_queue, &hit, 0) != pdTRUE) return;

    if (!instance.isCardReady()) return;
    if (!SD.exists("/AirTag")) SD.mkdir("/AirTag");

    File f = SD.open("/AirTag/discovered.txt", FILE_APPEND);
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

    // Headline fields decoded from the Apple manufacturer payload:
    //   apple_data[0..1] = company id (0x004C)
    //   apple_data[2]    = Find My sub-type (0x12 for offline finding)
    //   apple_data[3]    = payload length
    //   apple_data[4..]  = key material + status byte
    if (hit.apple_data_len >= 3) f.printf("\tFM_type 0x%02X", hit.apple_data[2]);
    if (hit.apple_data_len >= 4) f.printf("\tFM_len %u", (unsigned)hit.apple_data[3]);

    f.print("\tdata ");
    for (int i = 0; i < hit.apple_data_len; i++) f.printf("%02X", hit.apple_data[i]);

    if (gps_screen_has_lock() && instance.gps.location.isValid()) {
        f.printf("\tGPS %.6f,%.6f",
            instance.gps.location.lat(), instance.gps.location.lng());
        if (instance.gps.altitude.isValid())
            f.printf("\tAlt %.1fm", instance.gps.altitude.meters());
    }

    f.print("\n");
    f.close();
}
