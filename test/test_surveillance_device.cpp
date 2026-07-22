// test_surveillance_device.cpp - host unit tests for the pure passive
// surveillance-device fingerprinter (src/detect/surveillance_device) and its
// threat_map aggregator mapping. Builds small real-ish BLE adverts (reusing the
// AD-structure layout the adv_parser tests use) and Wi-Fi AP sightings, and
// asserts: positive ID of each device class from a representative signature, that
// ordinary beacons / normal APs are NOT flagged, bounds-safety on garbage, and
// that a verdict feeds the Surveillance domain of the aggregator at the expected
// severity (class ceiling capped by confidence).
#include "wl_test.h"
#include "surveillance_device.h"
#include "threat_map.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace detect;

// --- BLE advert builders ----------------------------------------------------

// A single 16-bit service-data (AD 0x16) advert carrying `uuid` (LE) plus one
// data byte, prefixed by the standard flags record. Mirrors the FindMyNetwork
// fixture in test_tracker_ident.cpp.
static std::vector<uint8_t> make_service_data16_adv(uint16_t uuid) {
  std::vector<uint8_t> v = {0x02, 0x01, 0x06};   // flags
  v.push_back(0x04);                              // len: type + uuid(2) + data(1)
  v.push_back(0x16);                              // AD: service data 16-bit
  v.push_back(static_cast<uint8_t>(uuid & 0xFF)); // uuid LE lo
  v.push_back(static_cast<uint8_t>(uuid >> 8));   // uuid LE hi
  v.push_back(0xAB);                              // one data byte
  return v;
}

// A manufacturer-specific (AD 0xFF) advert with `company` (LE) and one payload
// byte.
static std::vector<uint8_t> make_manufacturer_adv(uint16_t company) {
  std::vector<uint8_t> v = {0x02, 0x01, 0x06};       // flags
  v.push_back(0x04);                                  // len: type + company(2)+1
  v.push_back(0xFF);                                  // AD: manufacturer
  v.push_back(static_cast<uint8_t>(company & 0xFF));  // company LE lo
  v.push_back(static_cast<uint8_t>(company >> 8));    // company LE hi
  v.push_back(0x01);                                  // one payload byte
  return v;
}

// A complete-local-name (AD 0x09) advert carrying `name`.
static std::vector<uint8_t> make_name_adv(const char* name) {
  std::vector<uint8_t> v = {0x02, 0x01, 0x06};   // flags
  const size_t n = std::strlen(name);
  v.push_back(static_cast<uint8_t>(n + 1));       // len: type + name bytes
  v.push_back(0x09);                              // AD: complete local name
  for (size_t i = 0; i < n; ++i) v.push_back(static_cast<uint8_t>(name[i]));
  return v;
}

static WifiApSighting make_ap(const uint8_t oui0, const uint8_t oui1,
                              const uint8_t oui2, const char* ssid) {
  WifiApSighting ap{};
  ap.bssid[0] = oui0; ap.bssid[1] = oui1; ap.bssid[2] = oui2;
  ap.bssid[3] = 0x11; ap.bssid[4] = 0x22; ap.bssid[5] = 0x33;
  std::memset(ap.ssid, 0, sizeof(ap.ssid));
  std::strncpy(ap.ssid, ssid, sizeof(ap.ssid) - 1);
  return ap;
}

// --- Positive IDs: one representative signature per device class ------------

WL_TEST(surv_tile_tracker_by_service_uuid) {
  // Tile 0xFEED (AUTHORITATIVE) -> BleTracker, High.
  auto v = make_service_data16_adv(kTileServiceUuidA);
  DeviceVerdict id = classify_ble(v.data(), v.size());
  WL_CHECK(id.cls == DeviceClass::BleTracker);
  WL_CHECK(id.conf == Confidence::High);

  // The alternate Tile UUID 0xFEEC is recognized the same way.
  auto v2 = make_service_data16_adv(kTileServiceUuidB);
  WL_CHECK(classify_ble(v2.data(), v2.size()).cls == DeviceClass::BleTracker);
}

WL_TEST(surv_samsung_smarttag_by_service_uuid) {
  // Samsung SmartTag 0xFD5A (AUTHORITATIVE) -> BleTracker, High.
  auto v = make_service_data16_adv(kSmartTagServiceUuid);
  DeviceVerdict id = classify_ble(v.data(), v.size());
  WL_CHECK(id.cls == DeviceClass::BleTracker);
  WL_CHECK(id.conf == Confidence::High);
}

WL_TEST(surv_gopro_action_cam_by_service_uuid) {
  // GoPro 0xFEA6 (AUTHORITATIVE) -> ActionCamera, High.
  auto v = make_service_data16_adv(kGoProServiceUuid);
  DeviceVerdict id = classify_ble(v.data(), v.size());
  WL_CHECK(id.cls == DeviceClass::ActionCamera);
  WL_CHECK(id.conf == Confidence::High);
}

WL_TEST(surv_camera_glasses_by_company_id) {
  // Meta / Luxottica / Snap company ids -> CameraGlasses, Medium (non-exclusive).
  const uint16_t ids[] = {kMetaCompanyId, kMetaTechCompanyId,
                          kLuxotticaCompanyId, kSnapCompanyId};
  for (uint16_t cid : ids) {
    auto v = make_manufacturer_adv(cid);
    DeviceVerdict id = classify_ble(v.data(), v.size());
    WL_CHECK(id.cls == DeviceClass::CameraGlasses);
    WL_CHECK(id.conf == Confidence::Medium);
  }
}

WL_TEST(surv_chipolo_tracker_by_name) {
  // Chipolo (HEURISTIC name) -> BleTracker, Low. Case-insensitive.
  auto v = make_name_adv("Chipolo_A1B2");
  DeviceVerdict id = classify_ble(v.data(), v.size());
  WL_CHECK(id.cls == DeviceClass::BleTracker);
  WL_CHECK(id.conf == Confidence::Low);
}

WL_TEST(surv_body_camera_by_name) {
  // Axon body cam (HEURISTIC name) -> BodyCamera, Low.
  auto v = make_name_adv("AXON-Body3");
  DeviceVerdict id = classify_ble(v.data(), v.size());
  WL_CHECK(id.cls == DeviceClass::BodyCamera);
  WL_CHECK(id.conf == Confidence::Low);
}

WL_TEST(surv_dji_action_cam_by_oui) {
  // DJI OUI 60:60:1F (AUTHORITATIVE IEEE registration) -> ActionCamera, High,
  // regardless of SSID.
  WifiApSighting ap = make_ap(0x60, 0x60, 0x1F, "some-random-name");
  DeviceVerdict id = classify_wifi(ap);
  WL_CHECK(id.cls == DeviceClass::ActionCamera);
  WL_CHECK(id.conf == Confidence::High);
}

WL_TEST(surv_gopro_action_cam_by_ssid) {
  // GoPro default AP SSID (HEURISTIC brand string) -> ActionCamera, Medium.
  WifiApSighting ap = make_ap(0x00, 0x11, 0x22, "GoPro12345678");
  DeviceVerdict id = classify_wifi(ap);
  WL_CHECK(id.cls == DeviceClass::ActionCamera);
  WL_CHECK(id.conf == Confidence::Medium);
}

WL_TEST(surv_hidden_camera_by_ssid) {
  // Wyze (HEURISTIC) -> HiddenCamera, Medium.
  WifiApSighting wyze = make_ap(0x00, 0x11, 0x22, "WyzeCam_1234");
  DeviceVerdict idw = classify_wifi(wyze);
  WL_CHECK(idw.cls == DeviceClass::HiddenCamera);
  WL_CHECK(idw.conf == Confidence::Medium);

  // Reolink (HEURISTIC) -> HiddenCamera, Medium.
  WifiApSighting reo = make_ap(0x00, 0x11, 0x22, "Reolink-CAM-01");
  WL_CHECK(classify_wifi(reo).cls == DeviceClass::HiddenCamera);

  // Generic "ipcam" is broad -> HiddenCamera, Low only.
  WifiApSighting ipc = make_ap(0x00, 0x11, 0x22, "my-ipcam");
  DeviceVerdict idc = classify_wifi(ipc);
  WL_CHECK(idc.cls == DeviceClass::HiddenCamera);
  WL_CHECK(idc.conf == Confidence::Low);
}

// --- Negative cases: ordinary traffic must NOT be flagged -------------------

WL_TEST(surv_ordinary_ble_beacon_not_flagged) {
  // A plain name-only beacon "Watch".
  auto watch = make_name_adv("Watch");
  WL_CHECK(classify_ble(watch.data(), watch.size()).cls == DeviceClass::None);

  // An unrelated manufacturer beacon (Microsoft 0x0006) - not a surveillance
  // device.
  auto ms = make_manufacturer_adv(0x0006);
  WL_CHECK(classify_ble(ms.data(), ms.size()).cls == DeviceClass::None);

  // An unrelated service UUID (Nordic UART 0x0001 stand-in) is inert.
  auto other = make_service_data16_adv(0x1234);
  WL_CHECK(classify_ble(other.data(), other.size()).cls == DeviceClass::None);
}

WL_TEST(surv_normal_ap_not_flagged) {
  // A normal home AP: unrelated OUI, ordinary SSID -> nothing.
  WifiApSighting home = make_ap(0xAA, 0xBB, 0xCC, "HomeNetwork");
  WL_CHECK(classify_wifi(home).cls == DeviceClass::None);

  // A hidden AP (empty SSID) with an unrelated OUI must not match on the empty
  // string.
  WifiApSighting hidden = make_ap(0xAA, 0xBB, 0xCC, "");
  WL_CHECK(classify_wifi(hidden).cls == DeviceClass::None);
  WL_CHECK(classify_wifi(hidden).conf == Confidence::None);
}

WL_TEST(surv_truncated_and_garbage_no_crash) {
  WL_CHECK(classify_ble(nullptr, 8).cls == DeviceClass::None);
  static const uint8_t dummy[1] = {0x00};
  WL_CHECK(classify_ble(dummy, 0).cls == DeviceClass::None);

  // A length byte that runs past the end: adv_parser rejects it, classify as
  // None with no over-read.
  static const uint8_t past_end[] = {0x02, 0x01, 0x06, 0x1E, 0x16, 0xED, 0xFE};
  WL_CHECK(classify_ble(past_end, sizeof(past_end)).cls == DeviceClass::None);

  // Pure garbage.
  static const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  WL_CHECK(classify_ble(garbage, sizeof(garbage)).cls == DeviceClass::None);
}

// --- Combined sighting: higher-confidence half wins -------------------------

WL_TEST(surv_combined_sighting_picks_stronger_half) {
  // A weak SSID guess (GoPro, Medium) plus a strong BLE UUID (Tile, High): the
  // High BLE verdict wins.
  auto tile = make_service_data16_adv(kTileServiceUuidA);
  WifiApSighting gopro = make_ap(0x00, 0x11, 0x22, "GoPro999");
  Sighting s{tile.data(), tile.size(), &gopro};
  DeviceVerdict id = classify(s);
  WL_CHECK(id.cls == DeviceClass::BleTracker);
  WL_CHECK(id.conf == Confidence::High);

  // Wi-Fi-only sighting routes through classify() too.
  Sighting wonly{nullptr, 0, &gopro};
  WL_CHECK(classify(wonly).cls == DeviceClass::ActionCamera);

  // Empty sighting -> None.
  Sighting none{nullptr, 0, nullptr};
  WL_CHECK(classify(none).cls == DeviceClass::None);
}

// --- Aggregator mapping (threat_map -> Surveillance domain) -----------------

WL_TEST(surv_map_severity_ceiling_and_confidence_cap) {
  // Class ceiling: HiddenCamera High > CameraGlasses/BodyCamera/BleTracker
  // Medium > ActionCamera Low, when confidence does not cap it.
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::None, Confidence::High})
           == Severity::None);
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::HiddenCamera, Confidence::High})
           == Severity::High);
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::CameraGlasses, Confidence::High})
           == Severity::Medium);
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::BleTracker, Confidence::High})
           == Severity::Medium);
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::ActionCamera, Confidence::High})
           == Severity::Low);

  // Confidence caps the ceiling: a Low-confidence hidden-camera guess is only
  // Severity::Low, not High.
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::HiddenCamera, Confidence::Low})
           == Severity::Low);
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::HiddenCamera, Confidence::Medium})
           == Severity::Medium);
  // No confidence -> no severity, whatever the class.
  WL_CHECK(severity_of(DeviceVerdict{DeviceClass::HiddenCamera, Confidence::None})
           == Severity::None);
}

WL_TEST(surv_map_feed_routes_to_surveillance_domain) {
  ThreatState ts;
  // A confirmed hidden camera (High) reports High under the Surveillance domain.
  feed(ts, DeviceVerdict{DeviceClass::HiddenCamera, Confidence::High}, 10);
  WL_CHECK(ts.domain_severity(ThreatDomain::Surveillance) == Severity::High);
  WL_CHECK(ts.dominant() == ThreatDomain::Surveillance);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  WL_CHECK_EQ(ts.active_mask(),
              (uint8_t)(1u << (uint8_t)ThreatDomain::Surveillance));

  // A None verdict leaves the domain clear.
  ThreatState ts2;
  feed(ts2, DeviceVerdict{DeviceClass::None, Confidence::None}, 10);
  WL_CHECK(ts2.domain_severity(ThreatDomain::Surveillance) == Severity::None);
  WL_CHECK(ts2.level() == ThreatLevel::Calm);
}

WL_TEST(surv_map_end_to_end_classify_then_feed) {
  // Full path: a real Tile advert -> classify -> feed -> aggregator reflects it.
  auto tile = make_service_data16_adv(kTileServiceUuidA);
  DeviceVerdict v = classify_ble(tile.data(), tile.size());
  ThreatState ts;
  feed(ts, v, 50);
  // BleTracker ceiling Medium, High confidence -> Medium (Alert) on its own.
  WL_CHECK(ts.domain_severity(ThreatDomain::Surveillance) == Severity::Medium);
  WL_CHECK(ts.level() == ThreatLevel::Alert);
}
