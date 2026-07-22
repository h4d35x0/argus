// surveillance_device.h - pure, host-testable PASSIVE surveillance-device
// fingerprinter.
//
// DEFENSIVE ONLY. This module transmits nothing, probes nothing, and attacks
// nothing. It answers ONE question about a SINGLE sighting the radios ALREADY
// observed: "does this BLE advertisement and/or this Wi-Fi access point match a
// known surveillance / recording device, and of what class?" It is the wearer's
// passive "am I being recorded / tracked" awareness feature and complements the
// AirTag-focused tracker_ident (which handles Apple Find My); this module covers
// the NON-Find-My surveillance surface: camera glasses, body cameras, hidden
// Wi-Fi cameras, action cams / drones, and non-Apple commercial BLE trackers.
//
// It COMPOSES with the shared BLE AD parser (adv_parser.h): it never re-walks
// raw advertising bytes by hand, delegating every byte fetch to that bounds-
// checked parser exactly as tracker_ident / ble_spam do. The Wi-Fi half keys off
// only the two fields a passive scan already yields: the BSSID (its first three
// bytes are the OUI) and the advertised SSID.
//
// Design constraints (deliberate, matches adv_parser.h / the other detectors):
//   * Pure C++11/14. NO Arduino.h, NO ESP-IDF, NO LVGL, NO hardware. Compiles
//     and runs on the host g++ so the logic is unit-tested in milliseconds.
//   * Stateless. No clock, no dynamic allocation, fixed const signature tables.
//     Every call is a pure function of the sighting handed in; the caller owns
//     the buffers. (tracker_ident is stateless the same way; a follow-over-time
//     judgment, if ever wanted, is tail_detect's job, not this per-sighting gate.)
//   * Bounds-safe. A short / truncated / garbage payload or an empty SSID returns
//     a clean {None, None} and never over-reads.
//
// SIGNATURE HONESTY (read before trusting a verdict). Each signature below is
// tagged AUTHORITATIVE (a verified, device-specific constant) or HEURISTIC (an
// approximate / shared / name-based signal to verify against a real capture):
//   * Tile        service UUID 0xFEED / 0xFEEC - AUTHORITATIVE (Bluetooth SIG
//                 16-bit UUIDs assigned to Tile, Inc.).
//   * Samsung     SmartTag find service UUID 0xFD5A - AUTHORITATIVE (Samsung
//                 Electronics; SmartTag / SmartTag2 broadcast it).
//   * GoPro       service UUID 0xFEA6 - AUTHORITATIVE (Bluetooth SIG UUID
//                 assigned to GoPro, Inc.; GoPro's own OpenGoPro docs use it).
//   * DJI         Wi-Fi OUIs - AUTHORITATIVE (IEEE MA-L blocks registered to SZ
//                 DJI Technology Co., Ltd).
//   * Camera-     BLE company ids 0x01AB / 0x058E (Meta), 0x0D53 (Luxottica),
//     glasses     0x03C2 (Snap) - PARTIALLY AUTHORITATIVE company ids, but these
//                 vendors also ship non-camera BLE gear (e.g. Meta Quest), so a
//                 company-id hit alone is only MEDIUM confidence, not proof.
//   * Tile mfr    company id 0x00C7 - HEURISTIC (secondary corroboration only;
//                 the 0xFEED service UUID is the strong Tile signal).
//   * Chipolo     local-name "Chipolo" - HEURISTIC (Find My Chipolos are caught
//                 by tracker_ident's 0x004C path; classic Chipolos have no public
//                 dedicated UUID, so we fall back to the advertised name).
//   * Body cams   name / SSID patterns "Axon" / "WatchGuard" - HEURISTIC (the
//                 generic Nordic/BLE chipset ids these use are not vendor-unique,
//                 so a name match is the only low-false-positive passive signal).
//   * Hidden cams SSID substrings "Wyze" / "Reolink" / "ipcam" - HEURISTIC
//                 (setup-AP / default-SSID strings; verify per model).
//   * Action cams SSID substrings "GoPro" / "DJI" / "Insta360" - HEURISTIC-but-
//                 reasonable (user-facing brand strings in the default AP name).
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// The coarse class of a recognized surveillance / recording device.
enum class DeviceClass : uint8_t {
  None = 0,       // no surveillance-device signature matched
  CameraGlasses,  // camera-equipped smart glasses (Meta Ray-Ban / Oakley, Snap)
  BodyCamera,     // body-worn camera (Axon, Motorola/WatchGuard, Digital Ally)
  HiddenCamera,   // concealed Wi-Fi / IP camera (Wyze, Reolink, generic IP-cam)
  ActionCamera,   // action cam / camera drone (GoPro, Insta360, DJI)
  BleTracker,     // non-Apple commercial BLE tracker (Tile, SmartTag, Chipolo)
};

// How strong the matched signature is. See the SIGNATURE HONESTY note above: a
// dedicated device-specific service UUID / OUI is High; a shared-vendor company
// id / OUI is Medium; a name / SSID substring guess is Low. The aggregator
// mapping (threat_map) caps a class's severity by this confidence so a weak
// SSID guess never drives the same alarm as a verified UUID.
enum class Confidence : uint8_t {
  None = 0,
  Low,     // a name / SSID substring, or another weak heuristic
  Medium,  // a solid but non-exclusive signal (shared-vendor company id / OUI)
  High,    // an exclusive, verified signature (dedicated service UUID / OUI)
};

// The verdict for one sighting.
struct DeviceVerdict {
  DeviceClass cls;
  Confidence  conf;
};

// A Wi-Fi access-point sighting reduced from the on-device scan / beacon path.
//   bssid - the 6-byte AP MAC; bssid[0..2] is the OUI. The caller copies this
//           from the scan result; this module never stores the pointer.
//   ssid  - the advertised network name, NUL-terminated. "" means hidden /
//           broadcast-suppressed and is treated as "no SSID" (never a match).
struct WifiApSighting {
  uint8_t bssid[6];
  char    ssid[33];
};

// A combined sighting: a BLE half and/or a Wi-Fi half. Either may be absent, so
// the same entry point serves the BLE scan callback, the Wi-Fi beacon callback,
// or a correlated both-radios sighting.
//   adv / adv_len - the BLE advertising payload VIEW, or {nullptr, 0} for none.
//   wifi          - a Wi-Fi AP, or nullptr for none.
struct Sighting {
  const uint8_t*        adv;
  size_t                adv_len;
  const WifiApSighting* wifi;
};

// Classify a lone BLE advertising payload [adv, adv+len). Returns {None, None}
// for a null / empty / truncated / non-matching advert (adv_parser rejects
// malformed records, so this never over-reads).
DeviceVerdict classify_ble(const uint8_t* adv, size_t len);

// Classify a lone Wi-Fi AP sighting. Returns {None, None} for an AP whose OUI and
// SSID match nothing (an empty SSID matches nothing on its own).
DeviceVerdict classify_wifi(const WifiApSighting& ap);

// Classify a combined sighting: the higher-confidence of its BLE and Wi-Fi
// halves wins (a BLE tie is preferred as the more specific radio). Absent halves
// contribute nothing. This is the single entry point the future scan-loop
// integration calls per sighting.
DeviceVerdict classify(const Sighting& s);

// --- Exposed signature constants (also used by the host tests) --------------

// Tile 16-bit BLE service UUIDs (Bluetooth SIG, Tile, Inc.). AUTHORITATIVE.
static const uint16_t kTileServiceUuidA = 0xFEED;
static const uint16_t kTileServiceUuidB = 0xFEEC;
// Tile BLE manufacturer company id. HEURISTIC (secondary corroboration only).
static const uint16_t kTileCompanyId = 0x00C7;

// Samsung SmartTag "Find" 16-bit service UUID. AUTHORITATIVE.
static const uint16_t kSmartTagServiceUuid = 0xFD5A;

// GoPro 16-bit BLE service UUID (Bluetooth SIG, GoPro, Inc.). AUTHORITATIVE.
static const uint16_t kGoProServiceUuid = 0xFEA6;

// Camera-glasses BLE manufacturer company ids. Company ids are real Bluetooth
// SIG assignments, but these vendors ship non-camera BLE gear too, so a hit is
// MEDIUM confidence (see SIGNATURE HONESTY above), never proof of a camera.
static const uint16_t kMetaCompanyId       = 0x01AB;  // Meta Platforms, Inc.
static const uint16_t kMetaTechCompanyId   = 0x058E;  // Meta Platforms Technologies
static const uint16_t kLuxotticaCompanyId  = 0x0D53;  // Luxottica (Ray-Ban maker)
static const uint16_t kSnapCompanyId       = 0x03C2;  // Snap, Inc. (Spectacles)

}  // namespace detect
