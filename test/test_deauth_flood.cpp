// test_deauth_flood.cpp - host unit tests for the pure deauth/disassoc flood
// classifier (src/detect/deauth_flood). DEFENSIVE detection only: these exercise
// the per-BSSID sliding-window escalation (None -> Elevated -> Flood), window
// aging via tick(), per-BSSID independence, the Other-frame no-op, the global
// scattergun aggregate, and graceful table-full degradation. Time is a plain
// input on every event - no clock, no radio, no hardware.
#include "wl_test.h"
#include "deauth_flood.h"

#include <cstdint>

using namespace detect;

// Build a management-frame event. bssid is derived from a single byte so tests
// can spin up many distinct APs cheaply; rssi is carried for realism only.
static MgmtFrameEvent mk(uint8_t ap, MgmtType type, uint32_t t_sec,
                         int8_t rssi = -50) {
  MgmtFrameEvent e;
  e.bssid[0] = 0x02;  // locally-administered, keeps them obviously synthetic
  e.bssid[1] = 0x00;
  e.bssid[2] = 0x00;
  e.bssid[3] = 0x00;
  e.bssid[4] = 0x00;
  e.bssid[5] = ap;
  e.type  = type;
  e.t_sec = t_sec;
  e.rssi  = rssi;
  return e;
}

// ---- A single deauth is never a flood (the odd deauth is normal). -----------
WL_TEST(deauth_single_frame_is_none) {
  DeauthFloodDetector d;
  DeauthVerdict v = d.ingest(mk(1, MgmtType::Deauth, 100));
  WL_CHECK(v.flag == DeauthFlag::None);
  WL_CHECK_EQ(d.tracked(), (size_t)1);
  WL_CHECK(d.global_flag() == DeauthFlag::None);
}

// ---- A slow trickle (well under 1/sec) stays None across the whole window. ---
WL_TEST(deauth_slow_trickle_stays_none) {
  DeauthFloodDetector d;
  // One frame every 3 seconds -> at most ~4 in any 10s window, below Elevated.
  for (uint32_t t = 0; t <= 60; t += 3) {
    DeauthVerdict v = d.ingest(mk(2, MgmtType::Disassoc, t));
    WL_CHECK(v.flag == DeauthFlag::None);
  }
  WL_CHECK(d.global_flag() == DeauthFlag::None);
}

// ---- A burst on ONE BSSID climbs None -> Elevated -> Flood. -----------------
WL_TEST(deauth_burst_escalates_elevated_then_flood) {
  DeauthFloodDetector d;
  DeauthVerdict v;

  // Frames 1..9 in the same second: below kElevatedPerWin (10) -> still None.
  for (int i = 1; i <= 9; ++i) v = d.ingest(mk(3, MgmtType::Deauth, 500));
  WL_CHECK(v.flag == DeauthFlag::None);

  // 10th frame reaches the Elevated gate.
  v = d.ingest(mk(3, MgmtType::Deauth, 500));
  WL_CHECK(v.flag == DeauthFlag::Elevated);

  // Frames 11..49: Elevated but not yet Flood.
  for (int i = 11; i <= 49; ++i) v = d.ingest(mk(3, MgmtType::Deauth, 500));
  WL_CHECK(v.flag == DeauthFlag::Elevated);

  // 50th frame reaches the Flood gate (kFloodPerWin). Rate = 50 * 60 / 10.
  v = d.ingest(mk(3, MgmtType::Deauth, 500));
  WL_CHECK(v.flag == DeauthFlag::Flood);
  WL_CHECK_EQ(v.rate_per_min, (uint16_t)300);

  // A mix of deauth AND disassoc both count toward the same window.
  v = d.ingest(mk(3, MgmtType::Disassoc, 500));
  WL_CHECK(v.flag == DeauthFlag::Flood);
}

// ---- tick() ages a stopped flood back to None and frees the slot. -----------
WL_TEST(deauth_tick_ages_stopped_flood_to_none) {
  DeauthFloodDetector d;
  DeauthVerdict v;
  for (int i = 0; i < 50; ++i) v = d.ingest(mk(4, MgmtType::Deauth, 100));
  WL_CHECK(v.flag == DeauthFlag::Flood);
  WL_CHECK_EQ(d.tracked(), (size_t)1);

  // Still inside the window (age 9 < kWindowSec) -> attack still visible. The
  // 50 frames land on one BSSID, so globally that is Elevated (>= 20) but below
  // the higher global Flood gate (80) - the per-BSSID gate is what fired Flood.
  d.tick(109);
  WL_CHECK_EQ(d.tracked(), (size_t)1);
  WL_CHECK(d.global_flag() == DeauthFlag::Elevated);

  // Advance past the window (age 10 == kWindowSec) -> everything drops out.
  d.tick(110);
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  WL_CHECK(d.global_flag() == DeauthFlag::None);
  WL_CHECK_EQ(d.global_rate_per_min(), (uint16_t)0);
}

// ---- Two BSSIDs are tracked independently; one flooding does not taint the
//      other, and vice versa. ------------------------------------------------
WL_TEST(deauth_two_bssids_independent) {
  DeauthFloodDetector d;
  DeauthVerdict va;
  // BSSID A is flooded.
  for (int i = 0; i < 50; ++i) va = d.ingest(mk(10, MgmtType::Deauth, 200));
  WL_CHECK(va.flag == DeauthFlag::Flood);
  // BSSID B sees a single frame in the same window -> still None.
  DeauthVerdict vb = d.ingest(mk(11, MgmtType::Deauth, 200));
  WL_CHECK(vb.flag == DeauthFlag::None);
  WL_CHECK_EQ(d.tracked(), (size_t)2);
}

// ---- Other-type frames are inert: no state, no counts, no verdict change. ---
WL_TEST(deauth_other_frames_ignored) {
  DeauthFloodDetector d;
  for (int i = 0; i < 100; ++i) {
    DeauthVerdict v = d.ingest(mk(20, MgmtType::Other, 300));
    WL_CHECK(v.flag == DeauthFlag::None);
    WL_CHECK_EQ(v.rate_per_min, (uint16_t)0);
  }
  WL_CHECK_EQ(d.tracked(), (size_t)0);          // no BSSID ever tracked
  WL_CHECK(d.global_flag() == DeauthFlag::None); // nothing counted globally
}

// ---- Global aggregate catches a scattergun spread thin across many BSSIDs. --
WL_TEST(deauth_global_catches_scattergun) {
  DeauthFloodDetector d;
  // 16 distinct APs (fills the table exactly, no eviction), 6 frames each, all
  // in one second. Each AP sees 6 < kElevatedPerWin (10) so NONE trips its own
  // gate, but the aggregate is 96 >= kGlobalFloodPerWin (80).
  for (uint8_t ap = 0; ap < DeauthFloodDetector::kMaxBssids; ++ap) {
    for (int i = 0; i < 6; ++i) {
      DeauthVerdict v = d.ingest(mk(ap, MgmtType::Deauth, 400));
      WL_CHECK(v.flag == DeauthFlag::None);   // no single BSSID escalates
    }
  }
  WL_CHECK_EQ(d.tracked(), (size_t)DeauthFloodDetector::kMaxBssids);
  WL_CHECK(d.global_flag() == DeauthFlag::Flood);   // ... but the air is hostile
  WL_CHECK_EQ(d.global_rate_per_min(), (uint16_t)(96 * 60 / 10));
}

// ---- Table full: the active attacker survives while idle one-offs are
//      recycled; no crash, count stays capped. ------------------------------
WL_TEST(deauth_table_full_degrades_gracefully) {
  DeauthFloodDetector d;
  DeauthVerdict v;

  // BSSID 0 is the real attacker - drive it to Flood.
  for (int i = 0; i < 50; ++i) v = d.ingest(mk(0, MgmtType::Deauth, 100));
  WL_CHECK(v.flag == DeauthFlag::Flood);

  // Fill the rest of the table with idle one-off APs (a single frame each).
  for (uint8_t ap = 1; ap < DeauthFloodDetector::kMaxBssids; ++ap) {
    d.ingest(mk(ap, MgmtType::Deauth, 100));
  }
  WL_CHECK_EQ(d.tracked(), (size_t)DeauthFloodDetector::kMaxBssids);

  // Flood the table with many MORE brand-new APs. Each forces an eviction of a
  // least-active (count 1) entry - never the attacker. Must not crash; the
  // count stays capped and each newcomer classifies cleanly as None.
  for (uint8_t ap = 100; ap < 140; ++ap) {
    v = d.ingest(mk(ap, MgmtType::Deauth, 100));
    WL_CHECK(v.flag == DeauthFlag::None);
  }
  WL_CHECK_EQ(d.tracked(), (size_t)DeauthFloodDetector::kMaxBssids);

  // The attacker's evidence survived the churn - one more frame is still Flood.
  v = d.ingest(mk(0, MgmtType::Deauth, 100));
  WL_CHECK(v.flag == DeauthFlag::Flood);
}

// ---- reset() forgets everything; a fresh burst starts from baseline again. --
WL_TEST(deauth_reset_clears_state) {
  DeauthFloodDetector d;
  for (int i = 0; i < 50; ++i) d.ingest(mk(9, MgmtType::Deauth, 100));
  WL_CHECK(d.tracked() >= (size_t)1);
  d.reset();
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  WL_CHECK(d.global_flag() == DeauthFlag::None);
  WL_CHECK(d.ingest(mk(9, MgmtType::Deauth, 200)).flag == DeauthFlag::None);
}
