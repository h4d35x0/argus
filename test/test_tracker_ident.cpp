// test_tracker_ident.cpp - host unit tests for the pure "unwanted tracker"
// advert classifier (src/detect/tracker_ident). These build small real-ish BLE
// adverts (reusing the Apple Find My manufacturer pattern from test_ble_adv.cpp)
// and assert the OwnerNearby vs Separated status split, the FindMyNetwork
// service recognition, the non-tracker miss, and - the reason the module is
// bounds-safe - that truncated / garbage buffers return a clean {None, Unknown}
// with no over-read or crash.
#include "wl_test.h"
#include "tracker_ident.h"

#include <cstdint>
#include <vector>

using namespace detect;

// Build an Apple offline-finding (AirTag-style) manufacturer advert with a
// caller-chosen status byte. Layout mirrors make_airtag_adv() in test_ble_adv.cpp:
//   1E FF 4C 00 12 19 <status> <22-byte key> <key-byte0> <hint>
// seg_len 0x1E = 30 -> 29 value bytes. value[0..1]=4C 00 (Apple),
// value[2]=0x12 (fm_type), value[3]=0x19 (len), value[4]=<status>.
static std::vector<uint8_t> make_findmy_adv(uint8_t status) {
  std::vector<uint8_t> v;
  v.push_back(0x1E);   // AD length: 30 (type + 29 value bytes)
  v.push_back(0xFF);   // AD type: manufacturer
  v.push_back(0x4C);   // company LE lo (Apple 0x004C)
  v.push_back(0x00);   // company LE hi
  v.push_back(0x12);   // fm_type: offline finding
  v.push_back(0x19);   // payload length byte (25) = AirTag lost-mode
  v.push_back(status); // status byte (bit 6 = maintained/owner-connected)
  for (int i = 0; i < 22; i++) v.push_back((uint8_t)(0x10 + i));  // EC key
  v.push_back(0x3F);   // public-key byte 0 (upper bits)
  v.push_back(0x00);   // hint byte
  return v;
}

WL_TEST(tracker_separated_findmy_is_unwanted) {
  // Status 0xA0: bit 6 (0x40) CLEAR -> Separated. (Same fixture value the
  // adv_parser tests label a "separated" AirTag.)
  auto v = make_findmy_adv(0xA0);
  TrackerId id = identify_tracker(v.data(), v.size());
  WL_CHECK(id.kind == TrackerKind::AppleFindMy);
  WL_CHECK(id.status == TrackerStatus::Separated);
  WL_CHECK(is_unwanted_tracker(v.data(), v.size()));

  // A plain zero status is also separated (no maintained bit).
  auto z = make_findmy_adv(0x00);
  TrackerId idz = identify_tracker(z.data(), z.size());
  WL_CHECK(idz.status == TrackerStatus::Separated);
  WL_CHECK(is_unwanted_tracker(z.data(), z.size()));
}

WL_TEST(tracker_owner_nearby_findmy_is_not_unwanted) {
  // Status 0x40: bit 6 SET -> OwnerNearby. Owner present -> NOT a plant.
  auto v = make_findmy_adv(0x40);
  TrackerId id = identify_tracker(v.data(), v.size());
  WL_CHECK(id.kind == TrackerKind::AppleFindMy);
  WL_CHECK(id.status == TrackerStatus::OwnerNearby);
  WL_CHECK(!is_unwanted_tracker(v.data(), v.size()));

  // Maintained bit set alongside battery bits (0x40 | 0x80 = 0xC0) is still
  // OwnerNearby - we test only the maintained bit, not the battery bits.
  auto b = make_findmy_adv(0xC0);
  TrackerId idb = identify_tracker(b.data(), b.size());
  WL_CHECK(idb.status == TrackerStatus::OwnerNearby);
  WL_CHECK(!is_unwanted_tracker(b.data(), b.size()));
}

WL_TEST(tracker_findmy_network_service_recognized) {
  // Find My Network service-data advert: service data (AD 0x16) for UUID 0xFD44.
  //   02 01 06                 flags
  //   05 16 44 FD AB CD        service data UUID 0xFD44, data AB CD
  static const uint8_t sd[] = {
      0x02, 0x01, 0x06,
      0x05, 0x16, 0x44, 0xFD, 0xAB, 0xCD,
  };
  TrackerId id = identify_tracker(sd, sizeof(sd));
  WL_CHECK(id.kind == TrackerKind::FindMyNetwork);
  WL_CHECK(id.status == TrackerStatus::Unknown);
  // Unknown status is treated conservatively -> worth flagging.
  WL_CHECK(is_unwanted_tracker(sd, sizeof(sd)));

  // Also recognized when 0xFD44 appears in a 16-bit UUID list (AD 0x03).
  static const uint8_t uuid_list[] = {
      0x02, 0x01, 0x06,
      0x03, 0x03, 0x44, 0xFD,
  };
  TrackerId id2 = identify_tracker(uuid_list, sizeof(uuid_list));
  WL_CHECK(id2.kind == TrackerKind::FindMyNetwork);
  WL_CHECK(is_unwanted_tracker(uuid_list, sizeof(uuid_list)));
}

WL_TEST(tracker_non_tracker_advert_is_none) {
  // A plain name-only beacon "Watch" - not a tracker at all.
  static const uint8_t name_only[] = {
      0x02, 0x01, 0x06,
      0x06, 0x09, 'W', 'a', 't', 'c', 'h',
  };
  TrackerId id = identify_tracker(name_only, sizeof(name_only));
  WL_CHECK(id.kind == TrackerKind::None);
  WL_CHECK(id.status == TrackerStatus::Unknown);
  WL_CHECK(!is_unwanted_tracker(name_only, sizeof(name_only)));

  // A non-Apple manufacturer beacon (Microsoft 0x0006) is not a tracker.
  static const uint8_t ms[] = { 0x05, 0xFF, 0x06, 0x00, 0x01, 0x02 };
  TrackerId idm = identify_tracker(ms, sizeof(ms));
  WL_CHECK(idm.kind == TrackerKind::None);
  WL_CHECK(!is_unwanted_tracker(ms, sizeof(ms)));

  // Apple company id but the wrong sub-type byte (0x10, not 0x12) -> not Find My.
  static const uint8_t apple_other[] = { 0x05, 0xFF, 0x4C, 0x00, 0x10, 0x02 };
  TrackerId ido = identify_tracker(apple_other, sizeof(apple_other));
  WL_CHECK(ido.kind == TrackerKind::None);
  WL_CHECK(!is_unwanted_tracker(apple_other, sizeof(apple_other)));
}

WL_TEST(tracker_short_findmy_payload_status_unknown) {
  // Apple offline-finding record too short to carry the status byte:
  //   04 FF 4C 00 12    company 0x004C, fm_type 0x12, but NO length/status.
  // adv_is_apple_findmy holds (payload[0]==0x12), so kind is AppleFindMy, but
  // there is no status byte to read -> status Unknown, and (conservatively)
  // still flagged as unwanted. Must NOT over-read past the 5-byte buffer.
  static const uint8_t shortfm[] = { 0x04, 0xFF, 0x4C, 0x00, 0x12 };
  TrackerId id = identify_tracker(shortfm, sizeof(shortfm));
  WL_CHECK(id.kind == TrackerKind::AppleFindMy);
  WL_CHECK(id.status == TrackerStatus::Unknown);
  WL_CHECK(is_unwanted_tracker(shortfm, sizeof(shortfm)));

  // fm_type + length but still no status byte (payload_len == 2 < 3).
  static const uint8_t nostatus[] = { 0x05, 0xFF, 0x4C, 0x00, 0x12, 0x19 };
  TrackerId idn = identify_tracker(nostatus, sizeof(nostatus));
  WL_CHECK(idn.kind == TrackerKind::AppleFindMy);
  WL_CHECK(idn.status == TrackerStatus::Unknown);
}

WL_TEST(tracker_truncated_and_garbage_no_crash) {
  // Null / zero-length buffers.
  WL_CHECK(identify_tracker(nullptr, 8).kind == TrackerKind::None);
  WL_CHECK(!is_unwanted_tracker(nullptr, 8));
  static const uint8_t dummy[1] = { 0x00 };
  WL_CHECK(identify_tracker(dummy, 0).kind == TrackerKind::None);
  WL_CHECK(!is_unwanted_tracker(dummy, 0));

  // A length byte that runs past the end of the buffer: adv_parser rejects the
  // malformed record, so we must classify as None with no over-read.
  static const uint8_t past_end[] = { 0x02, 0x01, 0x06, 0x1E, 0xFF, 0x4C, 0x00 };
  TrackerId id = identify_tracker(past_end, sizeof(past_end));
  WL_CHECK(id.kind == TrackerKind::None);
  WL_CHECK(!is_unwanted_tracker(past_end, sizeof(past_end)));

  // Pure garbage bytes must not be mistaken for a tracker.
  static const uint8_t garbage[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  WL_CHECK(identify_tracker(garbage, sizeof(garbage)).kind == TrackerKind::None);
  WL_CHECK(!is_unwanted_tracker(garbage, sizeof(garbage)));
}
