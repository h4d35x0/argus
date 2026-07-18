#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Threat Radar — spatio-temporal correlation of the wireless detectors.
//
// The standalone AirTag / Flipper / Skimmer / Flock / Evil-Twin detectors each
// answer "is there a tracker NEAR me right now?". On their own a single hit is
// noise: a cafe is full of stray AirTags, a car park is full of Flippers.
//
// Threat Radar answers the question that actually matters: "is the SAME device
// MOVING WITH ME?". It funnels every confirmed detection through one store
// keyed by MAC, and because each sighting is stamped with the user's own GPS
// fix, a single MAC whose sightings spread across hundreds of metres of the
// user's travel can only mean that physical device travelled along — i.e. it is
// following. A fixed installation (Flock camera, a parked tracker) shows up at
// exactly one spot and is correctly ignored. The "am I even moving?" gate is
// therefore implicit in the contact's own waypoint spread — no separate user
// track needed, and no false positives from crowds of stationary trackers.
// ---------------------------------------------------------------------------

// Which detector surfaced the device.
enum TrCategory {
    TR_CAT_AIRTAG = 0,
    TR_CAT_FLIPPER,
    TR_CAT_SKIMMER,
    TR_CAT_FLOCK,
    TR_CAT_EVILTWIN,
    TR_CAT_VEHICLE,        // ambient BLE/WiFi promoted by counter-tail (a car)
    TR_CAT_COUNT
};

// Confidence that a contact is FOLLOWING the user (co-moving along the track).
enum TrLevel {
    TR_LVL_NONE = 0,   // seen, but not co-moving (one spot / you were idle)
    TR_LVL_POSSIBLE,   // re-seen at a couple of spread-out waypoints
    TR_LVL_LIKELY,     // sustained co-movement over distance + time
    TR_LVL_CONFIRMED   // long co-movement, many waypoints, large displacement
};

// Called from the BT/WiFi scan task by each detector's *_check() the instant a
// match is confirmed (post-dedup, so ~one clean sighting per MAC per 5 min).
// Lightweight by design: just enqueues the sighting for the main task to fold
// in — no GPS read, no store mutation, no SD here.
void threatradar_observe(const uint8_t *mac6, int8_t rssi, uint8_t category);

// Main-task pump: drains the sighting queue, samples the current GPS fix,
// updates the contact store, recomputes follow-levels and drives the haptic
// alert. Call from loop() alongside the other *_bg_tick()s.
void threatradar_bg_tick();

// --- read API for the screen + home-screen badge (main/LVGL task only) ------

// Snapshot of one ranked threat for the list UI / map pin.
struct TrThreat {
    uint8_t  mac[6];
    uint8_t  category;     // TrCategory
    uint8_t  level;        // TrLevel
    uint8_t  waypoints;    // distinct >=TR_WP_MIN_M-apart spots it re-appeared at
    uint16_t span_m;       // distance between its extreme waypoints (m, capped)
    uint16_t span_min;     // minutes between first and last sighting
    int8_t   best_rssi;    // strongest (closest) RSSI seen — proximity hint
    bool     active;       // seen within the staleness window
    bool     community;    // flagged as a stalker over the mesh (peer or us)
    bool     familiar;     // a vehicle/device you co-move with daily (your own)
    float    first_lat;    // where it was first picked up — the map pin
    float    first_lon;
    char     first_time[6];// "HH:MM" first seen
};

// Active contacts at >= TR_LVL_POSSIBLE — the home-screen badge count.
int  threatradar_threat_count();
// Highest level among active contacts — drives badge colour / banner.
int  threatradar_top_level();

// Fills up to `max` threats sorted by (level, span) desc. Returns the count.
int  threatradar_get_threats(TrThreat *out, int max);

// Edge-triggered alert: returns true (and fills `out`) once when a contact
// first crosses into TR_LVL_LIKELY. Consuming it clears the edge so the screen
// can flash the banner exactly once.
bool threatradar_take_alert(TrThreat *out);

// Wipe the whole contact store — start of a new trip / panic clear.
void threatradar_reset();

// Display helpers.
const char *threatradar_category_name(uint8_t category);
const char *threatradar_level_name(uint8_t level);
