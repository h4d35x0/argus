#pragma once
//
// gps_health.h - pure classification of WHY the GPS has no fix.
//
// Split out of gps_screen.cpp so the rules are testable on the host
// (test/test_gps_health.cpp) without a receiver on the bench. No Arduino, no
// LVGL, no hardware includes: keep it that way.
//
// WHY THIS EXISTS. Until now the watch rendered "--" identically for three
// completely different situations, and WarDrive answered a tap with a bare red
// X next to "GPS lock" and no account of why. On 2026-08-03 that cost three
// rounds of debugging: the receiver was healthy the whole time (bad=0 across
// ~93000 sentences, 12 satellites for 95 unbroken minutes), and the failing
// runs were 16 minutes of EXACTLY zero satellites - a reception problem the
// watch had all the information to describe and never did.
//
// The distinction the user actually needs is cheap, because TinyGPSPlus already
// counts everything required:
//
//   NoData        nothing arriving on the UART -> the receiver or its rail is
//                 the problem. Firmware/hardware, not sky.
//   NoSatellites  sentences arriving cleanly, nothing in view -> sky.
//                 Face-down on a desk, in a bag, or in a car. Move outside.
//   WeakSignal    satellites in view but every one too weak to acquire -> sky
//                 again, but the receiver can SEE it is being blocked, which is
//                 a far more credible thing to tell the user than "no sats".
//   Acquiring     satellites in view at a usable signal, no fix yet -> wait.
//   Locked        fix.
//
// CORRECTED 2026-09-03. The satellite count fed in here used to come from
// `TinyGPSPlus::satellites`, which is GGA field 7, "Satellites used"
// (TinyGPS++.cpp:277) - satellites in the position SOLUTION, not in view. That
// value is ~0 whenever there is no fix, so NoSatellites and Acquiring could not
// actually be told apart and the advice was inverted in the one case where
// waiting was right: on 2026-09-02 this said "Needs open sky" for the first
// 7.5 minutes of an acquisition that was visibly progressing, then locked at
// 11m17s. The count now comes from GSV via gps_gsv.h, and the C/N0 that arrives
// with it is what adds WeakSignal.
//
// C++11 only: the ESP32 Arduino core builds this at -std=gnu++11 even though
// the host test suite is C++17. No `if constexpr`, no inline variables.

#include <stdint.h>
#include <stdio.h>

enum class GpsHealth : uint8_t {
    Off,           // radio powered down by the user
    NoData,        // powered, but no NMEA is arriving
    NoSatellites,  // NMEA flowing, nothing in view at all
    WeakSignal,    // in view, but the best C/N0 is below acquisition
    Acquiring,     // in view at a usable signal, no fix yet
    Locked,        // fix
};

// Byte rate below which we call the link dead. NMEA at 38400 runs ~370 B/s even
// with nothing tracked (empty GGA/RMC plus a GSV reporting nothing in view), so
// anything at all above a trickle means the receiver is alive and talking. The
// floor is deliberately near zero: the question here is "is it saying anything",
// not "is it saying much".
static const uint32_t kGpsNoDataFloorBps = 20;

// How long the link must stay silent before we call it dead. The byte rate is
// sampled once per second and is legitimately 0 for the first sample of a power
// cycle, so reporting NoData instantly would flash a hardware fault at every
// power-on.
static const uint32_t kGpsNoDataDwellSec = 5;

// Carrier-to-noise floor, in dB-Hz, below which a satellite is in view but
// cannot realistically be brought into a fix from a standing start.
//
// This is a THRESHOLD, not a measurement, and it is set from the physics rather
// than from our own data: a GNSS receiver tracks a satellite it already holds
// far below the level it needs to ACQUIRE one, which is why a fix carried
// indoors survives while the same spot yields nothing after a restart. That
// asymmetry is exactly what the 2026-09-02 log caught - a fix held indoors on
// 5-8 satellites, then 16m45s of nothing after a 3m36s power cycle in the same
// room. 25 dB-Hz sits between the two: comfortably below open-sky levels
// (35-50), above the point where acquisition stops being plausible.
static const uint32_t kGpsCnoAcquireDb = 25;

// Classify the current GPS condition.
//
//   powered    the radio is on
//   have_lock  the caller's fix predicate (fresh position + enough satellites)
//   bps        NMEA bytes/second over the last sample interval
//   in_view    satellites IN VIEW from GSV (see gps_gsv.h), not used-in-fix
//   cno_max    strongest C/N0 in view, dB-Hz, 0 if nothing is in view
//   quiet_sec  how long bps has been continuously below the floor
//
// NoData is tested before Locked on purpose: a silent link cannot be vouching
// for a present-tense fix, and saying so is the more useful answer.
inline GpsHealth gps_health_classify(bool powered, bool have_lock,
                                     uint32_t bps, uint32_t in_view,
                                     uint32_t cno_max, uint32_t quiet_sec)
{
    if (!powered) return GpsHealth::Off;
    if (bps < kGpsNoDataFloorBps && quiet_sec >= kGpsNoDataDwellSec)
        return GpsHealth::NoData;
    if (have_lock)     return GpsHealth::Locked;
    if (in_view == 0)  return GpsHealth::NoSatellites;
    if (cno_max < kGpsCnoAcquireDb) return GpsHealth::WeakSignal;
    return GpsHealth::Acquiring;
}

// Short label for a status row. Kept narrow enough for the GPS screen's
// right-aligned value column.
inline const char *gps_health_label(GpsHealth h)
{
    switch (h) {
    case GpsHealth::Off:          return "Off";
    case GpsHealth::NoData:       return "No data";
    case GpsHealth::NoSatellites: return "No sats";
    case GpsHealth::WeakSignal:   return "Weak sig";
    case GpsHealth::Acquiring:    return "Acquiring";
    case GpsHealth::Locked:       return "Locked";
    }
    return "?";
}

// What the user should DO about it. This is the line that would have ended the
// 2026-08-03 debugging in one glance, so it names the action, not the state.
inline const char *gps_health_hint(GpsHealth h)
{
    switch (h) {
    case GpsHealth::Off:          return "GPS radio is off";
    case GpsHealth::NoData:       return "Receiver not responding";
    case GpsHealth::NoSatellites: return "Needs open sky";
    case GpsHealth::WeakSignal:   return "Signal too weak, move outside";
    case GpsHealth::Acquiring:    return "In view, hold still";
    case GpsHealth::Locked:       return "Locked";
    }
    return "";
}

// Ring bookkeeping for the health backlog. INDEX MATH ONLY - the caller owns
// the storage, so this stays free of any record layout and testable on the host.
//
// The backlog exists because the health log lives ON the SD card and could
// therefore only ever observe the card-IN condition, which is useless when the
// reported symptom is "GPS takes longer to lock once the SD card is in".
// Records are captured regardless and buffered here until a card appears.
//
// The failure mode worth guarding is not losing a record, it is MISORDERING
// one: a backlog that silently dropped its head, or walked from the wrong slot,
// would misdate an acquisition and send the next investigation the wrong way.
// Hence `dropped` is counted and reported rather than swallowed.
struct GpsBacklog {
    uint8_t  cap;
    uint8_t  head;      // next slot to write
    uint8_t  count;     // live records, <= cap
    uint16_t dropped;   // overwritten before anything could flush them

    void init(uint8_t capacity)
    {
        cap = capacity ? capacity : 1;
        head = 0; count = 0; dropped = 0;
    }

    // Claim the next slot. Returns the index to write into. When full this
    // overwrites the OLDEST record, so a long card-out run keeps its recent
    // history rather than refusing to record anything further.
    uint8_t push()
    {
        const uint8_t slot = head;
        head = (uint8_t)((head + 1u) % cap);
        if (count < cap) count++;
        else             dropped++;
        return slot;
    }

    // Index of the i-th OLDEST live record, i in [0, count).
    uint8_t slot(uint8_t i) const
    {
        return (uint8_t)((head + cap - count + i) % cap);
    }

    void clear() { count = 0; dropped = 0; }
};

// Compact duration for "no satellites for 3m12s". Writes "45s", "3m12s" or
// "1h04m"; always NUL-terminates. Durations are how the user tells "just
// started looking" from "this is never going to work".
inline void gps_health_duration(uint32_t secs, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (secs < 60u)
        snprintf(out, cap, "%lus", (unsigned long)secs);
    else if (secs < 3600u)
        snprintf(out, cap, "%lum%02lus",
                 (unsigned long)(secs / 60u), (unsigned long)(secs % 60u));
    else
        snprintf(out, cap, "%luh%02lum",
                 (unsigned long)(secs / 3600u), (unsigned long)((secs % 3600u) / 60u));
}
