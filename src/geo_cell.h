#pragma once
#include <cstdint>

// Pure GPS-to-coarse-cell quantizer. tail_detect (src/detect/tail_detect) counts
// how many DISTINCT location cells a device is seen across to decide a follow;
// it takes a caller-supplied int32 cell_id but nothing computes one. This turns
// a WGS84 lat/lon fix into that id.
//
// No Arduino / LVGL / hardware; only <cstdint> here and <cmath> in the .cpp, so
// it runs under the host test harness. The GNSS driver supplies lat/lon; this is
// a pure function of them.

namespace geo {

// Quantize (lat_deg, lon_deg) to a stable ~cell_m-metre cell id. Same physical
// cell -> same id; different cells -> (almost surely) different id. Equirect-
// angular grid with longitude spacing scaled by cos(lat) so cells stay roughly
// square away from the poles. The default 120 m matches the waypoint spacing the
// tail detector was tuned to in threat-radar-ref. NOT a reversible geocode: the
// grid indices are hashed into int32, so it is for equality/distinct-count only
// (tail_detect's exact need), not for recovering coordinates. Collisions across
// the handful of cells one tail path traverses are negligible (~1e-8).
//
// ALWAYS returns a NON-NEGATIVE id (31-bit hash). tail_detect reserves a
// negative cell_id as its "unknown location" sentinel (-1), so a negative cell
// would be silently dropped; keeping this >= 0 makes the two compose correctly.
int32_t coarse_cell(double lat_deg, double lon_deg, double cell_m = 120.0);

// Decimal places to use when writing a GPS fix into an SD detection log.
//
// The detector logs (/AirTag, /Flipper, /Skimmers, /EvilTwin, /Flock,
// /ThreatRadar) each pair a THIRD PARTY's MAC with the wearer's position at the
// moment of the sighting. At full precision that card becomes both a location
// dataset about other people's devices and a complete track of the wearer, which
// is a liability if the watch is lost, seized, or handed to a stranger.
//
// 3 places is roughly 110 m. That still shows a device was near you at distinct
// places over time, which is the entire claim a detection record makes, without
// pinning anyone to a spot. Use it as `f.printf("%.*f", geo::kGpsLogDecimals, lat)`.
//
// This is a LOGGING precision only. Detection maths (coarse_cell, waypoint
// spacing, span) keeps using the full-precision fix and is unaffected.
//
// Deliberately NOT applied to the wardriver's /Wardrive/*.csv: that is a WiGLE
// export whose whole purpose is a precise survey the wearer chose to collect.
static const int kGpsLogDecimals = 3;

// Stateful boundary hysteresis for a live GPS stream. A raw grid cell can flip
// when stationary GPS jitter straddles a cell edge, even if the fixes are only
// metres apart. update() keeps the accepted cell until the fix has moved at
// least min_move_m from the last accepted location, then admits the new raw
// cell. This preserves the 120 m evidence threshold without counting boundary
// noise as travel.
class StableCellTracker {
 public:
  StableCellTracker() { reset(); }

  int32_t update(double lat_deg, double lon_deg,
                 double min_move_m = 120.0);
  void reset();

 private:
  bool initialized_;
  double anchor_lat_;
  double anchor_lon_;
  int32_t cell_id_;
};

}  // namespace geo
