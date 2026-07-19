// test_beacon_flood.cpp - host unit tests for the pure WiFi beacon-flood /
// fake-AP-spam classifier (src/detect/beacon_flood). DEFENSIVE detection only:
// these exercise the distinct-BSSID sliding-window escalation (None -> Elevated
// -> Flood), dedup of one AP re-beaconing many times, window aging via tick(),
// the not-latched reuse of an aged BSSID, graceful table-full saturation, and
// reset. Time is a plain input on every observation - no clock, no radio, no
// hardware. This is the WiFi analog of test_ble_spam.cpp.
#include "wl_test.h"
#include "beacon_flood.h"

#include <cstdint>
#include <cstring>

using namespace detect;

// Build a beacon observation whose BSSID is derived from a 16-bit index, so
// tests can spin up many DISTINCT (fake) APs cheaply. The BSSID first byte is
// locally-administered (0x02) to stay obviously synthetic. SSID is a fixed name
// by default (channel/ssid are carried but not part of the decision).
static BeaconObservation mk(uint16_t idx, uint32_t t_sec,
                            const char* ssid = "Net", uint8_t channel = 6,
                            int8_t rssi = -50) {
  BeaconObservation o;
  o.bssid[0] = 0x02;
  o.bssid[1] = 0x00;
  o.bssid[2] = 0x00;
  o.bssid[3] = 0x00;
  o.bssid[4] = (uint8_t)(idx >> 8);
  o.bssid[5] = (uint8_t)(idx & 0xFF);
  std::memset(o.ssid, 0, sizeof(o.ssid));
  std::strncpy(o.ssid, ssid, sizeof(o.ssid) - 1);
  o.channel = channel;
  o.t_sec   = t_sec;
  o.rssi    = rssi;
  return o;
}

// ---- A handful of real nearby APs stays None. -------------------------------
WL_TEST(beacon_flood_few_real_aps_none) {
  BeaconFloodDetector d;
  BeaconVerdict v;
  // 12 distinct real APs (a dense-but-honest area), below kElevatedDistinct 20.
  for (uint16_t i = 0; i < 12; ++i) {
    v = d.ingest(mk(i, 100));
    WL_CHECK(v.flag == BeaconFlag::None);
  }
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)12);
  WL_CHECK_EQ(d.tracked(), (size_t)12);
  WL_CHECK(d.flag() == BeaconFlag::None);
}

// ---- A flood of distinct BSSIDs climbs None -> Elevated -> Flood. ------------
WL_TEST(beacon_flood_escalates_to_flood) {
  BeaconFloodDetector d;
  BeaconVerdict v;

  // First 19 distinct: below the Elevated gate (20) -> still None.
  for (uint16_t i = 0; i < 19; ++i)
    v = d.ingest(mk(i, 200));
  WL_CHECK(v.flag == BeaconFlag::None);

  // 20th distinct reaches Elevated.
  v = d.ingest(mk(19, 200));
  WL_CHECK(v.flag == BeaconFlag::Elevated);
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)20);

  // Distinct 21..39: Elevated but not yet Flood.
  for (uint16_t i = 20; i < 39; ++i)
    v = d.ingest(mk(i, 200));
  WL_CHECK(v.flag == BeaconFlag::Elevated);

  // 40th distinct reaches the Flood gate (kFloodDistinct).
  v = d.ingest(mk(39, 200));
  WL_CHECK(v.flag == BeaconFlag::Flood);
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)40);
}

// ---- The same AP re-beaconing is deduped: one distinct AP, never a flood no
//      matter how many beacons it sends (a legit AP beacons ~10x/sec). --------
WL_TEST(beacon_flood_same_bssid_deduped) {
  BeaconFloodDetector d;
  BeaconVerdict v;
  for (int i = 0; i < 100; ++i)
    v = d.ingest(mk(7, 400));
  WL_CHECK(v.flag == BeaconFlag::None);
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)1);
  WL_CHECK_EQ(d.tracked(), (size_t)1);
}

// ---- tick() ages a stopped flood back to None and frees the slots. ----------
WL_TEST(beacon_flood_tick_relaxes_stopped_flood) {
  BeaconFloodDetector d;
  BeaconVerdict v;
  for (uint16_t i = 0; i < 45; ++i)
    v = d.ingest(mk(i, 500));
  WL_CHECK(v.flag == BeaconFlag::Flood);
  WL_CHECK_EQ(d.tracked(), (size_t)45);

  // Still inside the window (age 4 < kWindowSec 5) -> flood still visible.
  d.tick(504);
  WL_CHECK(d.flag() == BeaconFlag::Flood);
  WL_CHECK_EQ(d.tracked(), (size_t)45);

  // Advance past the window (age 5 == kWindowSec) -> everything drops out.
  d.tick(505);
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  WL_CHECK(d.flag() == BeaconFlag::None);
  WL_CHECK_EQ(d.distinct(), (uint16_t)0);
}

// ---- A stale BSSID is re-usable: after it ages out, feeding it again counts as
//      one fresh distinct AP (window is not latched). --------------------------
WL_TEST(beacon_flood_window_not_latched) {
  BeaconFloodDetector d;
  // Fill to Elevated, then let it all age out.
  for (uint16_t i = 0; i < 22; ++i) d.ingest(mk(i, 600));
  WL_CHECK(d.flag() == BeaconFlag::Elevated);
  d.tick(610);
  WL_CHECK(d.flag() == BeaconFlag::None);
  // A single beacon later is just one AP -> None again.
  BeaconVerdict v = d.ingest(mk(0, 620));
  WL_CHECK(v.flag == BeaconFlag::None);
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)1);
}

// ---- Table full: many more distinct BSSIDs than the cap must saturate, stay
//      flagged Flood, and never crash or exceed the cap. ----------------------
WL_TEST(beacon_flood_table_full_saturates) {
  BeaconFloodDetector d;
  BeaconVerdict v;
  // 300 distinct fake APs in one second: far past kMaxBssids (64).
  for (uint16_t i = 0; i < 300; ++i)
    v = d.ingest(mk(i, 700));
  WL_CHECK(v.flag == BeaconFlag::Flood);
  // Distinct count saturates at the cap; it never exceeds kMaxBssids.
  WL_CHECK_EQ(d.tracked(), (size_t)BeaconFloodDetector::kMaxBssids);
  WL_CHECK(d.distinct() <= (uint16_t)BeaconFloodDetector::kMaxBssids);
  WL_CHECK(d.flag() == BeaconFlag::Flood);
}

// ---- A real AP re-beaconing many times AMONG a small set stays None: dedup
//      keeps a chatty-but-legit environment from tripping the gate. -----------
WL_TEST(beacon_flood_chatty_real_aps_stay_none) {
  BeaconFloodDetector d;
  BeaconVerdict v;
  // 8 real APs, each beaconing 50 times within the window.
  for (int round = 0; round < 50; ++round)
    for (uint16_t i = 0; i < 8; ++i)
      v = d.ingest(mk(i, 750));
  WL_CHECK(v.flag == BeaconFlag::None);
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)8);
  WL_CHECK_EQ(d.tracked(), (size_t)8);
}

// ---- reset() forgets everything; a fresh flood starts from baseline again. ---
WL_TEST(beacon_flood_reset_clears) {
  BeaconFloodDetector d;
  for (uint16_t i = 0; i < 45; ++i) d.ingest(mk(i, 800));
  WL_CHECK(d.tracked() >= (size_t)1);
  d.reset();
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  WL_CHECK(d.flag() == BeaconFlag::None);
  BeaconVerdict v = d.ingest(mk(0, 900));
  WL_CHECK(v.flag == BeaconFlag::None);
  WL_CHECK_EQ(v.distinct_bssids_per_win, (uint16_t)1);
}
