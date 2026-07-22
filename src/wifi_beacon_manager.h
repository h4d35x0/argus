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
