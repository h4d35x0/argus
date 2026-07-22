#include "detect_pipeline.h"

#include <Arduino.h>        // FreeRTOS portMUX + millis (via the ESP32 core)
#include <LilyGoLib.h>      // instance.isCardReady()
#include <SD.h>
#include <string.h>

#include "wifi_beacon_manager.h"   // WifiBeacon, wifi_beacon_add()
#include "usb_sd.h"                // usb_sd_is_running()
#include "theme.h"                 // argus_set_threat() - HADES-red brand accent
#include "hexhound.h"              // hexhound_set_threat_level()

#include "detect/evil_twin.h"      // RogueApDetector, ApObservation, AuthMode
#include "detect/beacon_flood.h"   // BeaconFloodDetector, BeaconObservation
#include "detect/threat_state.h"   // ThreatState, ThreatDomain, Severity, ThreatLevel
#include "detect/threat_map.h"     // detect::feed() verdict -> aggregator
#include "detect/threat_log.h"     // ThreatLog forensic edge recorder

// --- Owned pure-detector state (single long-lived instances). --------------
// The detectors, aggregator, and log are pure and already host-tested; this
// file only composes them - it never modifies them.
static detect::RogueApDetector     s_rogue;
static detect::BeaconFloodDetector s_beacon_flood;
static detect::ThreatState         s_threat;
static detect::ThreatLog           s_threat_log;

// The beacon callback runs in the WiFi driver task and has no clock of its own,
// so the 1Hz tick stashes the latest monotonic seconds here for it to read.
static volatile uint32_t s_now_sec = 0;

// The callback (WiFi task) and the tick (main/LVGL task) both mutate s_rogue /
// s_beacon_flood / s_threat, potentially on different cores. Guard every shared
// access with a short spinlock - the codebase's established cross-task pattern
// (see the "spinlock-guarded pending accumulator" note in hexhound.h). Only
// O(1)-ish fixed-table work happens inside it; no IO, so the section stays
// microseconds-long and never blocks.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Whether our beacon consumer is currently attached. Toggled by the piggyback
// logic in detect_pipeline_tick(): attached only while another WiFi scan runs.
static bool s_registered = false;

// The forensic log path, mirroring the /Settings convention used by gps_screen.
static const char *THREAT_LOG_PATH = "/Settings/threat_log.txt";

// --- auth string -> AuthMode ------------------------------------------------
// wifi_beacon_manager delivers auth as a "[WPA2-PSK-CCMP][ESS]" style string;
// the pure evil-twin detector wants a normalized detect::AuthMode. Map by
// case-insensitive token match.

// Case-insensitive ASCII substring test (no <ctype.h> locale surprises).
static bool auth_contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    size_t nlen = strlen(needle);
    if (nlen == 0) return false;
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
            ++i;
        }
        if (i == nlen) return true;
    }
    return false;
}

// Order matters. Check the strongest / most specific tokens first so a superset
// string resolves correctly: "WPA2" contains "WPA", and an enterprise string may
// contain "WPA2" - Enterprise (802.1X) must outrank the plain WPA tier, and WPA3
// / WPA2 must be tested before plain WPA. An unrecognized/empty string maps to
// Unknown, which is the safe default (it never triggers a false downgrade flag).
static detect::AuthMode auth_from_string(const char *s)
{
    using detect::AuthMode;
    if (!s || !s[0])                    return AuthMode::Unknown;
    if (auth_contains(s, "ENTERPRISE")) return AuthMode::Enterprise;
    if (auth_contains(s, "WPA3"))       return AuthMode::WPA3;
    if (auth_contains(s, "WPA2"))       return AuthMode::WPA2;
    if (auth_contains(s, "WPA"))        return AuthMode::WPA;
    if (auth_contains(s, "WEP"))        return AuthMode::WEP;
    if (auth_contains(s, "OPEN"))       return AuthMode::Open;
    return AuthMode::Unknown;
}

// --- Beacon callback (WiFi task context) ------------------------------------
// Tiny and non-blocking: reduce the WifiBeacon to the two pure observation
// structs, ingest into both detectors, and report each verdict to the shared
// aggregator. No SD, no LVGL, no allocation, no blocking calls.
static void beacon_cb(const WifiBeacon *b)
{
    if (!b) return;

    uint32_t now = s_now_sec;   // monotonic seconds published by the last tick

    // Rogue-AP / evil-twin observation.
    detect::ApObservation ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.bssid, b->bssid, sizeof(ap.bssid));
    strncpy(ap.ssid, b->ssid, sizeof(ap.ssid) - 1);   // WifiBeacon.ssid is NUL-term
    ap.ssid[sizeof(ap.ssid) - 1] = '\0';
    ap.channel   = b->channel;
    ap.rssi      = b->rssi;
    ap.auth_mode = auth_from_string(b->auth);

    // Beacon-flood observation (same reduction, stamped with now).
    detect::BeaconObservation bo;
    memset(&bo, 0, sizeof(bo));
    memcpy(bo.bssid, b->bssid, sizeof(bo.bssid));
    strncpy(bo.ssid, b->ssid, sizeof(bo.ssid) - 1);
    bo.ssid[sizeof(bo.ssid) - 1] = '\0';
    bo.channel = b->channel;
    bo.t_sec   = now;
    bo.rssi    = b->rssi;

    portENTER_CRITICAL(&s_mux);
    detect::RogueVerdict rv = s_rogue.ingest(ap);
    detect::feed(s_threat, rv.flag, now);
    detect::BeaconVerdict bv = s_beacon_flood.ingest(bo);
    detect::feed(s_threat, bv.flag, now);
    portEXIT_CRITICAL(&s_mux);
}

// --- Shared feed point for the BLE detect pipeline --------------------------
// The BLE side owns its own TailDetector but reports its follow verdict here so
// the Airtag domain lands in the SAME s_threat the WiFi tick ages and drives the
// UI/log from. Guarded by the same spinlock as the WiFi callback.
void detect_pipeline_feed_tracker(detect::TailFlag flag, uint32_t t_sec)
{
    portENTER_CRITICAL(&s_mux);
    detect::feed_tracker(s_threat, flag, t_sec);
    portEXIT_CRITICAL(&s_mux);
}

// --- 1Hz pipeline tick (main/LVGL task context) -----------------------------
void detect_pipeline_tick(uint32_t now_sec)
{
    // Publish the clock for the (clockless) beacon callback first.
    s_now_sec = now_sec;

    // PIGGYBACK activation: attach our beacon consumer ONLY while some OTHER WiFi
    // scan is already running (Evil Twin / Flock / Pwn / wardriver). This never
    // powers WiFi on by itself and never flips a connected STA into monitor mode -
    // it just enriches scans the user already started with threat posture + log.
    // Detach as soon as no other consumer remains, so we never hold WiFi up alone.
    // (wifi_beacon_add is a no-op on WiFi state when it is NOT the first consumer.)
    int others = wifi_beacon_consumer_count() - (s_registered ? 1 : 0);
    if (others > 0 && !s_registered) {
        s_registered = wifi_beacon_add(beacon_cb);
    } else if (others <= 0 && s_registered) {
        wifi_beacon_remove(beacon_cb);
        s_registered = false;
    }

    // Age the sliding window + aggregator and snapshot the per-domain severities
    // and overall level under the SAME lock the callback uses, so the log/UI see
    // a consistent read. Keep all IO (SD) OUT of the critical section.
    detect::Severity  sev[detect::ThreatState::kDomainCount];
    detect::ThreatLevel level;
    portENTER_CRITICAL(&s_mux);
    s_beacon_flood.tick(now_sec);
    s_threat.tick(now_sec);
    for (size_t i = 0; i < detect::ThreatState::kDomainCount; ++i)
        sev[i] = s_threat.domain_severity((detect::ThreatDomain)i);
    level = s_threat.level();
    portEXIT_CRITICAL(&s_mux);

    // Forensic log. Poll EVERY domain every tick (README/e2e gotcha: the log's
    // first update() per domain silently sets its None baseline, so a domain that
    // goes hot before its first poll would have that rise swallowed - polling all
    // domains from boot, all None, avoids it). Persist only genuine edges, and
    // only when the firmware owns the SD card, guarded exactly like
    // gps_save_power() in src/gps_screen.cpp.
    bool sd_ok = instance.isCardReady() && !usb_sd_is_running();
    for (size_t i = 0; i < detect::ThreatState::kDomainCount; ++i) {
        detect::ThreatDomain d = (detect::ThreatDomain)i;
        if (!s_threat_log.update(d, sev[i], now_sec)) continue;  // no edge
        if (!sd_ok) continue;                                    // in-RAM ring still holds it
        if (!SD.exists("/Settings")) SD.mkdir("/Settings");
        File f = SD.open(THREAT_LOG_PATH, FILE_APPEND);
        if (!f) continue;
        char line[64];
        detect::ThreatLog::format(s_threat_log.at(s_threat_log.count() - 1),
                                  line, sizeof(line));
        f.print(line);
        f.print('\n');
        f.close();
    }

    // Drive the UI from the overall posture: Alert (2) or Critical (3) => HADES.
    // argus_set_threat() flips the brand accent to HADES_RED (theme.cpp), and
    // hexhound_set_threat_level() snaps the pet to its wary/HADES-red mood. Both
    // clear on their own when the posture relaxes back below Alert (the pure
    // ThreatState already handles rise-instant / decay-graceful hysteresis).
    bool hot = static_cast<uint8_t>(level) >=
               static_cast<uint8_t>(detect::ThreatLevel::Alert);
    argus_set_threat(hot);
    hexhound_set_threat_level(hot ? 1 : 0);
}
