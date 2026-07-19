// test_evil_twin.cpp - host unit tests for the pure rogue-AP / evil-twin
// classifier (src/detect/evil_twin). DEFENSIVE detection only: these exercise
// baseline learning and deviation flagging, plus the graceful full-table
// behavior and the no-cross-trigger guarantees the module exists to provide.
#include "wl_test.h"
#include "evil_twin.h"

#include <cstdint>
#include <cstring>

using namespace detect;

static const uint8_t AP1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t AP2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
static const uint8_t AP3[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};

// Build an observation. rssi is irrelevant to the logic; carried for realism.
static ApObservation mk(const uint8_t bssid[6], const char* ssid, uint8_t channel,
                        AuthMode auth) {
  ApObservation o;
  std::memcpy(o.bssid, bssid, 6);
  std::memset(o.ssid, 0, sizeof(o.ssid));
  if (ssid) std::strncpy(o.ssid, ssid, sizeof(o.ssid) - 1);
  o.channel = channel;
  o.rssi = -50;
  o.auth_mode = auth;
  return o;
}

// A BSSID that differs from AP1 only in the last octet, to build many distinct
// radios for the same SSID (per-SSID list-full test).
static void bssid_n(uint8_t out[6], uint8_t n) {
  static const uint8_t base[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
  std::memcpy(out, base, 6);
  out[5] = n;
}

// ---- Baseline: first sighting is never a flag; re-seeing the same AP is not. -
WL_TEST(evil_twin_first_sighting_is_baseline) {
  RogueApDetector d;
  RogueVerdict v = d.ingest(mk(AP1, "CoffeeWiFi", 6, AuthMode::WPA2));
  WL_CHECK(v.flag == RogueFlag::None);
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)1);
  WL_CHECK_EQ(d.tracked_bssids(), (size_t)1);

  // Exact same AP again: still baseline, no new tracking entries.
  RogueVerdict v2 = d.ingest(mk(AP1, "CoffeeWiFi", 6, AuthMode::WPA2));
  WL_CHECK(v2.flag == RogueFlag::None);
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)1);
  WL_CHECK_EQ(d.tracked_bssids(), (size_t)1);
}

// ---- Classic evil twin: same SSID, a different radio. ----------------------
WL_TEST(evil_twin_new_bssid_for_known_ssid) {
  RogueApDetector d;
  d.ingest(mk(AP1, "CoffeeWiFi", 6, AuthMode::WPA2));           // baseline
  RogueVerdict v = d.ingest(mk(AP2, "CoffeeWiFi", 6, AuthMode::WPA2));  // rogue
  WL_CHECK(v.flag == RogueFlag::NewBssidForKnownSsid);
  WL_CHECK_EQ(v.detail, (uint8_t)1);   // one radio already claimed the SSID
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)1);
  WL_CHECK_EQ(d.tracked_bssids(), (size_t)2);

  // Seen a third time it is now known -> no longer flagged.
  RogueVerdict v2 = d.ingest(mk(AP2, "CoffeeWiFi", 6, AuthMode::WPA2));
  WL_CHECK(v2.flag == RogueFlag::None);
}

// ---- Security downgrade (same radio, weaker cipher) vs open-twin (rogue). ---
WL_TEST(evil_twin_security_downgrade_same_bssid) {
  RogueApDetector d;
  d.ingest(mk(AP1, "Corp", 6, AuthMode::WPA2));                 // baseline WPA2
  // Same BSSID re-beaconing as WEP: a downgrade, but not a new radio and not
  // Open -> SecurityDowngrade, carrying the prior (baseline) auth.
  RogueVerdict v = d.ingest(mk(AP1, "Corp", 6, AuthMode::WEP));
  WL_CHECK(v.flag == RogueFlag::SecurityDowngrade);
  WL_CHECK_EQ(v.detail, (uint8_t)AuthMode::WPA2);
}

WL_TEST(evil_twin_open_twin_of_secured_ssid) {
  RogueApDetector d;
  d.ingest(mk(AP1, "Bank", 6, AuthMode::WPA2));                 // baseline secured
  // Different radio advertising the SAME SSID as Open: both new-BSSID AND an
  // open downgrade apply; the more severe OpenTwin flag wins.
  RogueVerdict v = d.ingest(mk(AP2, "Bank", 6, AuthMode::Open));
  WL_CHECK(v.flag == RogueFlag::OpenTwinOfSecuredSsid);
  WL_CHECK_EQ(v.detail, (uint8_t)AuthMode::WPA2);
}

WL_TEST(evil_twin_upgrade_does_not_erase_secured_baseline) {
  RogueApDetector d;
  d.ingest(mk(AP1, "Home", 6, AuthMode::WPA2));                 // baseline WPA2
  d.ingest(mk(AP1, "Home", 6, AuthMode::WPA3));                 // benign upgrade
  // A later Open advert must still be caught against the strongest posture.
  RogueVerdict v = d.ingest(mk(AP1, "Home", 6, AuthMode::Open));
  WL_CHECK(v.flag == RogueFlag::OpenTwinOfSecuredSsid);
  WL_CHECK_EQ(v.detail, (uint8_t)AuthMode::WPA3);
}

// ---- Channel hop for a known radio. ----------------------------------------
WL_TEST(evil_twin_channel_change_for_bssid) {
  RogueApDetector d;
  d.ingest(mk(AP1, "Net", 6, AuthMode::WPA2));                  // baseline ch 6
  RogueVerdict v = d.ingest(mk(AP1, "Net", 11, AuthMode::WPA2));
  WL_CHECK(v.flag == RogueFlag::ChannelChangeForBssid);
  WL_CHECK_EQ(v.detail, (uint8_t)6);   // prior channel

  // Back on the new channel: now baseline, no flag.
  RogueVerdict v2 = d.ingest(mk(AP1, "Net", 11, AuthMode::WPA2));
  WL_CHECK(v2.flag == RogueFlag::None);
}

WL_TEST(evil_twin_channel_zero_is_never_a_change) {
  RogueApDetector d;
  d.ingest(mk(AP1, "Net", 6, AuthMode::WPA2));                  // baseline ch 6
  // channel 0 == unknown: no ChannelChange flag, and no SSID-level deviation.
  RogueVerdict v = d.ingest(mk(AP1, "Net", 0, AuthMode::WPA2));
  WL_CHECK(v.flag == RogueFlag::None);
}

// ---- Hidden / empty SSID is not tracked as a named network. ----------------
WL_TEST(evil_twin_hidden_ssid_not_tracked_as_named) {
  RogueApDetector d;
  WL_CHECK(d.ingest(mk(AP1, "", 6, AuthMode::WPA2)).flag == RogueFlag::None);
  // A DIFFERENT hidden AP must NOT be flagged as an evil twin of the first.
  WL_CHECK(d.ingest(mk(AP2, "", 6, AuthMode::WPA2)).flag == RogueFlag::None);
  // nullptr ssid treated as hidden too.
  WL_CHECK(d.ingest(mk(AP3, nullptr, 6, AuthMode::WPA2)).flag == RogueFlag::None);
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)0);   // no named networks tracked
  WL_CHECK_EQ(d.tracked_bssids(), (size_t)3);  // radios still channel-tracked
}

// ---- Two genuinely different networks do not cross-trigger. ----------------
WL_TEST(evil_twin_distinct_networks_do_not_cross_trigger) {
  RogueApDetector d;
  WL_CHECK(d.ingest(mk(AP1, "Alpha", 1, AuthMode::WPA2)).flag == RogueFlag::None);
  WL_CHECK(d.ingest(mk(AP2, "Beta", 6, AuthMode::WPA3)).flag == RogueFlag::None);
  // Re-seeing each remains clean.
  WL_CHECK(d.ingest(mk(AP1, "Alpha", 1, AuthMode::WPA2)).flag == RogueFlag::None);
  WL_CHECK(d.ingest(mk(AP2, "Beta", 6, AuthMode::WPA3)).flag == RogueFlag::None);
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)2);
}

// ---- reset() clears all learned baseline. ----------------------------------
WL_TEST(evil_twin_reset_clears_state) {
  RogueApDetector d;
  d.ingest(mk(AP1, "CoffeeWiFi", 6, AuthMode::WPA2));
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)1);
  d.reset();
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)0);
  WL_CHECK_EQ(d.tracked_bssids(), (size_t)0);
  // After reset the same AP is once again a baseline (not a flag).
  WL_CHECK(d.ingest(mk(AP1, "CoffeeWiFi", 6, AuthMode::WPA2)).flag == RogueFlag::None);
}

// ---- Full SSID table: stop tracking new names, no crash, no false flags. ----
WL_TEST(evil_twin_ssid_table_full_degrades_gracefully) {
  RogueApDetector d;
  char name[16];
  uint8_t b[6];
  // Fill exactly kMaxSsids distinct named networks, each on its own BSSID.
  for (uint8_t i = 0; i < RogueApDetector::kMaxSsids; ++i) {
    std::snprintf(name, sizeof(name), "net-%u", (unsigned)i);
    bssid_n(b, i);
    WL_CHECK(d.ingest(mk(b, name, 6, AuthMode::WPA2)).flag == RogueFlag::None);
  }
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)RogueApDetector::kMaxSsids);

  // One more distinct network: table full -> not tracked, but must not crash
  // and must not be flagged (an untracked SSID has no baseline to deviate from).
  bssid_n(b, 200);
  WL_CHECK(d.ingest(mk(b, "overflow", 6, AuthMode::WPA2)).flag == RogueFlag::None);
  WL_CHECK_EQ(d.tracked_ssids(), (size_t)RogueApDetector::kMaxSsids);
  // Even a would-be evil twin of the untracked SSID stays silent (no baseline).
  bssid_n(b, 201);
  WL_CHECK(d.ingest(mk(b, "overflow", 6, AuthMode::WPA2)).flag == RogueFlag::None);
}

// ---- Full per-SSID BSSID list: keep flagging new radios, never crash. -------
WL_TEST(evil_twin_per_ssid_bssid_list_full_over_reports_safely) {
  RogueApDetector d;
  uint8_t b[6];
  // First BSSID is baseline; then fill the list with (kMaxBssidsPerSsid - 1)
  // more, each flagged as a new BSSID for the known SSID.
  bssid_n(b, 0);
  WL_CHECK(d.ingest(mk(b, "Roam", 6, AuthMode::WPA2)).flag == RogueFlag::None);
  for (uint8_t i = 1; i < RogueApDetector::kMaxBssidsPerSsid; ++i) {
    bssid_n(b, i);
    WL_CHECK(d.ingest(mk(b, "Roam", 6, AuthMode::WPA2)).flag ==
             RogueFlag::NewBssidForKnownSsid);
  }
  // The list is now full. A brand-new radio for the same SSID must STILL be
  // flagged (safe over-report), and the call must not crash.
  bssid_n(b, 250);
  WL_CHECK(d.ingest(mk(b, "Roam", 6, AuthMode::WPA2)).flag ==
           RogueFlag::NewBssidForKnownSsid);
}
