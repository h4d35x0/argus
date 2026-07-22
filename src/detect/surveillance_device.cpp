// surveillance_device.cpp - implementation of the pure passive surveillance-
// device fingerprinter. See surveillance_device.h for the contract and the
// per-signature AUTHORITATIVE vs HEURISTIC provenance tags.
//
// This module inspects NO raw AD bytes on its own: every BLE fetch is delegated
// to the shared, bounds-checked BLE AD parser (adv_parser.h), matching
// tracker_ident / ble_spam. The Wi-Fi half uses only the BSSID OUI and the SSID.
#include "surveillance_device.h"

// Relative path so this resolves in BOTH the firmware build (PlatformIO puts
// only src/ on the include path, not src/detect/ or src/ble/) and the host test
// harness. Same convention as tracker_ident.cpp / ble_spam.cpp.
#include "../ble/adv_parser.h"

#include <cstring>  // strlen

namespace detect {

// Keep the best (highest-confidence) verdict seen so far. Ties do NOT upgrade,
// so the first, most-specific signature checked for a given confidence wins.
static void consider(DeviceVerdict& best, DeviceClass cls, Confidence conf) {
  if (static_cast<uint8_t>(conf) > static_cast<uint8_t>(best.conf)) {
    best.cls  = cls;
    best.conf = conf;
  }
}

// True if the advert carries 16-bit service `uuid` either as service data
// (AD 0x16) or in a 16-bit service UUID list (AD 0x02/0x03). Tile/GoPro/SmartTag
// all surface their UUID through one of these; check both, like tracker_ident.
static bool has_service_uuid16(const uint8_t* adv, size_t len, uint16_t uuid) {
  return ble::adv_find_service_data16(adv, len, uuid, nullptr, nullptr) ||
         ble::adv_has_service_uuid16(adv, len, uuid);
}

// ASCII lowercase of one byte (locale-free; the SSID is raw bytes on the air).
static char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive: does NUL-terminated `hay` contain the lowercase `needle`?
// `needle` MUST already be lowercase. Bounds are the hay's own NUL; an empty
// hay or needle never matches. Small O(n*m) scan - hay is <= 32 bytes.
static bool contains_ci(const char* hay, const char* needle) {
  if (!hay || !needle || hay[0] == '\0' || needle[0] == '\0') return false;
  const size_t nlen = strlen(needle);
  for (size_t i = 0; hay[i] != '\0'; ++i) {
    size_t j = 0;
    while (j < nlen && hay[i + j] != '\0' &&
           lower_ascii(hay[i + j]) == needle[j]) {
      ++j;
    }
    if (j == nlen) return true;
  }
  return false;
}

DeviceVerdict classify_ble(const uint8_t* adv, size_t len) {
  DeviceVerdict best{DeviceClass::None, Confidence::None};
  if (!adv || len == 0) return best;

  // --- Dedicated service UUIDs: exclusive, verified -> High -----------------
  // Tile (0xFEED / 0xFEEC), Samsung SmartTag (0xFD5A), GoPro (0xFEA6). These are
  // device-specific and are the strongest passive signals available.
  if (has_service_uuid16(adv, len, kTileServiceUuidA) ||
      has_service_uuid16(adv, len, kTileServiceUuidB)) {
    consider(best, DeviceClass::BleTracker, Confidence::High);
  }
  if (has_service_uuid16(adv, len, kSmartTagServiceUuid)) {
    consider(best, DeviceClass::BleTracker, Confidence::High);
  }
  if (has_service_uuid16(adv, len, kGoProServiceUuid)) {
    consider(best, DeviceClass::ActionCamera, Confidence::High);
  }

  // --- Manufacturer company id: shared-vendor -> Medium / heuristic ---------
  uint16_t company = 0;
  if (ble::adv_manufacturer_company_id(adv, len, &company)) {
    // Camera-glasses vendors. Real company ids, but non-exclusive (Meta also
    // makes Quest, etc.), so Medium confidence, never proof of a camera.
    if (company == kMetaCompanyId || company == kMetaTechCompanyId ||
        company == kLuxotticaCompanyId || company == kSnapCompanyId) {
      consider(best, DeviceClass::CameraGlasses, Confidence::Medium);
    }
    // Tile manufacturer id: secondary corroboration only (the UUID above is the
    // strong signal). HEURISTIC -> Medium, and consider() will not override an
    // already-High Tile UUID match.
    if (company == kTileCompanyId) {
      consider(best, DeviceClass::BleTracker, Confidence::Medium);
    }
  }

  // --- Local-name heuristics: weak -> Low -----------------------------------
  // Chipolo (no public dedicated UUID for classic models) and body cameras
  // (generic chipset ids are not vendor-unique) fall back to the advertised
  // name. adv_local_name is bounds-checked and always NUL-terminates.
  char name[33];
  if (ble::adv_local_name(adv, len, name, sizeof(name))) {
    if (contains_ci(name, "chipolo")) {
      consider(best, DeviceClass::BleTracker, Confidence::Low);
    }
    if (contains_ci(name, "axon") || contains_ci(name, "watchguard")) {
      consider(best, DeviceClass::BodyCamera, Confidence::Low);
    }
  }

  return best;
}

// DJI Wi-Fi OUIs (first 3 MAC bytes), IEEE MA-L blocks registered to SZ DJI
// Technology Co., Ltd. AUTHORITATIVE. A DJI drone / camera AP's BSSID starts
// with one of these.
static const uint8_t kDjiOuis[][3] = {
    {0x04, 0xA8, 0x5A}, {0x0C, 0x9A, 0xE6}, {0x34, 0xD2, 0x62},
    {0x48, 0x1C, 0xB9}, {0x4C, 0x43, 0xF6}, {0x58, 0xB8, 0x58},
    {0x60, 0x60, 0x1F}, {0x88, 0x29, 0x85}, {0x8C, 0x58, 0x23},
    {0xE4, 0x7A, 0x2C},
};

static bool oui_matches(const uint8_t bssid[6], const uint8_t oui[3]) {
  return bssid[0] == oui[0] && bssid[1] == oui[1] && bssid[2] == oui[2];
}

DeviceVerdict classify_wifi(const WifiApSighting& ap) {
  DeviceVerdict best{DeviceClass::None, Confidence::None};

  // --- OUI: verified IEEE registration -> High ------------------------------
  for (size_t i = 0; i < sizeof(kDjiOuis) / sizeof(kDjiOuis[0]); ++i) {
    if (oui_matches(ap.bssid, kDjiOuis[i])) {
      consider(best, DeviceClass::ActionCamera, Confidence::High);
      break;
    }
  }

  // --- SSID substrings: brand / default-AP strings ---------------------------
  // Action cams / drones. HEURISTIC-but-reasonable: these are the user-facing
  // brand strings the devices put in their default AP name.
  const char* ssid = ap.ssid;
  if (contains_ci(ssid, "gopro")) {
    consider(best, DeviceClass::ActionCamera, Confidence::Medium);
  }
  if (contains_ci(ssid, "dji") || contains_ci(ssid, "mavic") ||
      contains_ci(ssid, "osmo") || contains_ci(ssid, "avata")) {
    consider(best, DeviceClass::ActionCamera, Confidence::Medium);
  }
  if (contains_ci(ssid, "insta360")) {
    consider(best, DeviceClass::ActionCamera, Confidence::Medium);
  }

  // Hidden / IP cameras. HEURISTIC: setup-AP / default SSID strings.
  if (contains_ci(ssid, "wyze")) {
    consider(best, DeviceClass::HiddenCamera, Confidence::Medium);
  }
  if (contains_ci(ssid, "reolink")) {
    consider(best, DeviceClass::HiddenCamera, Confidence::Medium);
  }
  // Generic IP-cam strings are broad, so Low confidence only.
  if (contains_ci(ssid, "ipcam") || contains_ci(ssid, "ip-cam") ||
      contains_ci(ssid, "ipcamera")) {
    consider(best, DeviceClass::HiddenCamera, Confidence::Low);
  }

  // Body cameras. HEURISTIC: name patterns are the only low-false-positive
  // passive signal (their chipset ids are not vendor-unique).
  if (contains_ci(ssid, "axon") || contains_ci(ssid, "watchguard")) {
    consider(best, DeviceClass::BodyCamera, Confidence::Low);
  }

  return best;
}

DeviceVerdict classify(const Sighting& s) {
  DeviceVerdict best{DeviceClass::None, Confidence::None};

  // BLE first (the more specific radio wins a confidence tie via consider()).
  if (s.adv && s.adv_len > 0) {
    const DeviceVerdict b = classify_ble(s.adv, s.adv_len);
    consider(best, b.cls, b.conf);
  }
  if (s.wifi) {
    const DeviceVerdict w = classify_wifi(*s.wifi);
    consider(best, w.cls, w.conf);
  }
  return best;
}

}  // namespace detect
