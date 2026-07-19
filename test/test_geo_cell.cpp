// test_geo_cell.cpp - host unit tests for the pure GPS-to-coarse-cell quantizer
// (src/geo_cell). It feeds tail_detect's distinct-cell count, so what matters is:
// same place -> same id, a real move -> a different id, and a walk crosses
// several distinct cells. Not a reversible geocode, so we test behavior, not
// exact index values.
#include "wl_test.h"
#include "geo_cell.h"

#include <cstdint>
#include <set>

using geo::coarse_cell;

// A reference spot (somewhere near 45N, a mid-latitude so cos(lat) is exercised).
static const double kLat = 45.000000;
static const double kLon = -93.000000;

// ---- Determinism: same coordinates always map to the same cell. -------------
WL_TEST(geo_same_point_same_cell) {
  WL_CHECK_EQ(coarse_cell(kLat, kLon), coarse_cell(kLat, kLon));
}

// ---- A sub-metre jitter stays in the same cell. -----------------------------
// 1e-6 deg ~ 0.1 m, far inside a 120 m cell, so GPS noise does not spuriously
// look like a move to a new cell.
WL_TEST(geo_tiny_jitter_same_cell) {
  int32_t a = coarse_cell(kLat, kLon);
  int32_t b = coarse_cell(kLat + 1e-6, kLon - 1e-6);
  WL_CHECK_EQ(a, b);
}

// ---- A clear move to a different area yields a different cell. ---------------
// ~0.02 deg latitude ~ 2.2 km, many cells away.
WL_TEST(geo_far_move_different_cell) {
  int32_t a = coarse_cell(kLat, kLon);
  int32_t b = coarse_cell(kLat + 0.02, kLon);
  WL_CHECK(a != b);
  int32_t c = coarse_cell(kLat, kLon + 0.02);
  WL_CHECK(a != c);
}

// ---- A walk crosses several distinct cells (the tail-detect use case). ------
// Step ~200 m north each time (0.0018 deg lat); 6 steps should land in several
// distinct cells given a 120 m cell.
WL_TEST(geo_walk_spans_multiple_cells) {
  std::set<int32_t> cells;
  for (int i = 0; i < 6; i++)
    cells.insert(coarse_cell(kLat + i * 0.0018, kLon));
  WL_CHECK(cells.size() >= 4);   // a real walk is not one stationary cell
}

// ---- Custom cell size: a bigger cell merges points a small cell separates. --
WL_TEST(geo_cell_size_controls_resolution) {
  // Two points ~300 m apart (0.0027 deg lat).
  double lat2 = kLat + 0.0027;
  // At 120 m they are different cells...
  WL_CHECK(coarse_cell(kLat, kLon, 120.0) != coarse_cell(lat2, kLon, 120.0));
  // ...at 2 km they collapse into the same cell.
  WL_CHECK_EQ(coarse_cell(kLat, kLon, 2000.0), coarse_cell(lat2, kLon, 2000.0));
}

// ---- Guards: non-positive / NaN cell size falls back to the 120 m default. --
WL_TEST(geo_bad_cell_size_uses_default) {
  int32_t def = coarse_cell(kLat, kLon, 120.0);
  WL_CHECK_EQ(coarse_cell(kLat, kLon, 0.0), def);
  WL_CHECK_EQ(coarse_cell(kLat, kLon, -5.0), def);
}

// ---- Non-negative: tail_detect uses cell_id < 0 as "unknown location", so a
// negative cell would be silently dropped. coarse_cell must NEVER return < 0.
WL_TEST(geo_cell_is_never_negative) {
  // Sweep a spread of coordinates (many of which hashed into the sign bit before
  // the fix) and assert every id is non-negative.
  for (int i = 0; i < 200; i++) {
    double lat = -89.0 + (i * 179.0) / 200.0;   // -89 .. +90
    double lon = -179.0 + (i * 359.0) / 200.0;  // -179 .. +180
    WL_CHECK(coarse_cell(lat, lon) >= 0);
  }
}

// ---- Pole safety: an extreme but valid latitude does not blow up. -----------
// cos(lat)->0 near the poles; the clamp must keep the result finite/defined.
WL_TEST(geo_pole_is_safe) {
  int32_t n = coarse_cell(89.9, 10.0);
  int32_t s = coarse_cell(-89.9, 10.0);
  WL_CHECK(n != s);                       // different hemispheres, different cell
  WL_CHECK_EQ(n, coarse_cell(89.9, 10.0)); // still deterministic
}
