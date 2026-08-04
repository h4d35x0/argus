// test_gps_health.cpp - why the GPS has no fix, as told to the user.
//
// The defect this module exists to prevent is not a wrong fix, it is an
// UNEXPLAINED one. On 2026-08-03 the watch rendered "--" identically for "no
// sky", "still acquiring" and "receiver dead", and WarDrive refused a tap with
// a bare red X. The receiver was healthy throughout; the failing runs were 16
// minutes of exactly zero satellites. Three debugging rounds went into a
// question the device already had the counters to answer.
//
// So the cases that matter here are the BOUNDARIES between explanations: a
// silent link must not be reported as "no sky", a power-on must not flash a
// hardware fault, and a receiver that is talking must never be blamed.

#include "wl_test.h"
#include "gps_health.h"
#include <string.h>

// Healthy NMEA rate. Real captures ran ~370 B/s with nothing tracked and
// ~900 B/s locked, so this is comfortably "the link is alive".
static const uint32_t kLiveBps = 800;

WL_TEST(gps_health_off_beats_everything)
{
    // Radio off is a deliberate user action, never a fault. Even with a stale
    // satellite count and a dead link it must read Off.
    WL_CHECK(gps_health_classify(false, false, 0, 0, 999) == GpsHealth::Off);
    WL_CHECK(gps_health_classify(false, true, kLiveBps, 12, 0) == GpsHealth::Off);
}

WL_TEST(gps_health_locked_when_fixed)
{
    WL_CHECK(gps_health_classify(true, true, kLiveBps, 12, 0) == GpsHealth::Locked);
    WL_CHECK(gps_health_classify(true, true, kLiveBps, 4, 0) == GpsHealth::Locked);
}

WL_TEST(gps_health_no_satellites_is_a_sky_problem)
{
    // The 2026-08-03 failure signature: sentences arriving cleanly, receiver
    // seeing nothing. This must NOT read as a hardware fault.
    WL_CHECK(gps_health_classify(true, false, 369, 0, 0) == GpsHealth::NoSatellites);
    WL_CHECK(strcmp(gps_health_hint(GpsHealth::NoSatellites), "Needs open sky") == 0);
}

WL_TEST(gps_health_acquiring_when_sats_visible_but_no_fix)
{
    // Satellites in view but not enough of them yet. Waiting is the right
    // advice; telling the user to move would be wrong.
    WL_CHECK(gps_health_classify(true, false, kLiveBps, 1, 0) == GpsHealth::Acquiring);
    WL_CHECK(gps_health_classify(true, false, kLiveBps, 3, 0) == GpsHealth::Acquiring);
}

WL_TEST(gps_health_no_data_needs_to_persist)
{
    // A silent link on the FIRST sample of a power cycle is normal - the rate
    // is computed over an interval that has not elapsed yet. Flashing
    // "receiver not responding" there would cry wolf at every power-on.
    for (uint32_t q = 0; q < kGpsNoDataDwellSec; q++)
        WL_CHECK(gps_health_classify(true, false, 0, 0, q) != GpsHealth::NoData);

    // Once it has persisted, say so.
    WL_CHECK(gps_health_classify(true, false, 0, 0, kGpsNoDataDwellSec) == GpsHealth::NoData);
    WL_CHECK(gps_health_classify(true, false, 0, 0, 999) == GpsHealth::NoData);
}

WL_TEST(gps_health_no_data_beats_no_satellites)
{
    // Both conditions are true when the link dies (a silent link also reports
    // zero satellites). The link is the more specific and more actionable
    // answer, so it must win - otherwise a dead receiver reads as "go outside".
    WL_CHECK(gps_health_classify(true, false, 0, 0, kGpsNoDataDwellSec) == GpsHealth::NoData);
}

WL_TEST(gps_health_no_data_beats_a_stale_lock)
{
    // A link that has been silent for the dwell cannot be vouching for a
    // present-tense fix, whatever the caller's predicate still says.
    WL_CHECK(gps_health_classify(true, true, 0, 12, kGpsNoDataDwellSec) == GpsHealth::NoData);
}

WL_TEST(gps_health_trickle_counts_as_alive)
{
    // The floor asks "is it saying anything", not "is it saying much". A
    // receiver limping along at the floor is alive and must not be condemned.
    WL_CHECK(gps_health_classify(true, false, kGpsNoDataFloorBps, 0, 999)
             == GpsHealth::NoSatellites);
    WL_CHECK(gps_health_classify(true, false, kGpsNoDataFloorBps - 1, 0, 999)
             == GpsHealth::NoData);
}

WL_TEST(gps_health_labels_and_hints_are_present)
{
    const GpsHealth all[] = { GpsHealth::Off, GpsHealth::NoData,
                              GpsHealth::NoSatellites, GpsHealth::Acquiring,
                              GpsHealth::Locked };
    for (int i = 0; i < 5; i++) {
        WL_CHECK(gps_health_label(all[i])[0] != '\0');
        WL_CHECK(gps_health_hint(all[i])[0] != '\0');
        WL_CHECK(strcmp(gps_health_label(all[i]), "?") != 0);
    }
}

WL_TEST(gps_health_duration_formats)
{
    char buf[16];

    gps_health_duration(0, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "0s") == 0);

    gps_health_duration(45, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "45s") == 0);

    // Boundary: 60 s rolls to minutes, and seconds stay zero-padded so the
    // string does not jitter in width as it counts.
    gps_health_duration(59, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "59s") == 0);
    gps_health_duration(60, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "1m00s") == 0);
    gps_health_duration(192, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "3m12s") == 0);

    // Boundary: an hour. The 16-minute dead runs sat in the minutes band, but a
    // GPS left on overnight must not print a four-digit minute count.
    gps_health_duration(3599, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "59m59s") == 0);
    gps_health_duration(3600, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "1h00m") == 0);
    gps_health_duration(3600 + 4 * 60, buf, sizeof(buf));
    WL_CHECK(strcmp(buf, "1h04m") == 0);
}

// ---- BACKLOG RING ----------------------------------------------------------
// The backlog buffers health records while no SD card is present, because the
// log lives ON the card and could otherwise only ever observe the card-IN
// condition - useless when the symptom is "GPS is slower to lock with the card
// in". The failure mode worth guarding is MISORDERING, not loss: a backlog that
// walked from the wrong slot would misdate an acquisition.

// Push n records carrying identity values 0..n-1, return the ring contents in
// oldest-first order as the caller would flush them.
static void drain(GpsBacklog &b, uint8_t *store, int n, int *out, int *out_n)
{
    for (int i = 0; i < n; i++) store[b.push()] = (uint8_t)i;
    *out_n = b.count;
    for (int i = 0; i < b.count; i++) out[i] = store[b.slot((uint8_t)i)];
}

WL_TEST(gps_backlog_preserves_order_when_not_full)
{
    GpsBacklog b; b.init(8);
    uint8_t store[8];
    int got[8], n = 0;
    drain(b, store, 5, got, &n);

    WL_CHECK(n == 5);
    WL_CHECK(b.dropped == 0);
    for (int i = 0; i < 5; i++) WL_CHECK(got[i] == i);   // oldest first
}

WL_TEST(gps_backlog_exactly_full_drops_nothing)
{
    GpsBacklog b; b.init(4);
    uint8_t store[4];
    int got[4], n = 0;
    drain(b, store, 4, got, &n);

    WL_CHECK(n == 4);
    WL_CHECK(b.dropped == 0);
    for (int i = 0; i < 4; i++) WL_CHECK(got[i] == i);
}

WL_TEST(gps_backlog_overflow_keeps_the_newest_in_order)
{
    // 10 records into a 4-slot ring: the 4 most recent survive, still ordered
    // oldest-first, and the 6 lost are COUNTED rather than silently discarded.
    GpsBacklog b; b.init(4);
    uint8_t store[4];
    int got[4], n = 0;
    drain(b, store, 10, got, &n);

    WL_CHECK(n == 4);
    WL_CHECK(b.dropped == 6);
    WL_CHECK(got[0] == 6);
    WL_CHECK(got[1] == 7);
    WL_CHECK(got[2] == 8);
    WL_CHECK(got[3] == 9);
}

WL_TEST(gps_backlog_survives_wrap_after_clear)
{
    // A flush clears the ring but leaves head mid-array. The next card-out
    // stretch must still come back in order - this is the wrap case that an
    // off-by-one in slot() would silently corrupt.
    GpsBacklog b; b.init(4);
    uint8_t store[4];
    int got[4], n = 0;

    drain(b, store, 3, got, &n);   // head now at 3
    b.clear();
    WL_CHECK(b.count == 0);
    WL_CHECK(b.dropped == 0);

    for (int i = 100; i < 103; i++) store[b.push()] = (uint8_t)i;
    WL_CHECK(b.count == 3);
    for (int i = 0; i < 3; i++) WL_CHECK(store[b.slot((uint8_t)i)] == 100 + i);
}

WL_TEST(gps_backlog_zero_capacity_cannot_divide_by_zero)
{
    // slot() takes a modulo by cap, so a 0 capacity would fault. init() must
    // coerce it to something usable.
    GpsBacklog b; b.init(0);
    WL_CHECK(b.cap >= 1);
    b.push();
    WL_CHECK(b.slot(0) < b.cap);
}

WL_TEST(gps_health_duration_never_overruns)
{
    // Defensive: a tiny buffer must still be NUL-terminated, and a null one
    // must not be written at all.
    char small[4];
    memset(small, 'x', sizeof(small));
    gps_health_duration(123456, small, sizeof(small));
    WL_CHECK(small[sizeof(small) - 1] == '\0');

    gps_health_duration(42, nullptr, 0);   // must not crash
    char untouched[2] = { 'a', 'b' };
    gps_health_duration(42, untouched, 0);
    WL_CHECK(untouched[0] == 'a');         // cap 0 writes nothing
}
