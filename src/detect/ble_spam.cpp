// ble_spam.cpp - implementation of the pure BLE advertisement-spam detector.
//
// DEFENSIVE detection only: it classifies observed BLE advertisements and
// transmits nothing. See ble_spam.h for the on-device grounding and the
// rationale behind every threshold. It COMPOSES with the shared BLE AD parser
// (adv_parser.h) rather than re-walking raw advertising bytes by hand: the only
// byte inspection here is delegated to that parser. The mechanics are otherwise
// intentionally simple: a bounded table of recently-seen advertiser addresses,
// each stamped with its last spam-pattern advert time, a fixed-length sliding
// window, integer math throughout, no clock, no allocation.
#include "ble_spam.h"

// Relative path so this resolves in BOTH the firmware build (PlatformIO only
// puts src/ on the include path, not src/ble/) and the host test harness. The
// host Makefile/run.sh also add -I src/ble, but the relative form needs no flag.
#include "../ble/adv_parser.h"

#include <cstring>

namespace detect {

// ---- spam-pattern gate ------------------------------------------------------

bool BleSpamDetector::is_spam_pattern(const uint8_t* adv, uint8_t adv_len) {
  if (!adv || adv_len == 0) return false;

  // Manufacturer-specific data whose company id is one of the vendors whose
  // proximity-pairing beacons BLE-spam tools spoof.
  uint16_t company = 0;
  if (ble::adv_manufacturer_company_id(adv, adv_len, &company)) {
    if (company == kCompanyApple || company == kCompanyMicrosoft ||
        company == kCompanySamsung || company == kCompanyGoogle) {
      return true;
    }
  }

  // Google Fast Pair. Tools broadcast it as 16-bit service data under the Fast
  // Pair UUID; some carry it only in the service-UUID list, so check both.
  if (ble::adv_find_service_data16(adv, adv_len, kFastPairUuid, nullptr, nullptr))
    return true;
  if (ble::adv_has_service_uuid16(adv, adv_len, kFastPairUuid))
    return true;

  return false;
}

// ---- sliding window over distinct addresses ---------------------------------

uint16_t BleSpamDetector::count_in_window(uint32_t now_sec) const {
  uint32_t n = 0;
  for (uint8_t i = 0; i < kMaxAddrs; ++i) {
    if (!addrs_[i].used) continue;
    // In-window iff seen at or before now and not older than kWindowSec.
    if (addrs_[i].last_seen <= now_sec &&
        (now_sec - addrs_[i].last_seen) < kWindowSec) {
      ++n;
    }
  }
  if (n > 0xFFFF) n = 0xFFFF;
  return (uint16_t)n;
}

SpamFlag BleSpamDetector::classify(uint16_t distinct) {
  if (distinct >= kSpamDistinct) return SpamFlag::Spam;
  if (distinct >= kElevatedDistinct) return SpamFlag::Elevated;
  return SpamFlag::None;
}

// ---- address table ----------------------------------------------------------

BleSpamDetector::Entry* BleSpamDetector::find(const uint8_t addr[6]) {
  for (uint8_t i = 0; i < kMaxAddrs; ++i) {
    if (addrs_[i].used && memcmp(addrs_[i].addr, addr, 6) == 0) {
      return &addrs_[i];
    }
  }
  return nullptr;
}

BleSpamDetector::Entry* BleSpamDetector::alloc(const uint8_t addr[6],
                                               uint32_t now_sec) {
  // Prefer a free slot, or reclaim one that has already aged out of the window
  // (its last_seen is stale, so it no longer counts as a distinct advertiser).
  for (uint8_t i = 0; i < kMaxAddrs; ++i) {
    if (!addrs_[i].used ||
        !(addrs_[i].last_seen <= now_sec &&
          (now_sec - addrs_[i].last_seen) < kWindowSec)) {
      addrs_[i].used = true;
      memcpy(addrs_[i].addr, addr, 6);
      addrs_[i].last_seen = now_sec;
      return &addrs_[i];
    }
  }
  // Table full of in-window advertisers: this is itself unambiguous spam (>=
  // kMaxAddrs distinct spam-pattern addresses at once). Evict the LEAST-RECENTLY-
  // SEEN entry so the window keeps tracking the freshest attack traffic; the
  // count simply saturates at the cap. Never crashes, never corrupts.
  Entry*   victim = &addrs_[0];
  uint32_t oldest = addrs_[0].last_seen;
  for (uint8_t i = 1; i < kMaxAddrs; ++i) {
    if (addrs_[i].last_seen < oldest) {
      oldest = addrs_[i].last_seen;
      victim = &addrs_[i];
    }
  }
  victim->used      = true;
  memcpy(victim->addr, addr, 6);
  victim->last_seen = now_sec;
  return victim;
}

// ---- public API -------------------------------------------------------------

SpamVerdict BleSpamDetector::ingest(const BleAdvObservation& o) {
  // Only spam-pattern adverts are evidence; everything else is inert and does
  // not touch the table (so a benign flood of non-watch-listed devices cannot
  // trip the gate, nor evict real spam evidence).
  if (!is_spam_pattern(o.adv_data, o.adv_len)) {
    SpamVerdict v;
    v.flag                   = classify(count_in_window(now_));
    v.distinct_addrs_per_win = count_in_window(now_);
    return v;
  }

  if (o.t_sec >= now_) now_ = o.t_sec;   // never rewind the clock

  // Dedup: the same address re-advertising just refreshes its last_seen; it is
  // NOT counted as a new distinct device.
  Entry* en = find(o.addr);
  if (!en) en = alloc(o.addr, o.t_sec);
  if (o.t_sec >= en->last_seen) en->last_seen = o.t_sec;

  uint16_t distinct = count_in_window(now_);
  SpamVerdict v;
  v.flag                   = classify(distinct);
  v.distinct_addrs_per_win = distinct;
  return v;
}

void BleSpamDetector::tick(uint32_t now_sec) {
  if (now_sec >= now_) now_ = now_sec;
  // Drop any address that has aged out of the window: a stopped flood relaxes to
  // None and its slot is freed for future contacts.
  for (uint8_t i = 0; i < kMaxAddrs; ++i) {
    if (addrs_[i].used &&
        !(addrs_[i].last_seen <= now_ && (now_ - addrs_[i].last_seen) < kWindowSec)) {
      addrs_[i].used      = false;
      addrs_[i].last_seen = 0;
      memset(addrs_[i].addr, 0, 6);
    }
  }
}

void BleSpamDetector::reset() {
  for (uint8_t i = 0; i < kMaxAddrs; ++i) {
    addrs_[i].used      = false;
    addrs_[i].last_seen = 0;
    memset(addrs_[i].addr, 0, 6);
  }
  now_ = 0;
}

size_t BleSpamDetector::tracked() const {
  return (size_t)count_in_window(now_);
}

SpamFlag BleSpamDetector::flag() const {
  return classify(count_in_window(now_));
}

uint16_t BleSpamDetector::distinct() const {
  return count_in_window(now_);
}

}  // namespace detect
