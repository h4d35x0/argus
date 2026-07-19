#include "geo_cell.h"
#include <cmath>

namespace geo {

// Meters per degree of latitude (WGS84 mean); good to a fraction of a percent,
// which is far finer than a ~120 m coarse cell needs.
static const double kMetersPerDegLat = 111320.0;
static const double kPi = 3.14159265358979323846;

int32_t coarse_cell(double lat_deg, double lon_deg, double cell_m)
{
    if (!(cell_m > 0.0)) cell_m = 120.0;   // guard 0 / negative / NaN

    const double dlat = cell_m / kMetersPerDegLat;

    // Longitude degrees shrink with latitude; scale by cos(lat) so a cell stays
    // ~square. Clamp near the poles so dlon does not blow up (and to keep the
    // index finite for a valid-but-extreme fix).
    double coslat = std::cos(lat_deg * kPi / 180.0);
    if (coslat < 0.01) coslat = 0.01;
    const double dlon = cell_m / (kMetersPerDegLat * coslat);

    // floor() so a whole grid square maps to one index (round() would split a
    // cell across its centre). int64 accumulators cannot overflow for any valid
    // WGS84 coordinate at these resolutions.
    const int64_t lat_idx = static_cast<int64_t>(std::floor(lat_deg / dlat));
    const int64_t lon_idx = static_cast<int64_t>(std::floor(lon_deg / dlon));

    // Mix the two grid indices into a well-distributed 32-bit id (SplitMix64
    // finalizer over a golden-ratio combination). Distinct (lat_idx, lon_idx)
    // pairs almost never collide across the few cells a tail path spans.
    uint64_t h = static_cast<uint64_t>(lat_idx) * 0x9E3779B97F4A7C15ULL;
    h ^= static_cast<uint64_t>(lon_idx) * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    // Mask to 31 bits so the id is ALWAYS non-negative. Consumers (tail_detect)
    // reserve negative cell_id as the "unknown location" sentinel (-1); a hash
    // that landed in the sign bit would be silently dropped as unknown and
    // contribute no location evidence. 31 bits still makes collisions across a
    // tail's handful of cells negligible.
    return static_cast<int32_t>(h & 0x7FFFFFFFu);
}

}  // namespace geo
