// deauth_flood.cpp - implementation of the pure deauth/disassoc flood detector.
//
// DEFENSIVE detection only: it classifies observed management-frame events and
// transmits nothing. See deauth_flood.h for the on-device grounding and the
// rationale behind every threshold. The mechanics below are intentionally
// simple: a bucketed one-second sliding window per BSSID (and one global window),
// integer math throughout, fixed-size tables, no clock, no allocation.
#include "deauth_flood.h"

#include <cstring>

namespace detect {

// ---- sliding-window primitives ---------------------------------------------

void DeauthFloodDetector::win_reset(Window& w) {
  for (uint8_t i = 0; i < kBuckets; ++i) {
    w.b[i].sec   = 0;
    w.b[i].count = 0;
  }
}

// Record one frame at absolute second `sec`. The bucket for that second is
// (sec % kBuckets); if it currently holds a different (older) second it is
// reused, which is exactly how a frame from kWindowSec seconds ago is dropped.
void DeauthFloodDetector::win_add(Window& w, uint32_t sec) {
  Bucket& bk = w.b[sec % kBuckets];
  if (bk.sec != sec) {
    bk.sec   = sec;
    bk.count = 0;
  }
  if (bk.count < 0xFFFF) bk.count++;
}

// Sum the frames whose second is within the last kWindowSec seconds of now.
// A bucket in the future (sec > now) or aged out (now - sec >= kWindowSec) is
// ignored. Saturates at uint16 max.
uint16_t DeauthFloodDetector::win_count(const Window& w, uint32_t now_sec) {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kBuckets; ++i) {
    const Bucket& bk = w.b[i];
    if (bk.count == 0) continue;
    if (bk.sec <= now_sec && (now_sec - bk.sec) < kWindowSec) total += bk.count;
  }
  if (total > 0xFFFF) total = 0xFFFF;
  return (uint16_t)total;
}

DeauthFlag DeauthFloodDetector::classify(uint16_t count, uint16_t elevated,
                                         uint16_t flood) {
  if (count >= flood) return DeauthFlag::Flood;
  if (count >= elevated) return DeauthFlag::Elevated;
  return DeauthFlag::None;
}

// ---- per-BSSID table --------------------------------------------------------

DeauthFloodDetector::BssidEntry* DeauthFloodDetector::find(const uint8_t bssid[6]) {
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (bssids_[i].used && memcmp(bssids_[i].bssid, bssid, 6) == 0) {
      return &bssids_[i];
    }
  }
  return nullptr;
}

DeauthFloodDetector::BssidEntry* DeauthFloodDetector::alloc(const uint8_t bssid[6],
                                                            uint32_t now_sec) {
  // Prefer a free slot.
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (!bssids_[i].used) {
      bssids_[i].used = true;
      memcpy(bssids_[i].bssid, bssid, 6);
      win_reset(bssids_[i].win);
      return &bssids_[i];
    }
  }
  // Table full: evict the least-active BSSID (smallest in-window count) so the
  // active attacker, which has the highest count, is never the one recycled.
  BssidEntry* victim  = &bssids_[0];
  uint16_t    least   = win_count(bssids_[0].win, now_sec);
  for (uint8_t i = 1; i < kMaxBssids; ++i) {
    uint16_t c = win_count(bssids_[i].win, now_sec);
    if (c < least) {
      least  = c;
      victim = &bssids_[i];
    }
  }
  victim->used = true;
  memcpy(victim->bssid, bssid, 6);
  win_reset(victim->win);
  return victim;
}

// ---- public API -------------------------------------------------------------

DeauthVerdict DeauthFloodDetector::ingest(const MgmtFrameEvent& e) {
  // Only client-disconnect frames are evidence; everything else is inert.
  if (e.type == MgmtType::Other) {
    DeauthVerdict v;
    v.flag         = DeauthFlag::None;
    v.rate_per_min = 0;
    return v;
  }

  if (e.t_sec >= now_) now_ = e.t_sec;   // never rewind the clock

  win_add(global_, e.t_sec);

  BssidEntry* en = find(e.bssid);
  if (!en) en = alloc(e.bssid, e.t_sec);
  win_add(en->win, e.t_sec);

  uint16_t count = win_count(en->win, e.t_sec);
  uint32_t rate  = (uint32_t)count * 60u / kWindowSec;
  if (rate > 0xFFFF) rate = 0xFFFF;

  DeauthVerdict v;
  v.flag         = classify(count, kElevatedPerWin, kFloodPerWin);
  v.rate_per_min = (uint16_t)rate;
  return v;
}

void DeauthFloodDetector::tick(uint32_t now_sec) {
  if (now_sec >= now_) now_ = now_sec;
  // Drop any BSSID whose window has emptied out: a stopped attack relaxes to
  // None and its slot is freed for future contacts.
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (bssids_[i].used && win_count(bssids_[i].win, now_) == 0) {
      bssids_[i].used = false;
      win_reset(bssids_[i].win);
    }
  }
}

void DeauthFloodDetector::reset() {
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    bssids_[i].used = false;
    memset(bssids_[i].bssid, 0, 6);
    win_reset(bssids_[i].win);
  }
  win_reset(global_);
  now_ = 0;
}

size_t DeauthFloodDetector::tracked() const {
  size_t n = 0;
  for (uint8_t i = 0; i < kMaxBssids; ++i) {
    if (bssids_[i].used) ++n;
  }
  return n;
}

uint16_t DeauthFloodDetector::global_rate_per_min() const {
  uint32_t rate = (uint32_t)win_count(global_, now_) * 60u / kWindowSec;
  if (rate > 0xFFFF) rate = 0xFFFF;
  return (uint16_t)rate;
}

DeauthFlag DeauthFloodDetector::global_flag() const {
  return classify(win_count(global_, now_), kGlobalElevatedPerWin,
                  kGlobalFloodPerWin);
}

}  // namespace detect
