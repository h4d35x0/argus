// beacon_flood.h - pure, host-testable WiFi BEACON-FLOOD / FAKE-AP-SPAM DETECTION.
//
// DEFENSIVE ONLY. This module never transmits, beacons, spoofs, or attacks
// anything. It ingests a stream of OBSERVED 802.11 beacon frames (as the
// on-device promiscuous-mode sniffer already receives them) and classifies
// whether a nearby beacon-flooding attack is in progress - i.e. mdk3/mdk4
// beacon mode, a Flipper Zero WiFi board, or an "SSID flooder" broadcasting a
// storm of fake APs with random BSSIDs/SSIDs to bury the real APs and overwhelm
// scanners. It reads the air and raises a verdict; it produces no radio output.
//
// This is the WiFi-side analog of the BLE advertisement-spam detector
// (src/detect/ble_spam): both watch the DISTINCT-entity rate inside a short
// sliding window, because a spam tool sprays a continuous stream of FRESH
// (usually randomized) identities while a normal environment holds a stable
// handful of real transmitters. The BLE side counts distinct advertiser
// addresses; this side counts distinct AP BSSIDs.
//
// On-device grounding (confirmed, not assumed): src/wifi_beacon_manager.cpp
// runs the radio in STA + promiscuous and forwards every WIFI_PKT_MGMT beacon
// (frame control byte high nibble 0x80) after reducing it to a WifiBeacon
// { uint8_t bssid[6]; char ssid[33]; ...; int8_t rssi; uint8_t channel; }. The
// BSSID is bytes 16..21 of the MAC header (addr3); the SSID comes from tagged
// element id 0. Reducing one such WifiBeacon to a BeaconObservation (copy the
// BSSID, copy the SSID, carry the channel, stamp the seconds, carry rssi) is a
// trivial parse that lives at the device-only call site, NOT here. This module
// stays hardware-free so the decision logic is unit-testable on the host.
//
// Self-contained by design: it defines its own observation struct and its own
// flag enum, includes only standard headers, and uses integer math only. No
// Arduino.h, no esp_wifi, no LVGL, no clock, no dynamic allocation. Time arrives
// as a plain t_sec on every observation (and to tick()), so the whole decision
// is deterministic and reproducible off-device.
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// One observed WiFi beacon, as the on-device promiscuous path would reduce it.
// Field names/shape mirror evil_twin's ApObservation (bssid[6], ssid[33],
// channel) so the same device-side reduction feeds both detectors.
//   bssid   - the 6-byte AP address (addr3 of the beacon MAC header).
//   ssid    - NUL-terminated network name; "" means hidden / suppressed.
//   channel - 1..14; 0 == unknown. Carried for telemetry; not in the decision.
//   t_sec   - caller-supplied monotonic seconds; the module never reads a clock.
//   rssi    - carried for telemetry/proximity only; not used in the decision.
struct BeaconObservation {
  uint8_t  bssid[6];
  char     ssid[33];
  uint8_t  channel;
  uint32_t t_sec;
  int8_t   rssi;
};

// Escalation state for the current sliding window. NOT latched - it reflects the
// live in-window count of distinct BSSIDs, so when the flood stops the flag
// relaxes back to None on its own (via tick() aging or the next ingest()). A
// flood alarm should clear once the flooding actually ceases.
enum class BeaconFlag : uint8_t {
  None = 0,   // baseline: a handful of real nearby APs is normal
  Elevated,   // an abnormal but not yet damning number of distinct BSSIDs
  Flood,      // a burst of many distinct BSSIDs: an active beacon-flood attack
};

// The verdict for a single ingest(), carrying the evidence (the distinct-BSSID
// count in the current window) so a caller can alert / log without re-deriving it.
struct BeaconVerdict {
  BeaconFlag flag;
  uint16_t   distinct_bssids_per_win;   // distinct AP BSSIDs, in-window
};

// Stateful beacon-flood classifier. Fixed-size internal table, no dynamic
// allocation. A single long-lived instance is fed every observed beacon.
class BeaconFloodDetector {
 public:
  // --- Sliding window. A BSSID "counts" only while its most recent beacon is
  // within kWindowSec seconds of now. A real AP beacons ~10x/second, so a few
  // seconds is plenty to observe every AP actually present; a flood tool spins
  // up fresh BSSIDs continuously, so the same short window captures the storm
  // and relaxes quickly once it stops. Matches ble_spam's kWindowSec. ---
  static const uint32_t kWindowSec = 5;

  // --- Distinct-BSSID escalation thresholds (distinct AP BSSIDs counted within
  // the window).
  // Rationale: a real environment holds a STABLE, bounded set of APs - even a
  // dense apartment block or office floor tops out around a dozen-to-twenty
  // visible SSIDs, and they persist rather than churning. A beacon-flood tool
  // (mdk3/mdk4 beacon mode, Flipper WiFi, SSID flooders) spawns dozens-to-
  // hundreds of brand-new BSSIDs within seconds. The count of DISTINCT BSSIDs in
  // the window is therefore the primary, robust signal.
  //   Elevated: >= 20 distinct in the window - more APs than even a dense-but-
  //             legitimate area typically shows; worth flagging, not yet damning.
  //   Flood:    >= 40 distinct in the window - unmistakable fake-AP spam.
  // These are scaled UP from ble_spam's analogous 6 / 16. ble_spam only counts
  // adverts that pass a spam-vendor payload gate, so 6 distinct is already odd;
  // here EVERY beacon's BSSID counts with no vendor gate, and real WiFi space
  // legitimately holds far more APs than spoof-vendor BLE devices - so the bar
  // is raised to keep a crowded-but-honest RF environment at None. ---
  static const uint16_t kElevatedDistinct = 20;
  static const uint16_t kFloodDistinct    = 40;

  // --- Bounded BSSID table. Distinct-ness is tracked by remembering recently-
  // seen BSSIDs; the table is capped at kMaxBssids (64). When it is full, that is
  // by itself unambiguous flood evidence: 64 distinct BSSIDs in one short window
  // is far past the Flood gate. A newcomer arriving at a full table evicts the
  // LEAST-RECENTLY-SEEN entry (oldest last_seen) so the window stays fresh and
  // the detector keeps tracking the ongoing flood; the count simply saturates at
  // the cap. Never crashes, never corrupts. ---
  static const uint8_t kMaxBssids = 64;

  BeaconFloodDetector() { reset(); }

  // Ingest one observed beacon; return the current verdict. The BSSID is
  // refreshed (or inserted) in the window; the SAME BSSID re-beaconing is
  // deduped - it only refreshes last_seen, it is NOT a new distinct AP (a legit
  // AP beaconing ~10x/sec must never look like a flood). The observation's own
  // t_sec is the window "now"; older out-of-order observations are folded in but
  // never rewind now.
  BeaconVerdict ingest(const BeaconObservation& o);

  // Age the window against a caller-supplied "now" even when no new beacon has
  // arrived, so a stopped flood relaxes: any BSSID whose last_seen has fallen out
  // of the window is dropped (freeing its slot). tick() only moves now forward.
  void tick(uint32_t now_sec);

  // Forget all state.
  void reset();

  // How many BSSIDs currently have live in-window state (distinct APs seen within
  // kWindowSec of the latest known now).
  size_t tracked() const;

  // The verdict flag / distinct count for the current window at the latest known
  // now, without ingesting a new beacon (e.g. after tick()).
  BeaconFlag flag() const;
  uint16_t   distinct() const;

 private:
  struct Entry {
    bool     used;
    uint8_t  bssid[6];
    uint32_t last_seen;   // absolute second this BSSID last beaconed
  };

  Entry    bssids_[kMaxBssids];
  uint32_t now_;          // latest observed second (for const queries)

  // Count distinct BSSIDs whose last_seen is within kWindowSec of now_sec.
  uint16_t count_in_window(uint32_t now_sec) const;

  static BeaconFlag classify(uint16_t distinct);

  Entry* find(const uint8_t bssid[6]);
  // Never returns null: reuses a free/aged slot, else evicts the least-recently-
  // seen BSSID so the ongoing flood keeps being tracked.
  Entry* alloc(const uint8_t bssid[6], uint32_t now_sec);
};

}  // namespace detect
