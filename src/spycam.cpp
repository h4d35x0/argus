#include "spycam.h"
#include "detect/surveillance_device.h"
#include <Arduino.h>
#include <string.h>

#define SPYCAM_MAX 16
static SpycamHit    s_hits[SPYCAM_MAX];
static int          s_n  = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void spycam_note(const uint8_t bssid[6], const char *ssid,
                 uint8_t cls, uint8_t conf, int8_t rssi)
{
    if (cls == (uint8_t)detect::DeviceClass::None) return;
    uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    int idx = -1;
    for (int i = 0; i < s_n; i++)
        if (memcmp(s_hits[i].bssid, bssid, 6) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_n < SPYCAM_MAX) {
            idx = s_n++;
        } else {                       // full: evict the stalest entry
            idx = 0;
            for (int i = 1; i < s_n; i++)
                if (s_hits[i].last_ms < s_hits[idx].last_ms) idx = i;
        }
        memcpy(s_hits[idx].bssid, bssid, 6);
    }
    strncpy(s_hits[idx].ssid, ssid ? ssid : "", sizeof(s_hits[idx].ssid) - 1);
    s_hits[idx].ssid[sizeof(s_hits[idx].ssid) - 1] = '\0';
    s_hits[idx].cls     = cls;
    s_hits[idx].conf    = conf;
    s_hits[idx].rssi    = rssi;
    s_hits[idx].last_ms = now;
    portEXIT_CRITICAL(&s_mux);
}

int spycam_get(SpycamHit *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) out[i] = s_hits[i];
    portEXIT_CRITICAL(&s_mux);
    return n;
}

int spycam_count() { return s_n; }

const char *spycam_class_name(uint8_t cls)
{
    switch ((detect::DeviceClass)cls) {
    case detect::DeviceClass::CameraGlasses: return "Camera glasses";
    case detect::DeviceClass::BodyCamera:    return "Body camera";
    case detect::DeviceClass::HiddenCamera:  return "Hidden camera";
    case detect::DeviceClass::ActionCamera:  return "Action cam/drone";
    case detect::DeviceClass::BleTracker:    return "BLE tracker";
    default:                                 return "Device";
    }
}

const char *spycam_conf_name(uint8_t conf)
{
    switch ((detect::Confidence)conf) {
    case detect::Confidence::High:   return "HIGH";
    case detect::Confidence::Medium: return "MED";
    case detect::Confidence::Low:    return "LOW";
    default:                         return "";
    }
}
