#pragma once
//
// clock_time.h - pure clock arithmetic for the ARGUS watch face.
//
// THE INVARIANT
// The RTC always holds UTC. The clock face, the world clock, the Meshtastic
// screen and every SD log stamp derive local time as UTC + offset, where the
// offset is discovered from GPS longitude or IP geolocation and persisted to
// /Settings/timezone.txt.
//
// Everything that WRITES the RTC therefore has to convert the other way first,
// and record the offset it converted with. Two writers used not to:
//
//   - Manual Time wrote the user's LOCAL wall clock straight into the RTC and
//     dropped the offset to 0 in RAM ONLY. The 0 was never persisted, so the
//     next boot restored the last GPS/WiFi-detected offset and applied it to an
//     RTC that no longer held UTC. A watch set by hand after a trip came back up
//     off by the whole difference between the two zones.
//   - The firmware-build-time fallback wrote local build time and left the
//     offset alone, so the seeded face was wrong by the offset immediately.
//
// Both are the same defect: the RTC and the offset are a PAIR, and a writer
// that updates one without the other leaves them describing different zones.
// The conversion and the file-format migration that repairs old cards live here
// so they can be exercised on the host with no hardware.
//
// C++11 only: the ESP32 Arduino core builds at -std=gnu++11 even though the
// host suite is C++17.

#include <time.h>

namespace clocktime {

// Broken-down wall clock. 4-digit year, 1-based month and day, matching
// LilyGoLib's rtc.setDateTime() rather than struct tm's 1900/0-based fields.
struct DateTime {
    int year, mon, day, hour, min, sec;
};

// Offsets outside this range are not a timezone; they are a corrupt file or a
// garbage GPS fix, and applying one silently is how the face ends up hours off.
static const int kMinOffsetHours = -12;
static const int kMaxOffsetHours = 14;

bool offset_plausible(int offset_hours);

// The two halves of the invariant. Both normalise the day/month/year rollover
// the shift can produce: 23:30 local at offset +2 is 21:30 the SAME day in UTC,
// but 00:30 local at offset +2 is 22:30 the PREVIOUS day.
DateTime local_to_utc(const DateTime &local, int offset_hours);
DateTime utc_to_local(const DateTime &utc,   int offset_hours);

// In-place UTC -> local on the struct tm the RTC hands back. Same arithmetic as
// utc_to_local(); this is the shape the readers (clock face, log stamps) want.
void tm_utc_to_local(struct tm *t, int offset_hours);

// ---- /Settings/timezone.txt versioning -------------------------------------
//
// v2 files carry "v=2" and always store the offset that pairs with what is
// actually in the RTC. v1 files carry no "v=" key and only describe the RTC
// when Manual Time is OFF, because the old manual-time path never wrote its 0
// down (see above). Migrating on read is what repairs a card written by the
// old firmware.
static const int kFileVersion = 2;

// The offset that actually pairs with the RTC, given what the card says.
int effective_saved_offset(int saved_offset, int file_version, bool manual_active);

}   // namespace clocktime
