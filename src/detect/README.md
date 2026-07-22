# ARGUS detection subsystem

Pure, host-tested, DEFENSIVE-ONLY threat detection. No module here transmits or
attacks anything - they only classify what the radios already observe. Every
module is free of Arduino / LVGL / ESP-IDF / clock / GPS dependencies (time and
location are passed in), so the whole subsystem builds and runs under the host
g++ harness (`bash test/run.sh`).

## Data flow

```
  radios / scans (on-device)                        this subsystem (pure)                 UI (on-device)
  --------------------------                        ---------------------                 --------------
  wifi_beacon_manager  --(beacon: ssid/bssid/auth)--> evil_twin    --RogueFlag--\
  wifi_beacon_manager  --(beacon: bssid/ssid/chan)--> beacon_flood --BeaconFlag-\
  wifi promiscuous mgmt --(deauth/disassoc frame)---> deauth_flood --DeauthFlag-\
  ble_scan_manager     --(adv bytes)--> [ble/adv_parser] --> ble_spam --SpamFlag-+--> threat_map.feed() --> threat_state
  ble/wifi scans + GNSS --(sighting: id/cell/time)--> tail_detect  --TailFlag----/                              |
                                                                                                    level()/dominant()/active_mask()
                                                                                                               |
                                                                                            argus_accent() (HADES red) + HexHound
```

## Modules

| File | Role | Key input | Verdict |
|------|------|-----------|---------|
| `../ble/adv_parser.*` | Bounds-checked BLE AD (TLV) parser | raw adv bytes | parsed fields (name, mfr id, service data/UUID) |
| `evil_twin.*` | Rogue-AP / evil-twin | AP beacons (bssid/ssid/channel/auth) | `RogueFlag` |
| `tail_detect.*` | Anti-stalking follow detection | device sightings (id/time/coarse cell) | `TailFlag` |
| `deauth_flood.*` | Deauth/disassoc flood | mgmt-frame events (type/bssid/time) | `DeauthFlag` |
| `ble_spam.*` | BLE-spam flood (Flipper etc.) | BLE adv observations | `SpamFlag` |
| `beacon_flood.*` | WiFi beacon-flood / fake-AP spam | AP beacons (bssid/ssid/channel) | `BeaconFlag` |
| `tracker_ident.*` | AirTag / Find My tracker payload ident | one BLE adv | `TrackerId` + `is_unwanted_tracker()` |
| `surveillance_device.*` | Passive surveillance-device ident (camera glasses / body cams / hidden + action cams / non-Apple BLE trackers) | one BLE adv and/or one WiFi AP | `DeviceVerdict` {`DeviceClass`, `Confidence`} |
| `../geo_cell.*` | GPS lat/lon -> coarse cell id | a WGS84 fix | `int32` cell |
| `threat_map.*` | Verdict -> unified Severity + `feed()` | any detector flag | reports to aggregator |
| `threat_state.*` | Aggregator: unified posture | `(domain, severity, time)` | `ThreatLevel` + dominant/active_mask |

Design note: `threat_state` is deliberately decoupled (it includes no detector
header); `threat_map` is the ONE place that couples detector enums to the shared
`Severity`/`ThreatDomain` vocabulary. Keep new detectors the same shape: a pure
module returning its own flag, plus a `severity_of()` + `feed()` overload here.

## Integration guide (on-device, hardware-gated)

Each step is separate: wire ONE, flash, verify boot + behavior, then the next.
Do not batch-wire blind.

1. **BLE adv parser** - refactor `airtag.cpp` / `flipper.cpp` / skimmer detectors
   to call `ble::adv_*` instead of re-walking raw bytes. Behavior-neutral refactor.
2. **evil_twin** - in the beacon path (`wifi_beacon_manager`), build an
   `ApObservation` per beacon (map the ESP auth string/const to `AuthMode`), call
   `RogueApDetector::ingest`, then `detect::feed(threat, verdict.flag, now)`.
3. **deauth_flood** - in the promiscuous mgmt handler, reduce each frame to a
   `MgmtFrameEvent` (subtype 0xC=deauth / 0xA=disassoc, source = addr3), call
   `ingest`, and `tick(now)` on the 1 Hz cadence, then `feed()`.
4. **ble_spam** - in `ble_scan_manager` results, build `BleAdvObservation`
   (addr + raw adv + len), `ingest`, `tick(now)`, `feed()`.
   **beacon_flood** - in the same beacon path as evil_twin, build a
   `BeaconObservation` (bssid/ssid/channel), `ingest`, `tick(now)`, `feed()`.
5. **tail_detect** - quantize the current GNSS fix with `geo::coarse_cell(lat,
   lon)` (src/geo_cell.*, ~120 m default cell) to get the integer cell; for each
   BLE/WiFi device seen, `ingest({device_id, now, cell, rssi})`, `decay(now)` on
   a slow cadence, `feed()`. Tune the cell size to the deployment (tighter for a
   mall/school, wider for a road trip) - it sets how far a follower must move to
   count as a new cell.
7. **tracker_ident (Airtag domain)** - for each BLE adv, if
   `is_unwanted_tracker(adv, len)`, feed a sighting `{addr, now, geo::coarse_cell,
   rssi}` into a SEPARATE `TailDetector` instance (distinct from step 5's), then
   `detect::feed_tracker(threat, verdict.flag, now)` -> reports under the Airtag
   domain. Reuses the follow engine but gated to Find My / AirTag payloads, so a
   tracker physically following the wearer is flagged distinctly. Note: AirTag
   MACs rotate ~15 min when separated, so attribution is best-effort (documented
   in tracker_ident.h); the follow signal is the actionable part.
8. **threat_state -> UI** - once per UI tick call `threat.tick(now)` then read
   `threat.level()`; drive `argus_accent()` (Calm=steel-blue, Alert/Critical=HADES
   red) and a HexHound reaction; use `dominant()` + `active_mask()` for the
   headline / which-threat glyphs. `threat_state` already handles rise/decay
   hysteresis and correlated-threat escalation, so the UI just reflects `level()`.
9. **threat_log (forensic history)** - each cycle, after tick(), loop EVERY domain
   `for d in 0..ThreatDomain::_Count: log.update(d, threat.domain_severity(d), now)`
   and, when update() returns true, append `ThreatLog::format(event)` to
   /Settings/threat_log.txt (guard isCardReady()/usb_sd_is_running()).
   GOTCHA (proved by test_subsystem_e2e): you MUST poll EVERY domain every cycle,
   not just the one that changed - the log's first update() per domain sets its
   None baseline silently, so a domain that goes hot before its first poll would
   have that rise swallowed. Polling all domains from boot (all None) avoids it.

## Tests

`bash test/run.sh` (host g++; `make`/cmake not installed on the MSI - see the
project memory). Current: ~142 tests / 1541 checks, all green. Each module has its
own `test/test_<module>.cpp`, plus `test_subsystem_e2e.cpp` which drives real
detector inputs through map -> aggregator -> log/Airtag to prove they COMPOSE:
an evil twin + concurrent deauth flood escalating to Critical then decaying; and
an AirTag following across geo cells escalating the Airtag domain.

The e2e test has already caught TWO real cross-module integration bugs that no
unit test could (each module passed its own tests): (1) the forensic log must be
polled for EVERY domain each cycle or a first-rise is swallowed as baseline;
(2) geo::coarse_cell must be non-negative or tail_detect drops it as "unknown".
When wiring the hardware integration, extend the e2e first - it is cheaper than a
flash cycle. Keep everything pure.

## INTEGRATION (hardware-gated) - surveillance_device

`surveillance_device` is a PURE, stateless per-sighting fingerprinter (like
tracker_ident): nothing calls it yet. It reports under the new
`ThreatDomain::Surveillance` (see threat_state.h) via
`detect::feed(threat, verdict, now)` (threat_map.h). Wiring it is a SEPARATE,
hardware-gated step - do it as its own flash+verify, not batched:

1. **BLE half** - in `ble_scan_manager` / the GAP scan callback (the same place
   ble_spam and tracker_ident are fed - `src/flipper.cpp`'s `on_scan_result`
   already exposes `res.ble_adv` + `res.adv_data_len + res.scan_rsp_len`), call
   `DeviceVerdict v = detect::classify_ble(adv, adv_len);` and, when
   `v.cls != DeviceClass::None`, `detect::feed(threat, v, now_sec)`.
2. **WiFi half** - in the beacon path (`wifi_beacon_manager`, where evil_twin /
   beacon_flood are fed), build a `WifiApSighting{bssid, ssid}` from the beacon
   (copy the 6-byte BSSID - `bssid[0..2]` is the OUI - and the SSID string), call
   `detect::classify_wifi(ap)`, and `feed()` the verdict the same way. For a
   sighting seen on BOTH radios, `detect::classify(Sighting{adv, adv_len, &ap})`
   folds them and keeps the higher-confidence half.
3. **Aggregator / UI** - no change: `Surveillance` is just another domain the
   existing `threat.tick()` / `level()` / `dominant()` / `active_mask()` and the
   forensic `threat_log` (which now names the domain "Surveillance") already
   fold in, driving `argus_accent()` + HexHound like every other domain.

Signature provenance (which constants are verified vs heuristic-to-verify) is
documented in surveillance_device.h under "SIGNATURE HONESTY"; verify the
HEURISTIC ones against a real capture before treating a low-confidence verdict as
actionable. Extend `test_subsystem_e2e.cpp` with a Surveillance path before
wiring hardware.
