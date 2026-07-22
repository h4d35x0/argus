#pragma once
#include <stdint.h>

// Passive spy-camera / surveillance-device store. detect_pipeline's beacon_cb
// feeds it every WiFi AP that classify_wifi() fingerprints as a camera (#9); the
// Spycam results screen reads it. Thread-safe: notes come from the WiFi task,
// reads from the LVGL task (portMUX-guarded). Deduped by BSSID.

struct SpycamHit {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  cls;        // detect::DeviceClass
    uint8_t  conf;       // detect::Confidence
    int8_t   rssi;
    uint32_t last_ms;    // millis() of the most recent sighting
};

void spycam_note(const uint8_t bssid[6], const char *ssid,
                 uint8_t cls, uint8_t conf, int8_t rssi);
int  spycam_get(SpycamHit *out, int max);   // snapshot; returns count copied
int  spycam_count();
const char *spycam_class_name(uint8_t cls);
const char *spycam_conf_name(uint8_t conf);
