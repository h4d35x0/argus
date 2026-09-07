#pragma once
#include <stdint.h>

// Multi-consumer wrapper around the ESP32 WiFi promiscuous API.
//
// The ESP32 only supports one promiscuous callback at a time — the last call
// to esp_wifi_set_promiscuous_rx_cb() wins and silently discards the previous
// one.  This module owns that single slot, parses beacon frames, and fans out
// the result to all registered consumers, so the wardriver, evil-twin
// detector, flock detector, and any future features can share the same scan
// without trampling each other.
//
// WiFi is put in STA+promiscuous mode on the first add() call and torn down
// on the last remove() (reference-counted).  Channel hopping (200 ms, 1-13)
// is managed internally.

struct WifiBeacon {
    uint8_t bssid[6];
    char    ssid[33];
    char    auth[48];   // "[WPA2-PSK-CCMP][ESS]" style string
    int8_t  rssi;
    uint8_t channel;
};

typedef void (*wifi_beacon_cb_t)(const WifiBeacon *b);

// Register a consumer.  Idempotent — adding the same cb twice is a no-op.
// Returns false if WiFi init fails or the consumer table is full.
bool wifi_beacon_add(wifi_beacon_cb_t cb);

// Unregister a consumer.  WiFi is torn down when the last consumer leaves.
void wifi_beacon_remove(wifi_beacon_cb_t cb);

// True if at least one consumer is registered.
bool wifi_beacon_active();

// How many consumers are currently registered. Lets a feature decide whether
// ANOTHER scan is already running (e.g. the threat pipeline piggybacks only when
// count minus its own consumer is > 0, so it never powers WiFi on its own).
int wifi_beacon_consumer_count();

// Also receive 802.11 DATA frames (routed to handshake_rx_data) for handshake /
// PMKID capture. Off by default — the survey path is untouched unless enabled.
void wifi_beacon_set_data_capture(bool on);

// --- Channel pinning ---------------------------------------------------------
// Park the shared scan on ONE channel instead of hopping 1-13 every 200 ms.
//
// WHY: a full hop sweep is 13 x 200 ms = 2.6 s, so the scan sits on any given
// channel about 7.7% of the time. That is fine for surveying (an AP beacons
// every ~100 ms, so you WILL see it within a sweep) but it is useless for
// catching a WPA 4-way handshake, which completes in tens of milliseconds and
// is gone. Handshake capture has to be parked on the target's channel.
//
// SHARED-RESOURCE WARNING: this manager fans out to EVERY consumer, so pinning
// parks all of them. While a pin is held the Evil Twin / BeaconFlood detectors
// and the deauth survey only see the pinned channel. There is no useful
// "pin only if I am the sole consumer" guard, because detect_pipeline
// piggybacks whenever any consumer is up, so the count is never 1. Pin only
// from an explicit, user-initiated capture, and unpin as soon as it ends.
//
// ch must be 1-13; anything else is ignored and the scan keeps hopping.
// Pinning is idempotent and may be called before WiFi is up (start_wifi()
// honours a pending pin instead of starting the hop timer).
void    wifi_beacon_pin_channel(uint8_t ch);
void    wifi_beacon_unpin();
uint8_t wifi_beacon_pinned_channel();   // 0 = hopping

// --- Raw management-frame fanout (deauth / disassoc detection) ----------------
// The beacon fanout above only forwards beacons; a deauth-flood DETECTOR needs
// the raw deauth (mgmt subtype 0xC) / disassoc (0xA) frames the promiscuous mask
// already delivers. This is a SEPARATE, additive fanout: with no mgmt consumer
// registered it is a no-op and the beacon path is completely unchanged.
struct WifiMgmtFrame {
    uint8_t bssid[6];       // addr3 of the MAC header (the transmitter / AP)
    uint8_t src[6];         // addr2 (the transmitting station - e.g. a probing device)
    uint8_t subtype;        // FC subtype nibble: 0x4 probe-req, 0xC deauth, 0xA disassoc...
    int8_t  rssi;
    uint8_t channel;
    const uint8_t *frame;   // raw 802.11 frame (valid only during the callback)
    int     len;            // frame length, so a consumer can parse tagged params (SSID)
};
typedef void (*wifi_mgmt_cb_t)(const WifiMgmtFrame *m);

// Register/unregister a RAW management-frame consumer. PIGGYBACK-ONLY: never
// powers WiFi on - wifi_mgmt_add() refuses (returns false) unless a beacon scan
// is already running, so a mgmt consumer can only ride an existing scan and the
// beacon consumers keep owning the WiFi lifecycle. Idempotent.
bool wifi_mgmt_add(wifi_mgmt_cb_t cb);
void wifi_mgmt_remove(wifi_mgmt_cb_t cb);
int  wifi_mgmt_consumer_count();
