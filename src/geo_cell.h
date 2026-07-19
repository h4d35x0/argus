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
int32_t coarse_cell(double lat_deg, double lon_deg, double cell_m = 120.0);

}  // namespace geo
