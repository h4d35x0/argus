#include "offense_wifi.h"
#include "wifi_beacon_manager.h"   // wifi_beacon_active()
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include <stdio.h>
#include <string.h>

// True while the BLE controller is up. Same guard wifi_beacon_manager uses:
// WiFi.mode(WIFI_STA) HANGS if BLE holds the internal SRAM.
static bool ble_is_active()
{
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}

static bool s_held = false;
static const char *s_owner = nullptr;

bool offense_wifi_claim(uint8_t channel, const char *owner)
{
    // Single-owner: if some offense tool already holds WiFi, REFUSE rather than let
    // a second tool drive the same radio (two injectors on one interface collide).
    if (s_held) return false;
    if (ble_is_active())     return false;   // would hang the watch
    if (wifi_beacon_active()) return false;  // a detector scan owns WiFi (hopping)

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();                        // stay idle; we only inject
    if (channel < 1 || channel > 13) channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    s_held  = true;
    s_owner = owner;
    return true;
}

const char *offense_wifi_busy_reason()
{
    static char buf[96];
    if (s_held) {
        snprintf(buf, sizeof(buf), "%s is already running.\nStop it first, then try again.",
                 s_owner ? s_owner : "Another tool");
        return buf;
    }
    if (ble_is_active())
        return "Bluetooth is on.\nWiFi and BT can't run together -\nturn Bluetooth off, then try again.";
    if (wifi_beacon_active())
        return "A detector is scanning WiFi.\nStop it first, then try again.";
    return nullptr;   // radio is free
}

bool offense_wifi_tx(const uint8_t *frame, size_t len)
{
    if (!s_held || !frame || len == 0) return false;
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false) == ESP_OK;
}

void offense_wifi_set_channel(uint8_t channel)
{
    if (!s_held) return;
    if (channel < 1 || channel > 13) return;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void offense_wifi_release()
{
    if (!s_held) return;
    WiFi.mode(WIFI_OFF);
    s_held  = false;
    s_owner = nullptr;
}

bool offense_wifi_held() { return s_held; }
