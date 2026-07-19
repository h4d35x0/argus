// beacon_flood.cpp - implementation of the pure WiFi beacon-flood detector.
//
// DEFENSIVE detection only: it classifies observed beacon frames and transmits
// nothing. See beacon_flood.h for the on-device grounding and the rationale
// behind every threshold. The mechanics are intentionally simple and mirror the
// BLE-spam analog (src/detect/ble_spam): a bounded table of recently-seen AP
// BSSIDs, each stamped with its last beacon time, a fixed-length sliding window,
// integer math throughout, no clock, no allocation.
#include "beacon_flood.h"

#include <cstring>

namespace detect {

// ---- sliding window over distinct BSSIDs ------------------------------------

uint16_t BeaconFloodDetector::count_in_window(uint32_t now_sec) const {
  uint32_t n = 0;
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (!bssids_[i].used) continue;
    // In-window iff seen at or before now and not older than kWindowSec.
    if (bssids_[i].last_seen <= now_sec &&
        (now_sec - bssids_[i].last_seen) < kWindowSec) {
      ++n;
    }
  }
  if (n > 0xFFFF) n = 0xFFFF;
  return (uint16_t)n;
}

BeaconFlag BeaconFloodDetector::classify(uint16_t distinct) {
  if (distinct >= kFloodDistinct) return BeaconFlag::Flood;
  if (distinct >= kElevatedDistinct) return BeaconFlag::Elevated;
  return BeaconFlag::None;
}

// ---- BSSID table ------------------------------------------------------------

BeaconFloodDetector::Entry* BeaconFloodDetector::find(const uint8_t bssid[6]) {
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (bssids_[i].used && memcmp(bssids_[i].bssid, bssid, 6) == 0) {
      return &bssids_[i];
    }
  }
  return nullptr;
}

BeaconFloodDetector::Entry* BeaconFloodDetector::alloc(const uint8_t bssid[6],
                                                       uint32_t now_sec) {
  // Prefer a free slot, or reclaim one that has already aged out of the window
  // (its last_seen is stale, so it no longer counts as a distinct AP).
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (!bssids_[i].used ||
        !(bssids_[i].last_seen <= now_sec &&
          (now_sec - bssids_[i].last_seen) < kWindowSec)) {
      bssids_[i].used = true;
      memcpy(bssids_[i].bssid, bssid, 6);
      bssids_[i].last_seen = now_sec;
      return &bssids_[i];
    }
  }
  // Table full of in-window BSSIDs: this is itself unambiguous flood (>=
  // kMaxBssids distinct APs at once). Evict the LEAST-RECENTLY-SEEN entry so the
  // window keeps tracking the freshest attack traffic; the count simply
  // saturates at the cap. Never crashes, never corrupts.
  Entry*   victim = &bssids_[0];
  uint32_t oldest = bssids_[0].last_seen;
  for (uint8_t i = 1; i < kMaxBssids; ++i) {
    if (bssids_[i].last_seen < oldest) {
      oldest = bssids_[i].last_seen;
      victim = &bssids_[i];
    }
  }
  victim->used      = true;
  memcpy(victim->bssid, bssid, 6);
  victim->last_seen = now_sec;
  return victim;
}

// ---- public API -------------------------------------------------------------

BeaconVerdict BeaconFloodDetector::ingest(const BeaconObservation& o) {
  if (o.t_sec >= now_) now_ = o.t_sec;   // never rewind the clock

  // Dedup: the same BSSID re-beaconing just refreshes its last_seen; it is NOT
  // counted as a new distinct AP (a legit AP beacons ~10x/sec).
  Entry* en = find(o.bssid);
  if (!en) en = alloc(o.bssid, o.t_sec);
  if (o.t_sec >= en->last_seen) en->last_seen = o.t_sec;

  uint16_t distinct = count_in_window(now_);
  BeaconVerdict v;
  v.flag                    = classify(distinct);
  v.distinct_bssids_per_win = distinct;
  return v;
}

void BeaconFloodDetector::tick(uint32_t now_sec) {
  if (now_sec >= now_) now_ = now_sec;
  // Drop any BSSID that has aged out of the window: a stopped flood relaxes to
  // None and its slot is freed for future contacts.
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (bssids_[i].used &&
        !(bssids_[i].last_seen <= now_ && (now_ - bssids_[i].last_seen) < kWindowSec)) {
      bssids_[i].used      = false;
      bssids_[i].last_seen = 0;
      memset(bssids_[i].bssid, 0, 6);
    }
  }
}

void BeaconFloodDetector::reset() {
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    bssids_[i].used      = false;
    bssids_[i].last_seen = 0;
    memset(bssids_[i].bssid, 0, 6);
  }
  now_ = 0;
}

size_t BeaconFloodDetector::tracked() const {
  return (size_t)count_in_window(now_);
}

BeaconFlag BeaconFloodDetector::flag() const {
  return classify(count_in_window(now_));
}

uint16_t BeaconFloodDetector::distinct() const {
  return count_in_window(now_);
}

}  // namespace detect
