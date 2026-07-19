// tracker_ident.cpp - implementation of the pure "unwanted tracker" advert
// classifier. See tracker_ident.h for the contract, the exact status byte/bit
// read, its reverse-engineering provenance, and the honest MAC-rotation limit.
//
// This module inspects NO raw AD bytes on its own: every fetch is delegated to
// the shared, bounds-checked BLE AD parser (adv_parser.h). The only direct byte
// read is the Apple offline-finding status byte, and it is guarded by an
// explicit length re-check before indexing.
#include "tracker_ident.h"

// Relative path so this resolves in BOTH the firmware build (PlatformIO only
// puts src/ on the include path, not src/detect/ or src/ble/) and the host test
// harness. The host Makefile/run.sh also add -I src/ble, but the relative form
// needs no flag. Same convention as ble_spam.cpp.
#include "../ble/adv_parser.h"

namespace detect {

// Byte offsets INTO the Apple manufacturer payload (the value bytes AFTER the
// 2-byte company id that ble::adv_manufacturer strips). See tracker_ident.h.
static const size_t kFindMyTypeOffset   = 0;  // 0x12 offline-finding type
static const size_t kFindMyLenOffset    = 1;  // length byte (0x19 for AirTag)
static const size_t kFindMyStatusOffset = 2;  // status byte we decode

TrackerId identify_tracker(const uint8_t* adv, size_t len) {
  TrackerId out{TrackerKind::None, TrackerStatus::Unknown};
  if (!adv || len == 0) return out;

  // --- Apple offline-finding (AirTag / iPhone / Find My accessory) ----------
  // adv_is_apple_findmy is the loose Find My predicate: an Apple (0x004C)
  // manufacturer record whose first post-company-id byte is the offline-finding
  // type (0x12). It already bounds-checks the manufacturer record.
  if (ble::adv_is_apple_findmy(adv, len)) {
    out.kind = TrackerKind::AppleFindMy;

    // Re-fetch the manufacturer payload to reach the status byte. adv_is_apple_
    // findmy guaranteed a valid Apple record with payload[0] == 0x12, but we
    // must still verify the payload is long enough to carry payload[2] before
    // indexing it - a truncated offline-finding advert (or the short "nearby"
    // form) leaves the status Unknown rather than over-reading.
    uint16_t       company = 0;
    const uint8_t* payload = nullptr;
    size_t         plen    = 0;
    if (ble::adv_manufacturer(adv, len, &company, &payload, &plen) &&
        company == ble::APPLE_COMPANY_ID &&
        payload != nullptr &&
        plen > kFindMyStatusOffset &&
        payload[kFindMyTypeOffset] == ble::APPLE_FINDMY_TYPE) {
      // Decode the maintained / owner-connected bit (see header for the exact
      // bit and its provenance). Set -> owner nearby; clear -> separated.
      const uint8_t status = payload[kFindMyStatusOffset];
      out.status = (status & kFindMyStatusMaintainedBit)
                       ? TrackerStatus::OwnerNearby
                       : TrackerStatus::Separated;
    }
    // (void) the length byte offset - documented for the reader, not needed to
    // reach the status byte since the >= 3 length guard already covers it.
    (void)kFindMyLenOffset;
    return out;
  }

  // --- Find My Network service advert (third-party accessory) ---------------
  // Recognized via the 16-bit Find My Network service UUID 0xFD44, either as
  // service data (AD 0x16) or listed in a 16-bit service UUID list. We do not
  // decode third-party owner-state here, so status stays Unknown.
  if (ble::adv_find_service_data16(adv, len, kFindMyNetworkServiceUuid,
                                   nullptr, nullptr) ||
      ble::adv_has_service_uuid16(adv, len, kFindMyNetworkServiceUuid)) {
    out.kind   = TrackerKind::FindMyNetwork;
    out.status = TrackerStatus::Unknown;
    return out;
  }

  return out;  // {None, Unknown}
}

bool is_unwanted_tracker(const uint8_t* adv, size_t len) {
  const TrackerId id = identify_tracker(adv, len);
  if (id.kind == TrackerKind::None) return false;
  // Flag anything that is not positively OwnerNearby: a Separated tag (lost
  // mode) OR an Unknown-status tracker (be conservative). A tracker whose owner
  // is demonstrably present is not treated as a plant.
  return id.status != TrackerStatus::OwnerNearby;
}

}  // namespace detect
