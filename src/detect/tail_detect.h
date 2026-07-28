// tail_detect.h - pure, host-testable anti-stalking / tail DETECTION logic.
//
// DEFENSIVE ONLY. This module never transmits, deauthenticates, tracks, or
// attacks anything. It ingests a stream of sightings of nearby wireless devices
// (each already reduced by the caller to a stable-ish hashed id) and decides
// which of those devices are plausibly FOLLOWING the wearer across time and
// place, versus benign familiar devices (a home/work router the wearer passes
// every day). It classifies observations; it produces no radio output.
//
// It is a pure port of the tradecraft in the reference Threat Radar engine
// (threat_radar.cpp): a device that re-appears at MANY DISTINCT locations as the
// wearer moves is co-moving = following, while a device pinned to ONE location
// is a fixture and is ignored. Threat Radar expressed "distinct location" as GPS
// waypoints >=120 m apart plus a metric span; here the caller supplies a coarse
// integer location bucket (cell_id), so "distinct cells" replace the metric
// waypoint/span pair. Everything else (the 2/3/4-location, 5/10/18-minute
// escalation ladder, upward-latching level, evict-least-interesting when full)
// is carried over unchanged. See tail_detect.cpp for the per-rule provenance.
//
// Self-contained by design: it defines its own sighting struct, includes only
// standard headers, and uses integer math only. No Arduino.h, no LVGL, no
// ESP-IDF, no GPS/clock calls, no dynamic allocation. Time and the wearer's
// coarse location arrive as plain values on ingest(), so the whole decision is
// deterministic and unit-testable on the host. Mapping a real BLE/WiFi scan
// onto DeviceSighting (fold the rotating MAC to an id, quantize the GPS fix to a
// cell) lives at the device-only call site, not here.
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// One sighting of a nearby device, as the on-device scan path would produce it.
// device_id: caller-folded stable-ish id for the (possibly rotating) MAC.
// t_sec:     monotonic seconds, caller-supplied (never read from a clock here).
// cell_id:   coarse location bucket the wearer was in, caller-supplied;
//            -1 == unknown/no fix (the sighting still counts for time + recency
//            but contributes no new distinct-cell evidence).
// rssi:      dBm; carried for a proximity hint, not used in the follow decision.
struct DeviceSighting {
  uint32_t device_id;
  uint32_t t_sec;
  int32_t  cell_id;
  int8_t   rssi;
};

// Confidence that a device is FOLLOWING the wearer. Mirrors Threat Radar's
// TrLevel ladder (None/Possible/Likely/Confirmed) with an added, explicit
// Familiar verdict for the learned-benign case. Familiar is NOT a point on the
// tail ladder: it is a separate state a device settles into while it is seen
// over and over in a single cell. Cross-cell movement revokes Familiar so a
// device first observed at home cannot remain permanently exempt while following.
enum class TailFlag : uint8_t {
  None = 0,       // brand-new / single sighting / not enough evidence yet
  Familiar,       // learned benign: repeatedly seen in ONE cell (home/work)
  Watching,       // early cross-cell evidence (>= 2 cells over time)
  PossibleTail,   // sustained cross-cell co-movement (>= 3 cells)
  ConfirmedTail,  // long, wide cross-cell co-movement (>= 4 cells)
};

// The verdict for a single ingest() call, with the evidence behind it so a
// caller can alert/log without re-deriving anything.
struct TailVerdict {
  TailFlag flag;
  uint8_t  distinct_cells;    // distinct location buckets this device was seen in
  uint16_t span_sec_over_60;  // (last_seen - first_seen) in whole minutes (sec/60)
};

// Stateful tail classifier. Fixed-size internal tables, no dynamic allocation.
// A single long-lived instance is fed every confirmed sighting.
class TailDetector {
 public:
  // Bounded tracked-device table. When full, ingest() evicts the least
  // interesting device (lowest follow-level, then oldest last-seen) so a real
  // tail's evidence survives while idle passers-by are recycled - no crash, no
  // corruption. Exposed for the host tests' table-full case.
  static const uint8_t kMaxDevices = 32;
  // Bounded per-device location memory. distinct_cells saturates here; since the
  // cap (8) already exceeds the ConfirmedTail threshold, a device that has
  // filled it is provably a tail already, so refusing further cells loses no
  // decision - it only stops the count climbing past the cap.
  static const uint8_t kMaxCellsPerDevice = 8;

  // --- Escalation ladder (ported 1:1 from threat_radar.cpp score_level, which
  // used 2/3/4 GPS waypoints and 5/10/18 minutes; distinct cells here stand in
  // for the metric waypoint+span pair). Both axes must be met together, so a
  // device has to genuinely ride along across places AND persist over time. ---
  static const uint8_t  kCellsWatching   = 2;
  static const uint8_t  kCellsPossible   = 3;
  static const uint8_t  kCellsConfirmed  = 4;
  static const uint32_t kSpanWatchingSec  = 5u  * 60;
  static const uint32_t kSpanPossibleSec  = 10u * 60;
  static const uint32_t kSpanConfirmedSec = 18u * 60;

  // --- Familiarity (learned-benign). DESIGNED for this pure module: the
  // reference learned "your own daily gear" from multi-day co-movement persisted
  // to SD (counter_tail: co-moved on >= 2 distinct calendar days). A pure,
  // clockless module has no notion of a calendar day, so we instead implement
  // the other half the task calls out: a device seen many times and pinned to a
  // SINGLE cell is a stationary fixture at a place the wearer frequents (home /
  // work AP) and is benign. Familiar remains active only while the device stays
  // in that one cell; cross-cell evidence revokes it and enables the tail ladder.
  // ---
  static const uint16_t kFamiliarMinHits  = 5;   // sightings before we trust it
  static const uint8_t  kFamiliarMaxCells = 1;   // ... all in one cell

  // --- Decay windows (used by decay()). DESIGNED: the reference did not mutate
  // a stored level down; it latched upward and applied a staleness window at
  // READ time. The task's API asks for an explicit decay() that both relaxes a
  // device that stopped following and frees table space, so we provide it. ---
  static const uint32_t kRelaxSec = 15u * 60;   // inactive this long -> drop to None
  static const uint32_t kEvictSec = 60u * 60;   // stale this long -> free the slot

  TailDetector() { reset(); }

  // Ingest one sighting; return the verdict for THIS device now. A first-ever
  // sighting only establishes a track and returns {None,...} - normal discovery
  // is never a tail; only accumulated cross-cell, cross-time evidence escalates.
  TailVerdict ingest(const DeviceSighting& s);

  // Age the store against a caller-supplied "now": relax devices unseen for
  // kRelaxSec back down to None, and free slots for devices unseen for
  // kEvictSec. A stationary Familiar device's learned-benign status is preserved
  // through a relax (cross-cell movement or a full evict clears it).
  void decay(uint32_t now_sec);

  // Forget every tracked device.
  void reset();

  // How many devices are currently tracked.
  size_t tracked() const;

 private:
  struct Cell {
    int32_t  id;
    uint16_t hits;
  };
  struct Device {
    bool     in_use;
    uint32_t id;
    uint32_t first_sec;
    uint32_t last_sec;
    uint16_t hits;                     // total sightings folded in
    int8_t   best_rssi;                // strongest (closest) sample
    uint8_t  tail_level;               // 0..3 ladder rank (see ladder_flag)
    bool     familiar;                 // learned benign while still single-cell
    uint8_t  ncells;                   // distinct cells recorded (<= kMaxCellsPerDevice)
    Cell     cells[kMaxCellsPerDevice];
  };

  Device devices_[kMaxDevices];

  Device* find_device(uint32_t id);
  Device* alloc_device(uint32_t id, uint32_t now_sec);
  // Record a cell hit; returns the device's distinct-cell count afterward.
  static uint8_t note_cell(Device* d, int32_t cell_id);
  // Map the 0..3 internal rank to the ladder TailFlag (never returns Familiar).
  static TailFlag ladder_flag(uint8_t level);
};

}  // namespace detect
