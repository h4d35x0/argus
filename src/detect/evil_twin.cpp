// evil_twin.cpp - implementation of the pure rogue-AP classifier.
//
// Tradecraft each rule is grounded in (all DEFENSIVE detection, no transmit):
//
//   NewBssidForKnownSsid   The classic evil twin. An attacker stands up a
//                          second radio (a different BSSID / MAC) beaconing an
//                          SSID a victim already trusts, hoping clients roam to
//                          it. Legitimate multi-AP networks (mesh, campus
//                          roaming) ALSO show many BSSIDs per SSID, so this is a
//                          suspicion signal to correlate, not proof - hence a
//                          flag, not a block. First BSSID for an SSID is
//                          baseline, only additional ones flag.
//
//   SecurityDowngrade /    A network normally seen secured (WPA2/WPA3) suddenly
//   OpenTwinOfSecuredSsid  advertised with weaker or no encryption is a hallmark
//                          of a captive-portal / credential-harvest twin: the
//                          rogue drops encryption so any client can associate.
//                          We keep the STRONGEST posture ever seen as baseline
//                          so a single Open advert of a normally-WPA2 SSID is
//                          caught even after benign upgrades. An Open advert of
//                          a secured SSID is the most severe case and gets its
//                          own flag.
//
//   ChannelChangeForBssid  A given radio (BSSID) that jumps channels between
//                          sightings can indicate a cloned/spoofed BSSID (the
//                          impostor cannot always occupy the victim AP's exact
//                          channel) or an AP relocation. Benign causes exist
//                          (DFS radar avoidance, auto-channel), so it is the
//                          lowest-priority signal.
//
// Only DEVIATION from an established baseline flags; the first sighting of any
// SSID or BSSID is recorded silently to avoid false positives on normal
// discovery.
#include "evil_twin.h"

#include <cstring>

namespace detect {
namespace {

// Security ranking used only for "is this weaker?" comparisons. Higher is
// stronger. Unknown is 0 so it never reads as an upgrade; downgrade logic
// additionally refuses to alarm when EITHER side is Unknown (never alarm on
// "we don't know").
uint8_t auth_strength(AuthMode a) {
  switch (a) {
    case AuthMode::Open:       return 1;
    case AuthMode::WEP:        return 2;
    case AuthMode::WPA:        return 3;
    case AuthMode::WPA2:       return 4;
    case AuthMode::WPA3:       return 5;
    case AuthMode::Enterprise: return 6;
    case AuthMode::Unknown:    return 0;
  }
  return 0;
}

// "Secured" = a real, known encryption posture stronger than Open.
bool is_secured(AuthMode a) {
  return a != AuthMode::Open && a != AuthMode::Unknown;
}

bool bssid_eq(const uint8_t a[6], const uint8_t b[6]) {
  return std::memcmp(a, b, 6) == 0;
}

bool ssid_is_hidden(const char* s) {
  return s == nullptr || s[0] == '\0';
}

}  // namespace

RogueApDetector::SsidEntry* RogueApDetector::find_ssid(const char* ssid) {
  for (uint8_t i = 0; i < kMaxSsids; ++i) {
    if (ssids_[i].used && std::strncmp(ssids_[i].ssid, ssid, sizeof(ssids_[i].ssid)) == 0)
      return &ssids_[i];
  }
  return nullptr;
}

RogueApDetector::SsidEntry* RogueApDetector::alloc_ssid(const char* ssid, AuthMode auth,
                                                        const uint8_t bssid[6]) {
  for (uint8_t i = 0; i < kMaxSsids; ++i) {
    if (!ssids_[i].used) {
      SsidEntry& e = ssids_[i];
      e.used = true;
      std::strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
      e.ssid[sizeof(e.ssid) - 1] = '\0';
      e.baseline_auth = auth;
      e.bssid_count = 1;
      std::memcpy(e.bssids[0], bssid, 6);
      return &e;
    }
  }
  return nullptr;  // table full: stop tracking new SSIDs (documented)
}

RogueApDetector::BssidEntry* RogueApDetector::find_bssid(const uint8_t bssid[6]) {
  for (uint16_t i = 0; i < kMaxBssids; ++i) {
    if (bssids_[i].used && bssid_eq(bssids_[i].bssid, bssid))
      return &bssids_[i];
  }
  return nullptr;
}

RogueVerdict RogueApDetector::ingest(const ApObservation& obs) {
  RogueVerdict v = {RogueFlag::None, 0};

  // --- BSSID channel tracking (independent of SSID; applies to hidden APs too).
  // channel == 0 means "unknown" and is never treated as a change.
  RogueFlag channel_flag = RogueFlag::None;
  uint8_t   channel_detail = 0;
  if (obs.channel != 0) {
    BssidEntry* be = find_bssid(obs.bssid);
    if (be == nullptr) {
      // First sighting of this radio -> baseline only (or drop silently if the
      // table is full).
      for (uint16_t i = 0; i < kMaxBssids; ++i) {
        if (!bssids_[i].used) {
          bssids_[i].used = true;
          std::memcpy(bssids_[i].bssid, obs.bssid, 6);
          bssids_[i].channel = obs.channel;
          break;
        }
      }
    } else if (be->channel != obs.channel) {
      channel_flag = RogueFlag::ChannelChangeForBssid;
      channel_detail = be->channel;  // prior channel; the new one is in obs
      be->channel = obs.channel;     // adopt the new channel as baseline
    }
  }

  // --- SSID-level tracking (named networks only). Hidden / empty SSIDs are not
  // a "named network", so we never bucket all hidden APs together (which would
  // false-positive an evil twin across every hidden AP in range).
  RogueFlag ssid_flag = RogueFlag::None;
  uint8_t   ssid_detail = 0;
  if (!ssid_is_hidden(obs.ssid)) {
    SsidEntry* se = find_ssid(obs.ssid);
    if (se == nullptr) {
      // First time we have seen this network name -> baseline only (alloc is a
      // no-op returning nullptr if the SSID table is full).
      alloc_ssid(obs.ssid, obs.auth_mode, obs.bssid);
    } else {
      // (1) Security downgrade vs the strongest posture ever seen for this SSID.
      //     Ignore Unknown on either side.
      if (obs.auth_mode != AuthMode::Unknown &&
          se->baseline_auth != AuthMode::Unknown &&
          auth_strength(obs.auth_mode) < auth_strength(se->baseline_auth)) {
        if (is_secured(se->baseline_auth) && obs.auth_mode == AuthMode::Open) {
          ssid_flag = RogueFlag::OpenTwinOfSecuredSsid;  // most severe
        } else {
          ssid_flag = RogueFlag::SecurityDowngrade;
        }
        ssid_detail = static_cast<uint8_t>(se->baseline_auth);
      }

      // (2) New BSSID advertising this known SSID = classic evil twin. Only
      //     surfaced if a (more specific / severe) downgrade was not already
      //     flagged for this same sighting.
      bool known_bssid = false;
      for (uint8_t i = 0; i < se->bssid_count; ++i) {
        if (bssid_eq(se->bssids[i], obs.bssid)) { known_bssid = true; break; }
      }
      if (!known_bssid) {
        if (ssid_flag == RogueFlag::None) {
          ssid_flag = RogueFlag::NewBssidForKnownSsid;
          ssid_detail = se->bssid_count;  // radios already claiming this SSID
        }
        // Record it if there is room; if the per-SSID list is full we keep
        // flagging new radios (safe over-report) but cannot store them.
        if (se->bssid_count < kMaxBssidsPerSsid) {
          std::memcpy(se->bssids[se->bssid_count], obs.bssid, 6);
          se->bssid_count++;
        }
      }

      // Keep baseline_auth at the STRONGEST posture ever seen: a benign upgrade
      // must not erase our knowledge that the network is normally secured, or a
      // later Open twin would slip past. Mutually exclusive with the downgrade
      // branch above (an observation cannot be both weaker and stronger).
      if (auth_strength(obs.auth_mode) > auth_strength(se->baseline_auth)) {
        se->baseline_auth = obs.auth_mode;
      }
    }
  }

  // Precedence: an SSID-level verdict (active impersonation / downgrade)
  // outranks a channel hop, which has more benign causes.
  if (ssid_flag != RogueFlag::None) {
    v.flag = ssid_flag;
    v.detail = ssid_detail;
  } else if (channel_flag != RogueFlag::None) {
    v.flag = channel_flag;
    v.detail = channel_detail;
  }
  return v;
}

void RogueApDetector::reset() {
  std::memset(ssids_, 0, sizeof(ssids_));
  std::memset(bssids_, 0, sizeof(bssids_));
}

size_t RogueApDetector::tracked_ssids() const {
  size_t n = 0;
  for (uint8_t i = 0; i < kMaxSsids; ++i)
    if (ssids_[i].used) ++n;
  return n;
}

size_t RogueApDetector::tracked_bssids() const {
  size_t n = 0;
  for (uint16_t i = 0; i < kMaxBssids; ++i)
    if (bssids_[i].used) ++n;
  return n;
}

}  // namespace detect
