// tracker_ident.h - pure, host-testable "unwanted tracker" ADVERT classifier.
//
// DEFENSIVE ONLY. This module transmits nothing and attacks nothing. It answers
// ONE question about a SINGLE BLE advertising payload: "is this a separated /
// unwanted commercial location tracker (an AirTag / Find My beacon someone may
// have slipped into your bag), and of what kind?" It is the payload-
// classification half of ARGUS's anti-stalking feature.
//
// It COMPOSES with the shared BLE AD parser (adv_parser.h): it never re-walks
// raw advertising bytes by hand, delegating every byte fetch to that bounds-
// checked parser and only re-checking the specific status byte it indexes.
//
// WHAT THIS MODULE DOES NOT DO (read this before trusting a single verdict):
//   * It does NOT and CANNOT by itself prove that a specific physical tag is
//     FOLLOWING the wearer. A separated Find My tracker rotates its advertised
//     public key / MAC roughly every 15 minutes, so two sightings 20 minutes
//     apart look like two different devices at the BLE layer. Attribution across
//     that rotation - "the same unwanted tracker keeps showing up wherever I go"
//     - is the job of the cross-location follow detector (tail_detect.h), which
//     the integration feeds with the sightings THIS helper flags. This helper is
//     the per-advert gate; tail_detect is the per-place-over-time judge.
//   * It does NOT fingerprint a make/model beyond the coarse TrackerKind below.
//   * The Apple offline-finding status-byte semantics it reads are REVERSE-
//     ENGINEERED from public research (Apple has never published this format);
//     see the note on identify_tracker() for the exact byte/bit and its source.
//     Treat the OwnerNearby / Separated split as a best-effort hint, not proof.
//
// Design constraints (deliberate, matches adv_parser.h / the other detectors):
//   * Pure C++11/14. NO Arduino.h, NO ESP-IDF, NO LVGL, NO hardware. Compiles
//     and runs on the host g++ so the logic is unit-tested in milliseconds.
//   * Stateless. No tables, no clock, no dynamic allocation. Every call is a
//     pure function of the bytes handed in; the caller owns the buffer.
//   * Bounds-safe. A short / truncated / garbage payload returns a clean
//     {None, Unknown} and never over-reads.
#pragma once

#include <cstddef>
#include <cstdint>

namespace detect {

// The coarse family of a recognized tracker advert.
enum class TrackerKind : uint8_t {
  None = 0,       // not a tracker advert we recognize
  AppleFindMy,    // Apple offline-finding manufacturer advert (0x004C, type 0x12)
  FindMyNetwork,  // Find My Network service advert (16-bit service UUID 0xFD44)
  Unknown,        // reserved: recognized-tracker-ish but unclassifiable (unused today)
};

// Owner-presence state, decoded from the Apple offline-finding status byte when
// available. Unknown means "we could not tell" (a non-Apple-manufacturer Find My
// service advert, or a payload too short to carry the status byte) - which the
// convenience gate below treats conservatively (still worth flagging).
enum class TrackerStatus : uint8_t {
  Unknown = 0,   // status byte absent / not decodable
  OwnerNearby,   // status indicates the owner device is present/maintaining it
  Separated,     // status indicates separated / "lost mode" (no owner nearby)
};

// The verdict for one advert.
struct TrackerId {
  TrackerKind   kind;
  TrackerStatus status;
};

// Classify a single BLE advertising payload [adv, adv+len).
//
// Recognition:
//   * AppleFindMy  when ble::adv_is_apple_findmy() holds (Apple company 0x004C
//                  manufacturer record whose first post-company-id byte is the
//                  offline-finding type 0x12). Status is then decoded from the
//                  status byte (see below).
//   * FindMyNetwork when the 16-bit Find My Network service UUID 0xFD44 is
//                  present (as service data 0x16 or in a 16-bit UUID list) and
//                  the advert is not already an Apple offline-finding record.
//                  Status is left Unknown (third-party accessory state is not
//                  decoded here).
//   * None         otherwise, including for any short / truncated / garbage
//                  buffer (adv_parser rejects malformed records, so we never
//                  over-read).
//
// STATUS BYTE (the exact bit and its provenance):
//   The Apple offline-finding manufacturer value, AFTER the 2-byte company id
//   that adv_manufacturer() strips, is laid out as:
//       payload[0] = 0x12   (offline-finding type; APPLE_FINDMY_TYPE)
//       payload[1] = length (0x19 == 25 for an AirTag full lost-mode key advert)
//       payload[2] = STATUS byte   <-- the byte we read
//       payload[3..]           = rotating EC public-key material, hint, etc.
//   We read payload[2] (== value byte index 4 of the manufacturer AD value:
//   4C 00 12 <len> <status> ...) only when the payload is long enough to carry
//   it (payload_len >= 3); otherwise status stays Unknown.
//
//   We test the MAINTAINED / owner-connected flag in the HIGH nibble of the
//   status byte: mask kFindMyStatusMaintainedBit == 0x40 (bit 6). Bit SET is
//   read as OwnerNearby (the tag's key is being maintained because the owner
//   device is nearby / recently connected); bit CLEAR is read as Separated
//   (broadcasting in lost mode with no owner present). Chosen so it does not
//   collide with the battery-level bits that public reverse-engineering places
//   in the top two status bits, and so it agrees with the "separated" AirTag
//   fixture used by the adv_parser tests (status 0xA0, bit 6 clear -> Separated).
//
//   PROVENANCE + HONESTY: this layout and the status-bit meaning come from
//   PUBLIC reverse-engineering of the Find My / offline-finding protocol
//   (e.g. Heinrich et al. 2021, "Who Can Find My Devices?"; the OpenHaystack
//   project) and from ARGUS's own airtag detector (company 0x004C, fm_type 0x12,
//   length 0x19). Apple does not publish it, so the OwnerNearby vs Separated
//   split is a best-effort hint, NOT ground truth. A tag that is genuinely
//   following you is proven by tail_detect across locations, not by this bit.
TrackerId identify_tracker(const uint8_t* adv, size_t len);

// Convenience gate for the follow-detector feed: is this advert worth handing to
// tail_detect as a POTENTIAL unwanted tracker?
//   * true  for a recognized tracker whose status is Separated (lost mode) OR
//           Unknown (be conservative: an unknown-state Find My beacon that then
//           follows you across places is still worth flagging).
//   * false for a tracker positively OwnerNearby (the owner is present - not a
//           plant) and for any non-tracker / malformed advert.
bool is_unwanted_tracker(const uint8_t* adv, size_t len);

// Apple offline-finding status byte: mask of the maintained / owner-connected
// bit we test (bit 6). See identify_tracker() for the full rationale + caveats.
static const uint8_t kFindMyStatusMaintainedBit = 0x40;

// Find My Network 16-bit service UUID (Bluetooth SIG member service, Apple).
static const uint16_t kFindMyNetworkServiceUuid = 0xFD44;

}  // namespace detect
