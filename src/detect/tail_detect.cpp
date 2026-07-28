// tail_detect.cpp - implementation of the pure anti-stalking / tail classifier.
//
// Provenance of each rule (CONFIRMED = ported from the reference Threat Radar
// engine at threat-radar-ref/src/{threat_radar,counter_tail}.cpp; DESIGNED =
// added here for the pure, clockless, cell-based host module):
//
//   CONFIRMED  Distinct-location count is the backbone of the follow score. A
//              sighting only adds evidence when it is at a NEW location; a
//              device pinned to one place is a fixture and never escalates.
//              (threat_radar.cpp: distinct waypoints >=120 m apart.)
//   CONFIRMED  Three-tier ladder with both axes required together: 2/3/4
//              distinct locations AND 5/10/18 minutes of persistence.
//              (threat_radar.cpp score_level, thresholds carried over exactly.)
//   CONFIRMED  Level latches upward; recency is handled separately.
//              (threat_radar.cpp: "if (lvl > c->level) c->level = lvl".)
//   CONFIRMED  When the device table is full, evict the least interesting one
//              (lowest level, then oldest last-seen) so a tail's evidence is
//              kept and idle noise recycled. (threat_radar.cpp find_or_alloc.)
//   CONFIRMED  Familiar devices (your own daily gear) are benign and must not
//              raise a tail alarm. (counter_tail.cpp: is_familiar suppresses.)
//   DESIGNED   "Distinct cells" replace GPS waypoints + metric span: the caller
//              supplies a coarse integer location bucket, keeping the module
//              integer-only and clockless. A per-metre distance test is not
//              possible (and not needed) with coarse buckets.
//   DESIGNED   Familiarity is LEARNED from single-cell concentration (seen
//              kFamiliarMinHits times, all in one cell) rather than the
//              reference's multi-calendar-day co-movement, because a pure
//              clockless module cannot know the date. It captures the same
//              intent the task calls out: a device predominantly in ONE cell
//              (home/work) is benign. Cross-cell movement revokes this local
//              familiarity so an initially stationary device cannot receive a
//              permanent exemption from the tail ladder.
//   DESIGNED   decay() actively relaxes a stopped tail to None and frees stale
//              slots. The reference latched upward and applied staleness only at
//              read time; the task's API asks for an explicit decay(), so it is
//              provided here.
#include "tail_detect.h"

#include <cstring>

namespace detect {

// 0..3 internal rank -> ladder flag. Never yields Familiar (that is a separate
// terminal state applied before the ladder is consulted).
TailFlag TailDetector::ladder_flag(uint8_t level) {
  switch (level) {
    case 3: return TailFlag::ConfirmedTail;
    case 2: return TailFlag::PossibleTail;
    case 1: return TailFlag::Watching;
    default: return TailFlag::None;
  }
}

TailDetector::Device* TailDetector::find_device(uint32_t id) {
  for (uint8_t i = 0; i < kMaxDevices; ++i) {
    if (devices_[i].in_use && devices_[i].id == id) return &devices_[i];
  }
  return nullptr;
}

// Allocate a slot for a new device: reuse a free slot if there is one, else
// evict the least interesting existing device. "Least interesting" == lowest
// follow-level, ties broken by oldest last-seen. A Familiar device counts as
// benign (level 0) and so is a preferred eviction victim over any live tail.
TailDetector::Device* TailDetector::alloc_device(uint32_t id, uint32_t now_sec) {
  int free_idx = -1;
  for (uint8_t i = 0; i < kMaxDevices; ++i) {
    if (!devices_[i].in_use) { free_idx = i; break; }
  }
  int idx = free_idx;
  if (idx < 0) {
    int victim = 0;
    for (uint8_t i = 1; i < kMaxDevices; ++i) {
      const Device& a = devices_[victim];
      const Device& b = devices_[i];
      // familiar -> treat as level 0 (benign) for eviction ranking.
      uint8_t la = a.familiar ? 0 : a.tail_level;
      uint8_t lb = b.familiar ? 0 : b.tail_level;
      if (lb < la || (lb == la && b.last_sec < a.last_sec)) victim = i;
    }
    idx = victim;
  }
  Device& d = devices_[idx];
  std::memset(&d, 0, sizeof(d));
  d.in_use    = true;
  d.id        = id;
  d.first_sec = now_sec;
  d.last_sec  = now_sec;
  d.best_rssi = INT8_MIN;
  return &d;
}

// Record that the device was seen in cell_id. Bumps an existing cell's hit
// count, or adds a new distinct cell if there is room. Returns the distinct-cell
// count afterward. When the per-device cell table is full we stop adding new
// cells (the count saturates at kMaxCellsPerDevice) but still succeed - the
// device is already well past ConfirmedTail, so no decision is lost.
uint8_t TailDetector::note_cell(Device* d, int32_t cell_id) {
  for (uint8_t i = 0; i < d->ncells; ++i) {
    if (d->cells[i].id == cell_id) {
      if (d->cells[i].hits < 0xFFFF) d->cells[i].hits++;
      return d->ncells;
    }
  }
  if (d->ncells < kMaxCellsPerDevice) {
    d->cells[d->ncells].id   = cell_id;
    d->cells[d->ncells].hits = 1;
    d->ncells++;
  }
  return d->ncells;
}

TailVerdict TailDetector::ingest(const DeviceSighting& s) {
  Device* d = find_device(s.device_id);
  if (!d) d = alloc_device(s.device_id, s.t_sec);

  // Fold in time + signal. t_sec is caller-monotonic; guard last_sec so an
  // out-of-order sample cannot rewind the span.
  if (s.t_sec > d->last_sec) d->last_sec = s.t_sec;
  if (s.t_sec < d->first_sec) d->first_sec = s.t_sec;
  if (d->hits < 0xFFFF) d->hits++;
  if (s.rssi > d->best_rssi) d->best_rssi = s.rssi;

  // Location evidence only accrues from a known cell.
  if (s.cell_id >= 0) note_cell(d, s.cell_id);

  uint32_t span = (d->last_sec >= d->first_sec) ? (d->last_sec - d->first_sec) : 0;

  // Familiarity learned from one location is valid only while the device stays
  // there. Real BLE devices advertise repeatedly, so a tracker can easily reach
  // five hits before the wearer leaves the starting cell. Keeping Familiar
  // latched after a second cell would permanently hide exactly that real tail.
  if (d->familiar && d->ncells > kFamiliarMaxCells) d->familiar = false;

  TailFlag flag;
  if (d->familiar) {
    // Still pinned to the familiar location, so it remains benign.
    flag = TailFlag::Familiar;
  } else if (d->hits >= kFamiliarMinHits && d->ncells >= 1 &&
             d->ncells <= kFamiliarMaxCells) {
    // Learned benign: seen enough times, all in one cell -> a fixture the wearer
    // frequents (home/work). It stays quiet unless later seen in another cell.
    d->familiar = true;
    flag = TailFlag::Familiar;
  } else {
    // Tail ladder. Both the distinct-location and the time axis must clear each
    // tier together; the level only ever climbs (latch upward).
    uint8_t lvl = 0;
    if (d->ncells >= kCellsConfirmed && span >= kSpanConfirmedSec)      lvl = 3;
    else if (d->ncells >= kCellsPossible && span >= kSpanPossibleSec)   lvl = 2;
    else if (d->ncells >= kCellsWatching && span >= kSpanWatchingSec)   lvl = 1;
    if (lvl > d->tail_level) d->tail_level = lvl;
    flag = ladder_flag(d->tail_level);
  }

  TailVerdict v;
  v.flag           = flag;
  v.distinct_cells = d->ncells;
  uint32_t mins    = span / 60u;
  v.span_sec_over_60 = (mins > 0xFFFF) ? 0xFFFF : (uint16_t)mins;
  return v;
}

void TailDetector::decay(uint32_t now_sec) {
  for (uint8_t i = 0; i < kMaxDevices; ++i) {
    Device& d = devices_[i];
    if (!d.in_use) continue;
    uint32_t gap = (now_sec >= d.last_sec) ? (now_sec - d.last_sec) : 0;
    if (gap >= kEvictSec) {
      // Long-stale: free the slot entirely (forgets familiarity too).
      std::memset(&d, 0, sizeof(d));
    } else if (gap >= kRelaxSec) {
      // Inactive: a stopped tail relaxes back down to None. Keep the track and
      // any learned-familiar status so a returning device is remembered.
      d.tail_level = 0;
    }
  }
}

void TailDetector::reset() {
  std::memset(devices_, 0, sizeof(devices_));
}

size_t TailDetector::tracked() const {
  size_t n = 0;
  for (uint8_t i = 0; i < kMaxDevices; ++i) {
    if (devices_[i].in_use) ++n;
  }
  return n;
}

}  // namespace detect
