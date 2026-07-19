// evil_twin.h - pure, host-testable evil-twin / rogue-AP DETECTION logic.
//
// DEFENSIVE ONLY. This module never transmits, deauthenticates, or attacks
// anything. It ingests a stream of observed Wi-Fi access-point beacons (as they
// would be produced by the on-device scan / beacon path) and classifies whether
// a given sighting DEVIATES from the baseline established by earlier sightings
// in a way that matches known rogue-AP / evil-twin tradecraft.
//
// It is deliberately self-contained: it defines its own small observation
// struct and its own auth enum, and includes only standard headers. No
// Arduino.h, no esp_wifi, no LVGL, no hardware. That keeps the decision logic
// unit-testable on the host and reusable regardless of how beacons are sourced.
// On-device, a WifiBeacon (bssid[6], ssid[33], auth string, rssi, channel) maps
// trivially onto ApObservation, and the esp_wifi WIFI_AUTH_* constants map onto
// AuthMode; that mapping lives at the (device-only) call site, not here.
//
// Integration (wiring a real scan into ingest(), surfacing verdicts in the UI)
// is a separate, hardware-gated step and is intentionally NOT done in this
// module.
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// Normalized authentication / encryption posture of an AP. The declaration
// order is NOT the security ranking (Unknown sorts first for a clean zero
// default); use the internal auth_strength() for "weaker than" comparisons.
enum class AuthMode : uint8_t {
  Unknown = 0,  // could not be determined
  Open,         // no encryption
  WEP,          // legacy, broken
  WPA,          // TKIP-era
  WPA2,         // CCMP
  WPA3,         // SAE
  Enterprise,   // 802.1X / RADIUS
};

// A single observed beacon. An empty ssid ("") means hidden / broadcast-
// suppressed and is never treated as a named network.
struct ApObservation {
  uint8_t  bssid[6];
  char     ssid[33];   // NUL-terminated
  uint8_t  channel;    // 1..14; 0 == unknown
  int8_t   rssi;       // dBm; carried for callers/telemetry, not used in logic
  AuthMode auth_mode;
};

// The single classification a sighting can receive. Exactly one flag is
// returned per ingest() call; see the .cpp for the precedence rule when several
// conditions are simultaneously true.
enum class RogueFlag : uint8_t {
  None = 0,
  NewBssidForKnownSsid,   // classic evil twin: another radio claiming a known SSID
  SecurityDowngrade,      // known SSID now advertised with weaker encryption
  ChannelChangeForBssid,  // a known BSSID has jumped to a different channel
  OpenTwinOfSecuredSsid,  // most severe downgrade: a secured SSID now seen OPEN
};

struct RogueVerdict {
  RogueFlag flag;
  // Context for the flag, so a caller can log / alert without re-deriving it:
  //   ChannelChangeForBssid                     -> the PRIOR channel
  //   SecurityDowngrade / OpenTwinOfSecuredSsid -> prior (baseline) AuthMode
  //   NewBssidForKnownSsid                      -> # of BSSIDs already known
  //   None                                      -> 0
  uint8_t detail;
};

// Stateful rogue-AP classifier. Fixed-size internal tables, no dynamic
// allocation. Copyable/movable is fine (it is just POD tables), but it is
// designed to be a single long-lived instance owned by the scan pipeline.
class RogueApDetector {
 public:
  // Tracking caps. When a table is full the detector degrades gracefully:
  //   - SSID table full     -> stop tracking NEW named networks (no flag, no
  //                            crash); already-tracked ones keep working.
  //   - per-SSID BSSID list  -> keep FLAGGING new radios that claim the SSID
  //                            (safe over-report) but cannot store them.
  //   - BSSID table full     -> stop tracking NEW radios' channels.
  // See the .cpp for the full rationale.
  static const uint8_t  kMaxSsids = 32;
  static const uint8_t  kMaxBssidsPerSsid = 8;
  static const uint16_t kMaxBssids = 128;

  RogueApDetector() { reset(); }

  // Ingest one beacon; return the verdict for THIS sighting. A first-ever
  // sighting only establishes baseline and returns {None, 0} - normal discovery
  // is never a flag, only deviation from an established baseline is.
  RogueVerdict ingest(const ApObservation& obs);

  // Forget all learned baseline (both tables).
  void reset();

  // How many named SSIDs / distinct BSSIDs are currently tracked.
  size_t tracked_ssids() const;
  size_t tracked_bssids() const;

 private:
  struct SsidEntry {
    bool     used;
    char     ssid[33];
    AuthMode baseline_auth;               // strongest auth ever seen for this SSID
    uint8_t  bssid_count;
    uint8_t  bssids[kMaxBssidsPerSsid][6];
  };
  struct BssidEntry {
    bool    used;
    uint8_t bssid[6];
    uint8_t channel;                      // last channel seen
  };

  SsidEntry  ssids_[kMaxSsids];
  BssidEntry bssids_[kMaxBssids];

  SsidEntry* find_ssid(const char* ssid);
  SsidEntry* alloc_ssid(const char* ssid, AuthMode auth, const uint8_t bssid[6]);
  BssidEntry* find_bssid(const uint8_t bssid[6]);
};

}  // namespace detect
