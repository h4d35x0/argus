#include "wifi_beacon_manager.h"
#include "radio_coexist.h"
#include "pwnagotchi_peer.h"
#include "handshake.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include <lvgl.h>
#include <string.h>

// True while the BLE controller is up. Symmetric to ble_scan_manager's
// wifi_is_active(): on this board WiFi.mode(WIFI_STA) HANGS if the BLE
// controller already holds the internal SRAM, so we must refuse to bring WiFi
// up while BLE is enabled rather than freeze the watch.
[[maybe_unused]] static bool ble_is_active()
{
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}

#define WBM_MAX_CONSUMERS 4

static wifi_beacon_cb_t s_consumers[WBM_MAX_CONSUMERS] = {};
static int              s_count     = 0;
static lv_timer_t      *s_hop_timer = nullptr;
static uint8_t          s_hop_ch    = 1;
static bool             s_data_capture = false;   // also receive DATA frames (handshake capture)

static void parse_and_dispatch(const uint8_t *frame, int len,
                                int8_t rssi, uint8_t ch)
{
    if (len < 38) return;
    if ((frame[0] & 0xFC) != 0x80) return;   // not a beacon

    // Pwnagotchi advertisement — a beacon from DE:AD:BE:EF:DE:AD carrying a JSON
    // blob in an oversized SSID. Non-standard (capability bits may be unset, the
    // SSID exceeds 32 bytes), so catch it here, ahead of the infrastructure-AP
    // gate and 32-byte SSID cap below that would otherwise drop or truncate it.
    {
        static const uint8_t PWN_MAC[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD };
        if (memcmp(frame + 16, PWN_MAC, 6) == 0) {
            char js[120] = {0};
            const uint8_t *tg = frame + 36;
            int tgl = len - 36;
            for (int pos = 0; pos + 2 <= tgl; ) {
                uint8_t id = tg[pos], tl = tg[pos + 1];
                if (pos + 2 + tl > tgl) break;
                if (id == 0) {                       // SSID element holds the JSON
                    int n = tl < (int)sizeof(js) - 1 ? tl : (int)sizeof(js) - 1;
                    memcpy(js, tg + pos + 2, n);
                    js[n] = '\0';
                    break;
                }
                pos += 2 + tl;
            }
            pwnagotchi_check(frame + 16, rssi, js);
            return;   // not a real AP — keep it out of the survey consumers
        }
    }

    uint16_t cap = frame[34] | ((uint16_t)frame[35] << 8);
    if (!(cap & 0x0001)) return;              // not an infrastructure AP

    bool has_privacy = (cap & 0x0010) != 0;
    bool has_rsn     = false;
    bool has_wpa     = false;

    WifiBeacon b = {};
    memcpy(b.bssid, frame + 16, 6);
    b.rssi    = rssi;
    b.channel = ch;

    const uint8_t *tags     = frame + 36;
    const int      tags_len = len - 36;

    for (int pos = 0; pos + 2 <= tags_len; ) {
        uint8_t id = tags[pos], tl = tags[pos + 1];
        if (pos + 2 + tl > tags_len) break;
        const uint8_t *td = tags + pos + 2;

        if (id == 0 && tl <= 32) {
            memcpy(b.ssid, td, tl);
            b.ssid[tl] = '\0';
        } else if (id == 48) {
            has_rsn = true;
        } else if (id == 221 && tl >= 4 &&
                   td[0] == 0x00 && td[1] == 0x50 && td[2] == 0xF2 && td[3] == 0x01) {
            has_wpa = true;
        }
        pos += 2 + tl;
    }

    if (has_rsn)
        snprintf(b.auth, sizeof(b.auth), "[WPA2-PSK-CCMP][ESS]");
    else if (has_wpa)
        snprintf(b.auth, sizeof(b.auth), "[WPA-PSK-CCMP+TKIP][ESS]");
    else if (has_privacy)
        snprintf(b.auth, sizeof(b.auth), "[WEP][ESS]");
    else
        snprintf(b.auth, sizeof(b.auth), "[ESS]");

    for (int i = 0; i < WBM_MAX_CONSUMERS; i++) {
        if (s_consumers[i]) s_consumers[i](&b);
    }
}

// --- Raw management-frame fanout (deauth / disassoc) ------------------------
#define WBM_MAX_MGMT 2
static wifi_mgmt_cb_t s_mgmt[WBM_MAX_MGMT] = {};
static int            s_mgmt_count = 0;

// Fan every management frame out to the raw mgmt consumers, IN ADDITION to the
// beacon parse. No-op (early return) when no mgmt consumer is registered, so the
// beacon-only path is unchanged. Cheap: a type check + one memcpy + the fanout.
static void dispatch_mgmt(const uint8_t *frame, int len, int8_t rssi, uint8_t ch)
{
    if (s_mgmt_count == 0) return;
    if (len < 24) return;                     // need the full MAC header (addr3)
    if ((frame[0] & 0x0C) != 0x00) return;    // FC type bits: keep MANAGEMENT only
    WifiMgmtFrame m;
    memcpy(m.bssid, frame + 16, 6);           // addr3 = transmitter / AP
    memcpy(m.src,   frame + 10, 6);           // addr2 = the transmitting station
    m.subtype  = (uint8_t)(frame[0] >> 4);    // 0x4 probe-req, 0xC deauth, 0x8 beacon...
    m.rssi     = rssi;
    m.channel  = ch;
    m.frame    = frame;
    m.len      = len;
    for (int i = 0; i < WBM_MAX_MGMT; i++)
        if (s_mgmt[i]) s_mgmt[i](&m);
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (type == WIFI_PKT_MGMT) {
        dispatch_mgmt(pkt->payload, (int)pkt->rx_ctrl.sig_len,
                      (int8_t)pkt->rx_ctrl.rssi, (uint8_t)pkt->rx_ctrl.channel);
        parse_and_dispatch(pkt->payload, (int)pkt->rx_ctrl.sig_len,
                           (int8_t)pkt->rx_ctrl.rssi, (uint8_t)pkt->rx_ctrl.channel);
    } else if (type == WIFI_PKT_DATA && s_data_capture) {
        handshake_rx_data(pkt->payload, (int)pkt->rx_ctrl.sig_len,
                          (int8_t)pkt->rx_ctrl.rssi, (uint8_t)pkt->rx_ctrl.channel);
    }
}

static void on_channel_hop(lv_timer_t *)
{
    s_hop_ch = (s_hop_ch % 13) + 1;
    esp_wifi_set_channel(s_hop_ch, WIFI_SECOND_CHAN_NONE);
}

static bool start_wifi()
{
    // COEXISTENCE GUARD: WiFi.mode(WIFI_STA) below HANGS if the BLE controller
    // is up. Refuse cleanly BEFORE the call so a WiFi detector (Evil Twin, Pwn,
    // Flock) can tell the user "turn Bluetooth off first" instead of freezing.
#if !ARGUS_RADIO_COEXIST
    if (ble_is_active())
        return false;   // mutual-exclusion fallback (coexistence disabled)
#endif

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        WiFi.mode(WIFI_OFF);
        return false;
    }
    wifi_promiscuous_filter_t filter = {
        .filter_mask = (uint32_t)(WIFI_PROMIS_FILTER_MASK_MGMT |
                       (s_data_capture ? WIFI_PROMIS_FILTER_MASK_DATA : 0))
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    s_hop_ch   = 1;
    esp_wifi_set_channel(s_hop_ch, WIFI_SECOND_CHAN_NONE);
    s_hop_timer = lv_timer_create(on_channel_hop, 200, nullptr);
    return true;
}

static void stop_wifi()
{
    if (s_hop_timer) { lv_timer_del(s_hop_timer); s_hop_timer = nullptr; }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.mode(WIFI_OFF);
}

bool wifi_beacon_add(wifi_beacon_cb_t cb)
{
    if (!cb) return false;
    for (int i = 0; i < WBM_MAX_CONSUMERS; i++)
        if (s_consumers[i] == cb) return true;   // idempotent

    for (int i = 0; i < WBM_MAX_CONSUMERS; i++) {
        if (!s_consumers[i]) {
            if (s_count == 0 && !start_wifi()) return false;
            s_consumers[i] = cb;
            s_count++;
            return true;
        }
    }
    return false;  // table full
}

void wifi_beacon_remove(wifi_beacon_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < WBM_MAX_CONSUMERS; i++) {
        if (s_consumers[i] == cb) {
            s_consumers[i] = nullptr;
            if (--s_count == 0) stop_wifi();
            return;
        }
    }
}

bool wifi_beacon_active() { return s_count > 0; }

int wifi_beacon_consumer_count() { return s_count; }

// Toggle reception of DATA frames (for handshake/PMKID capture). Off by default,
// so the survey path is unchanged unless a capture consumer asks for it. Updates
// the promiscuous filter live when the radio is already running.
void wifi_beacon_set_data_capture(bool on)
{
    s_data_capture = on;
    if (wifi_beacon_active()) {
        wifi_promiscuous_filter_t f = {
            .filter_mask = (uint32_t)(WIFI_PROMIS_FILTER_MASK_MGMT |
                           (on ? WIFI_PROMIS_FILTER_MASK_DATA : 0))
        };
        esp_wifi_set_promiscuous_filter(&f);
    }
}

bool wifi_mgmt_add(wifi_mgmt_cb_t cb)
{
    if (!cb) return false;
    if (!wifi_beacon_active()) return false;   // PIGGYBACK-ONLY: never bring WiFi up
    for (int i = 0; i < WBM_MAX_MGMT; i++)
        if (s_mgmt[i] == cb) return true;      // idempotent
    for (int i = 0; i < WBM_MAX_MGMT; i++) {
        if (!s_mgmt[i]) { s_mgmt[i] = cb; s_mgmt_count++; return true; }
    }
    return false;                              // table full
}

void wifi_mgmt_remove(wifi_mgmt_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < WBM_MAX_MGMT; i++)
        if (s_mgmt[i] == cb) { s_mgmt[i] = nullptr; s_mgmt_count--; return; }
}

int wifi_mgmt_consumer_count() { return s_mgmt_count; }
