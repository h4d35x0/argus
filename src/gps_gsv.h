#pragma once
//
// gps_gsv.h - satellites IN VIEW and their signal strength, from GSV.
//
// WHY THIS EXISTS. `gps_health.h` classifies WHY there is no fix, and until now
// it did so from `TinyGPSPlus::satellites`, which is bound to GGA field 7,
// "Satellites used" (TinyGPS++.cpp:277) - satellites in the position solution.
// That value is ~0 whenever there is no fix, so the branch that was supposed to
// separate "nothing in the sky" from "plenty in view, still collecting data"
// could not actually tell them apart, and the watch gave the wrong advice in
// the one case where waiting was the right move.
//
// Measured cost, 2026-09-02 (artifacts/field/20260903/Settings/gpshealth.log,
// cycle 3): the watch displayed "No sats / Needs open sky" for the first
// 7.5 minutes of an acquisition that was visibly progressing - the NMEA byte
// rate climbed 421 -> 513 B/s the whole time because satellites were coming
// into view - and then locked at 11m17s. The stream carried the answer in GSV
// and nothing parsed it.
//
// The same log also showed why C/N0 matters: satellites-USED fell 12 -> 5 while
// the byte rate never moved off ~860 B/s, so in-view and used-in-fix diverged
// by more than a factor of two inside one power cycle. And a fix held indoors
// on 5-8 satellites could not be re-established from a standing start 3m36s
// later in the same room, because acquisition needs far more signal than
// tracking does. Without C/N0 that distinction is invisible.
//
// DESIGN: assumption-free about the sentence mix. u-blox M10 emits GSV per
// constellation (GPGSV/GLGSV/GAGSV/GBGSV/GQGSV) and, on NMEA 4.11, can emit
// more than one set per constellation for different signals. Rather than trust
// a talker list, a message count or the in-view field, this counts DISTINCT
// (talker, satellite id) pairs actually reported, and expires them on a TTL.
// Multi-signal duplicates of one satellite collapse onto the same key, so
// nothing is double counted, and no assumption about the receiver's NMEA
// version can be wrong.
//
// C++11 only: the ESP32 Arduino core builds this at -std=gnu++11 even though
// the host test suite is C++17. No Arduino, no LVGL, no hardware includes -
// keep it that way so test/test_gps_gsv.cpp can drive it on the host.

#include <stdint.h>
#include <stddef.h>

// How long a satellite stays "in view" after the last GSV that mentioned it.
// GSV arrives at the 1 Hz navigation rate, so this only has to outlast normal
// jitter; it must still be short enough that a receiver going quiet, or the
// radio being powered down, empties the table promptly.
static const uint32_t kGpsGsvTtlMs = 4000;

// Table capacity. A multi-constellation receiver with a clear sky reports on
// the order of 30 satellites in view; 48 leaves headroom without being worth a
// heap allocation. Overflow is COUNTED rather than silently ignored - an
// undercount here would read as "less sky than there is" and send the next
// investigation the wrong way.
static const uint8_t kGpsGsvMaxSats = 48;

// Longest legal NMEA sentence is 82 chars including "$" and CRLF. The extra
// slack costs nothing and means a receiver that runs long does not desync the
// assembler.
static const size_t kGpsGsvBufBytes = 100;

struct GpsGsvSat {
    uint16_t talker;    // two talker chars packed, e.g. 'G'<<8 | 'P'
    uint16_t svid;      // satellite id as reported for that talker
    uint8_t  cno;       // carrier-to-noise, dB-Hz; 0 = in view but not tracked
    uint32_t seen_ms;   // last GSV that mentioned it
};

class GpsGsv {
public:
    GpsGsv() { reset(); }

    // Drop all state. Call on every GPS power transition, so a new cycle never
    // inherits the previous one's sky.
    void reset();

    // Feed one raw NMEA byte. Non-GSV sentences and sentences that fail their
    // checksum are ignored. `now_ms` is the caller's monotonic clock, passed in
    // rather than read, so the host tests can drive time directly.
    void feed(char c, uint32_t now_ms);

    // Satellites in view: distinct (talker, svid) pairs still within the TTL.
    uint8_t in_view(uint32_t now_ms) const;

    // Strongest C/N0 currently in view, 0 if nothing is. This is the number that
    // separates "sky is blocked" from "sky is there, still working".
    uint8_t cno_max(uint32_t now_ms) const;

    // How many satellites are in view at or above `db`. A fix needs four
    // satellites acquired, so this is the honest answer to "could this ever
    // lock from here".
    uint8_t cno_at_least(uint32_t now_ms, uint8_t db) const;

    // Satellites dropped because the table was full and nothing in it had
    // expired. Counted rather than swallowed: an undercount here would read as
    // "less sky than there is". Expected to stay 0.
    uint16_t overflowed() const { return _overflow; }

    // Exposed for the tests; the firmware has no reason to walk the table.
    uint8_t capacity() const { return kGpsGsvMaxSats; }

private:
    void     process_sentence(uint32_t now_ms);
    void     record(uint16_t talker, uint16_t svid, uint8_t cno, uint32_t now_ms);
    bool     live(const GpsGsvSat &s, uint32_t now_ms) const;

    GpsGsvSat _sat[kGpsGsvMaxSats];
    uint8_t   _count;
    uint16_t  _overflow;

    char      _buf[kGpsGsvBufBytes];
    size_t    _len;
    bool      _in_sentence;   // false until a '$' is seen, so we resync cleanly
};

// Validate an NMEA checksum. `s` points at the sentence WITHOUT the leading '$'
// and including the "*HH" suffix. Exposed because it is worth testing on its
// own: a corrupted sentence that slipped through would inject a satellite that
// was never there.
bool gps_gsv_checksum_ok(const char *s, size_t len);
