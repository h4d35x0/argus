// test_tail_detect.cpp - host unit tests for the pure anti-stalking / tail
// classifier (src/detect/tail_detect). DEFENSIVE detection only: these exercise
// the escalation ladder, single-cell familiarity learning, decay/relax/evict,
// no-cross-contamination, and graceful table-full behavior the module exists to
// provide. Time and location are plain inputs - no clock, no GPS, no hardware.
#include "wl_test.h"
#include "tail_detect.h"

#include <cstdint>

using namespace detect;

// Build a sighting. rssi is carried for realism; it is not used in the decision.
static DeviceSighting mk(uint32_t id, uint32_t t_sec, int32_t cell, int8_t rssi = -60) {
  DeviceSighting s;
  s.device_id = id;
  s.t_sec     = t_sec;
  s.cell_id   = cell;
  s.rssi      = rssi;
  return s;
}

// ---- A brand-new device seen once is never a tail (no paranoia on passers). --
WL_TEST(tail_single_sighting_is_none) {
  TailDetector d;
  TailVerdict v = d.ingest(mk(1, 0, 100));
  WL_CHECK(v.flag == TailFlag::None);
  WL_CHECK_EQ(v.distinct_cells, (uint8_t)1);
  WL_CHECK_EQ(v.span_sec_over_60, (uint16_t)0);
  WL_CHECK_EQ(d.tracked(), (size_t)1);
}

// ---- A couple of quick same-cell sightings stays None (below thresholds). ----
WL_TEST(tail_baseline_not_paranoid) {
  TailDetector d;
  WL_CHECK(d.ingest(mk(1, 0,  100)).flag == TailFlag::None);
  WL_CHECK(d.ingest(mk(1, 30, 100)).flag == TailFlag::None);  // 1 cell, 30s
}

// ---- Many sightings in ONE cell -> Familiar, and it NEVER escalates to tail. -
WL_TEST(tail_one_cell_becomes_familiar_never_tail) {
  TailDetector d;
  // Below kFamiliarMinHits (5): still None.
  WL_CHECK(d.ingest(mk(7, 0,   500)).flag == TailFlag::None);
  WL_CHECK(d.ingest(mk(7, 60,  500)).flag == TailFlag::None);
  WL_CHECK(d.ingest(mk(7, 120, 500)).flag == TailFlag::None);
  WL_CHECK(d.ingest(mk(7, 180, 500)).flag == TailFlag::None);
  // 5th sighting, all in cell 500 -> learned benign.
  TailVerdict v = d.ingest(mk(7, 240, 500));
  WL_CHECK(v.flag == TailFlag::Familiar);
  WL_CHECK_EQ(v.distinct_cells, (uint8_t)1);

  // Now it moves across many cells over a long time - tail-like geometry - but
  // a Familiar device is latched benign and must NOT escalate.
  WL_CHECK(d.ingest(mk(7, 1000, 600)).flag == TailFlag::Familiar);
  WL_CHECK(d.ingest(mk(7, 2000, 700)).flag == TailFlag::Familiar);
  WL_CHECK(d.ingest(mk(7, 3000, 800)).flag == TailFlag::Familiar);  // 4 cells, >18min
}

// ---- A device seen many times but across TWO cells is NOT familiar. ----------
WL_TEST(tail_two_cells_is_not_familiar) {
  TailDetector d;
  d.ingest(mk(3, 0,   100));
  d.ingest(mk(3, 10,  200));   // second distinct cell early -> disqualifies familiar
  d.ingest(mk(3, 20,  100));
  d.ingest(mk(3, 30,  200));
  TailVerdict v = d.ingest(mk(3, 40, 100));   // 5 hits but 2 cells
  WL_CHECK(v.flag != TailFlag::Familiar);
  // span only 40s -> still below Watching's 5-min gate, so None here.
  WL_CHECK(v.flag == TailFlag::None);
  WL_CHECK_EQ(v.distinct_cells, (uint8_t)2);
}

// ---- A follower across distinct cells over time escalates step by step. ------
WL_TEST(tail_follower_escalates_through_ladder) {
  TailDetector d;
  WL_CHECK(d.ingest(mk(9, 0,    1)).flag == TailFlag::None);          // 1 cell
  WL_CHECK(d.ingest(mk(9, 310,  2)).flag == TailFlag::Watching);      // 2 cells, >5min
  WL_CHECK(d.ingest(mk(9, 620,  3)).flag == TailFlag::PossibleTail);  // 3 cells, >10min
  TailVerdict v = d.ingest(mk(9, 1100, 4));                          // 4 cells, >18min
  WL_CHECK(v.flag == TailFlag::ConfirmedTail);
  WL_CHECK_EQ(v.distinct_cells, (uint8_t)4);
  WL_CHECK(v.span_sec_over_60 >= (uint16_t)18);
}

// ---- Level latches upward: revisiting an old cell does not de-escalate. ------
WL_TEST(tail_level_latches_upward) {
  TailDetector d;
  d.ingest(mk(9, 0,    1));
  d.ingest(mk(9, 310,  2));
  d.ingest(mk(9, 620,  3));
  d.ingest(mk(9, 1100, 4));   // ConfirmedTail
  // Re-seen back in an earlier cell: no new distinct cell, must stay Confirmed.
  TailVerdict v = d.ingest(mk(9, 1200, 1));
  WL_CHECK(v.flag == TailFlag::ConfirmedTail);
}

// ---- A brief passer-by (<=2 cells) never reaches ConfirmedTail, then ages out.
WL_TEST(tail_passerby_never_confirmed_then_evicted) {
  TailDetector d;
  WL_CHECK(d.ingest(mk(5, 0,   10)).flag == TailFlag::None);
  TailVerdict v = d.ingest(mk(5, 310, 11));   // 2 cells, >5min
  WL_CHECK(v.flag == TailFlag::Watching);
  WL_CHECK(v.flag != TailFlag::ConfirmedTail);
  WL_CHECK(v.flag != TailFlag::PossibleTail);
  WL_CHECK_EQ(d.tracked(), (size_t)1);

  // Gone for longer than the evict window -> slot is freed (decays to nothing).
  d.decay(310 + TailDetector::kEvictSec);
  WL_CHECK_EQ(d.tracked(), (size_t)0);
}

// ---- decay(): relax keeps the slot mid-window; evict frees a long-stale one. --
WL_TEST(tail_decay_relaxes_and_frees_slots) {
  TailDetector d;
  // Device A last seen at t=4000 (recently, relative to the decay clock).
  d.ingest(mk(1, 0,    10));
  d.ingest(mk(1, 4000, 11));   // Watching, last_sec = 4000
  // Device B last seen at t=1000 (stale).
  d.ingest(mk(2, 900,  20));
  d.ingest(mk(2, 1000, 21));   // last_sec = 1000
  WL_CHECK_EQ(d.tracked(), (size_t)2);

  // now = 5000: A gap = 1000 (>= relax, < evict) -> relaxed but KEPT.
  //             B gap = 4000 (>= evict)          -> freed.
  d.decay(5000);
  WL_CHECK_EQ(d.tracked(), (size_t)1);   // only the stale device B was freed
}

// ---- Two independent devices do not cross-contaminate each other's state. ----
WL_TEST(tail_two_devices_independent) {
  TailDetector d;
  // Device 100 becomes Familiar (5 hits, one cell).
  for (uint32_t i = 0; i < 5; ++i) d.ingest(mk(100, i * 60, 42));
  // Device 200 becomes a Confirmed tail (4 cells over time).
  d.ingest(mk(200, 0,    1));
  d.ingest(mk(200, 310,  2));
  d.ingest(mk(200, 620,  3));
  WL_CHECK(d.ingest(mk(200, 1100, 4)).flag == TailFlag::ConfirmedTail);
  // The confirmed tail did not disturb the familiar device.
  WL_CHECK(d.ingest(mk(100, 5000, 42)).flag == TailFlag::Familiar);
  WL_CHECK_EQ(d.tracked(), (size_t)2);
}

// ---- Unknown cell (cell_id = -1) contributes no location evidence. -----------
WL_TEST(tail_unknown_cell_no_false_evidence) {
  TailDetector d;
  // Seen many times over a long span but always with an unknown location.
  for (uint32_t i = 0; i < 6; ++i) d.ingest(mk(4, i * 300, -1));
  TailVerdict v = d.ingest(mk(4, 2000, -1));
  WL_CHECK_EQ(v.distinct_cells, (uint8_t)0);
  // No cells -> cannot be a tail (needs >= 2) and cannot be familiar (needs 1).
  WL_CHECK(v.flag == TailFlag::None);
}

// ---- reset() forgets everything; the same device is a baseline again. --------
WL_TEST(tail_reset_clears_state) {
  TailDetector d;
  d.ingest(mk(9, 0,    1));
  d.ingest(mk(9, 310,  2));
  d.ingest(mk(9, 620,  3));
  d.ingest(mk(9, 1100, 4));   // ConfirmedTail
  WL_CHECK(d.tracked() >= (size_t)1);
  d.reset();
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  // Fresh again: a single sighting is once more just None.
  WL_CHECK(d.ingest(mk(9, 2000, 1)).flag == TailFlag::None);
}

// ---- Table full: no crash, count capped, and a confirmed tail is NOT evicted.-
WL_TEST(tail_table_full_degrades_gracefully) {
  TailDetector d;
  // Device 0 climbs to ConfirmedTail - the most interesting contact.
  d.ingest(mk(0, 0,    1));
  d.ingest(mk(0, 310,  2));
  d.ingest(mk(0, 620,  3));
  WL_CHECK(d.ingest(mk(0, 1100, 4)).flag == TailFlag::ConfirmedTail);

  // Fill the rest of the table with plain (None) one-off devices.
  for (uint32_t id = 1; id < TailDetector::kMaxDevices; ++id) {
    d.ingest(mk(id, 2000, 500));
  }
  WL_CHECK_EQ(d.tracked(), (size_t)TailDetector::kMaxDevices);

  // Flood many more brand-new devices. Each forces an eviction of a least-
  // interesting (None) device; must not crash and the count stays capped.
  for (uint32_t id = 1000; id < 1050; ++id) {
    TailVerdict v = d.ingest(mk(id, 3000, 700));
    WL_CHECK(v.flag == TailFlag::None);          // fresh devices classify cleanly
  }
  WL_CHECK_EQ(d.tracked(), (size_t)TailDetector::kMaxDevices);

  // Device 0 (ConfirmedTail) must have survived the flood, not been recycled.
  // If it had been evicted and re-allocated, this lone sighting would read as a
  // fresh None; instead its accrued geometry keeps it Confirmed.
  WL_CHECK(d.ingest(mk(0, 4000, 1)).flag == TailFlag::ConfirmedTail);
}

// ---- Per-device cell memory saturates without corrupting the verdict. --------
WL_TEST(tail_per_device_cells_saturate_safely) {
  TailDetector d;
  // Walk through more distinct cells than kMaxCellsPerDevice.
  uint32_t t = 0;
  for (int32_t cell = 0; cell < TailDetector::kMaxCellsPerDevice + 5; ++cell) {
    t += 310;
    d.ingest(mk(77, t, cell));
  }
  // distinct_cells saturates at the cap; still (well past) Confirmed, no crash.
  TailVerdict v = d.ingest(mk(77, t + 310, 999));
  WL_CHECK_EQ(v.distinct_cells, (uint8_t)TailDetector::kMaxCellsPerDevice);
  WL_CHECK(v.flag == TailFlag::ConfirmedTail);
}
