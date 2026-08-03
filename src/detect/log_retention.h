#pragma once
//
// log_retention.h - retention policy for the on-SD detection logs.
//
// WHY THIS EXISTS
// ARGUS writes a line to the SD card every time a detector first alerts on a
// contact: /ThreatRadar, /AirTag, /Flipper, /Skimmers, /EvilTwin discovered.txt,
// and one file per hit under /Flock. Each record contains ANOTHER PERSON'S
// device identifier and roughly where they were standing. The 2026-07-28
// con-readiness audit flagged that these were append-only with no cap, no
// rotation and no age-out: a record written once was kept forever.
//
// That is indefensible for a tool whose entire argument is that it does not
// hoard other people's identifiers. The mitigating factor used to be the
// RARITY of the records, not their contents, and rarity is not a policy.
//
// The fix is to bound RETENTION, not fidelity. The MAC stays in the clear
// because the log's whole purpose is to be the wearer's own evidence about a
// device that followed THEM, and a hashed MAC cannot be matched against a
// device you later physically identify. What changes is that the evidence
// expires: old enough to be someone else's business, gone.
//
// This header is PURE - no Arduino, no SD, no LVGL - so test/test_log_retention
// can exercise the policy on the host. The SD side lives in detect_log_sd.cpp.
//
// C++11 only: the ESP32 Arduino core builds at -std=gnu++11 even though the
// host suite is C++17.

#include <stdint.h>

namespace detlog {

// A tail you care about is recent. 30 days is long enough to notice a pattern
// across a few weeks of commuting and short enough that the watch is not
// carrying a quarter's worth of strangers.
static const int kMaxAgeDays = 30;

// Hard ceiling per log, independent of age, so a single dense day (a con floor,
// an airport) cannot leave a huge file behind. Oldest entries are evicted first.
static const int kMaxEntries = 300;

static const long kSecondsPerDay = 86400L;
static const long kMaxAgeSeconds = (long)kMaxAgeDays * kSecondsPerDay;

// Days since 1970-01-01 for a civil (proleptic Gregorian) date.
// Howard Hinnant's algorithm. Deliberately avoids time.h/mktime: both the
// stamps in the log and the "now" we compare against are watch-LOCAL time, so
// introducing a timezone anywhere in this path could only corrupt the answer.
long days_from_civil(int y, unsigned m, unsigned d);

// Parse a log line beginning "YYYY-MM-DD HH:MM:SS" into seconds since the
// epoch in that same local frame. Returns false when the line does not start
// with that shape, which is how continuation lines and headers are recognised.
bool parse_stamp(const char *line, long *out_sec);

// True when `stamp` is older than the retention window.
// A stamp in the FUTURE is never expired: if the RTC was wrong when a record
// was written, or is wrong now, silently deleting evidence is the worse error.
bool is_expired(long stamp_sec, long now_sec, long max_age_sec);

// Index of the first entry to KEEP, given a chronologically ordered log.
//   n              total entries in the file
//   first_unexpired index of the first entry inside the age window
//                  (pass n when every entry has expired)
//   max_entries    hard cap, or <= 0 to disable
// Retention only ever drops a PREFIX, which is what makes the SD-side
// compaction a single streaming copy rather than a random-access rewrite.
int first_kept(int n, int first_unexpired, int max_entries);

}  // namespace detlog
