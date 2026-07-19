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
6. **threat_state -> UI** - once per UI tick call `threat.tick(now)` then read
   `threat.level()`; drive `argus_accent()` (Calm=steel-blue, Alert/Critical=HADES
   red) and a HexHound reaction; use `dominant()` + `active_mask()` for the
   headline / which-threat glyphs. `threat_state` already handles rise/decay
   hysteresis and correlated-threat escalation, so the UI just reflects `level()`.

## Tests

`bash test/run.sh` (host g++; `make`/cmake not installed on the MSI - see the
project memory). Current: 102 tests / 944 checks, all green. Each module has its
own `test/test_<module>.cpp`; add cases there, keep everything pure.
