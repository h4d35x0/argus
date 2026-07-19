// ble_spam.h - pure, host-testable BLE ADVERTISEMENT-SPAM / FLOOD DETECTION.
//
// DEFENSIVE ONLY. This module never transmits, advertises, pairs, or attacks
// anything. It ingests a stream of OBSERVED BLE advertisements (as the on-device
// GAP scanner already receives them) and classifies whether a nearby BLE-spam
// attack is in progress - i.e. a Flipper Zero or a phone "BLE spam" app blasting
// a flood of fake devices (fake AirTags, fake Apple pairing popups, fake Google
// Fast Pair / Samsung buds, Microsoft Swift Pair, ...). It reads the air and
// raises a verdict; it produces no radio output.
//
// On-device grounding (confirmed, not assumed): src/flipper.cpp's on_scan_result
// takes an esp_ble_gap_cb_param_t scan result and forwards res.bda (the 6-byte
// advertiser address), res.ble_adv (the raw advertising+scan-response payload),
// res.adv_data_len + res.scan_rsp_len (the payload length) and res.rssi. Reducing
// one such scan result to a BleAdvObservation (copy the address, point at the raw
// adv bytes, carry the length, stamp the seconds, carry rssi) is a trivial parse
// that lives at the device-only call site, NOT here. This module stays
// hardware-free so the decision logic is unit-testable on the host.
//
// It COMPOSES with the existing pure BLE AD parser (src/ble/adv_parser.h): it
// does NOT re-implement AD parsing. It calls adv_manufacturer_company_id() and
// adv_find_service_data16()/adv_has_service_uuid16() to decide whether an advert
// carries a spam-associated payload.
//
// Self-contained decision surface: it defines its own observation struct and its
// own flag enum, includes only standard headers, and uses integer math only. No
// Arduino.h, no esp_gap_ble, no LVGL, no clock, no dynamic allocation. Time
// arrives as a plain t_sec on every observation (and to tick()), so the whole
// decision is deterministic and reproducible off-device.
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// One observed BLE advertisement, as the on-device GAP scan path would reduce it.
//   addr     - the 6-byte advertiser address (res.bda).
//   adv_data - a VIEW into the caller's raw advertising payload (adv + scan rsp).
//              The caller owns the memory; this module never stores the pointer,
//              it parses the bytes during ingest() and copies out only the address.
//   adv_len  - number of valid bytes at adv_data.
//   t_sec    - caller-supplied monotonic seconds; the module never reads a clock.
//   rssi     - carried for telemetry/proximity only; not used in the decision.
struct BleAdvObservation {
  uint8_t        addr[6];
  const uint8_t* adv_data;
  uint8_t        adv_len;
  uint32_t       t_sec;
  int8_t         rssi;
};

// Escalation state for the current sliding window. NOT latched - it reflects the
// live in-window count of distinct spam-pattern advertisers, so when the flood
// stops the flag relaxes back to None on its own (via tick() aging or the next
// ingest()). A spam alarm should clear once the spamming actually ceases.
enum class SpamFlag : uint8_t {
  None = 0,   // baseline: a handful of ordinary nearby devices is normal
  Elevated,   // an abnormal but not yet damning number of distinct advertisers
  Spam,       // a burst of many distinct spam-pattern advertisers: BLE spam
};

// The verdict for a single ingest(), carrying the evidence (the distinct-address
// count in the current window) so a caller can alert / log without re-deriving it.
struct SpamVerdict {
  SpamFlag flag;
  uint16_t distinct_addrs_per_win;   // distinct spam-pattern advertisers, in-window
};

// Stateful BLE-spam classifier. Fixed-size internal table, no dynamic allocation.
// A single long-lived instance is fed every observed BLE advertisement.
class BleSpamDetector {
 public:
  // --- Sliding window. An advertiser "counts" only while its most recent
  // spam-pattern advert is within kWindowSec seconds of now. BLE spam tools cycle
  // through a fresh randomized address every advertisement, so a short window of a
  // few seconds is enough to see the flood and short enough to relax quickly once
  // it stops. ---
  static const uint32_t kWindowSec = 5;

  // --- Spam-pattern gate. Only advertisements that look like the payloads BLE
  // spam tools abuse are counted, to keep false positives low. An advert matches
  // if its manufacturer company id is on a small watch-list (the vendors whose
  // proximity-pairing beacons are spoofed) OR it carries Google Fast Pair service
  // data. Ordinary WiFi/beacon/name-only adverts are inert. ---
  static const uint16_t kCompanyApple     = 0x004C;  // Continuity / Nearby / Find My
  static const uint16_t kCompanyMicrosoft = 0x0006;  // Swift Pair
  static const uint16_t kCompanySamsung   = 0x0075;  // Galaxy buds / SmartThings
  static const uint16_t kCompanyGoogle    = 0x00E0;  // Google / Fast Pair mfr
  static const uint16_t kFastPairUuid     = 0xFE2C;  // Google Fast Pair service UUID

  // --- Distinct-advertiser escalation thresholds (distinct spam-pattern
  // addresses counted within the window).
  // Rationale: a normal room holds a handful of STABLE devices - a few phones,
  // earbuds, a watch - not dozens of brand-new randomized MACs every few seconds.
  // A BLE-spam tool sprays a continuous stream of fresh addresses, so the count of
  // DISTINCT spam-pattern advertisers is the primary signal.
  //   Elevated: >= 6 distinct in the window - more spam-pattern devices than a
  //             normal room, worth flagging but not yet damning.
  //   Spam:     >= 16 distinct in the window - unmistakable BLE-spam flood.
  // Kept conservative so a real cafe full of Apple/Google gear stays None. ---
  static const uint16_t kElevatedDistinct = 6;
  static const uint16_t kSpamDistinct     = 16;

  // --- Bounded address table. Distinct-ness is tracked by remembering recently
  // seen advertiser addresses; the table is capped. When it is full, that is by
  // itself strong spam evidence: kMaxAddrs (64) distinct spam-pattern advertisers
  // in one short window is far past the Spam gate. A newcomer arriving at a full
  // table evicts the LEAST-RECENTLY-SEEN entry (oldest last_seen) so the window
  // stays fresh and the detector keeps tracking the ongoing flood - no crash, no
  // corruption, the count simply saturates at the cap. ---
  static const uint8_t kMaxAddrs = 64;

  BleSpamDetector() { reset(); }

  // Ingest one observed advertisement; return the current verdict. An advert that
  // does NOT match the spam-pattern gate updates nothing and returns {None, 0}.
  // A matching advert refreshes (or inserts) its address in the window; the same
  // address re-advertising is deduped (it refreshes last_seen, it is NOT a new
  // distinct device). The observation's own t_sec is the window "now"; older
  // out-of-order observations are folded in but never rewind now.
  SpamVerdict ingest(const BleAdvObservation& o);

  // Age the window against a caller-supplied "now" even when no new advert has
  // arrived, so a stopped flood relaxes: any address whose last_seen has fallen
  // out of the window is dropped (freeing its slot). tick() only moves now
  // forward.
  void tick(uint32_t now_sec);

  // Forget all state.
  void reset();

  // How many addresses currently have live in-window state (distinct advertisers
  // seen within kWindowSec of the latest known now).
  size_t tracked() const;

  // The verdict flag / distinct count for the current window at the latest known
  // now, without ingesting a new advert (e.g. after tick()).
  SpamFlag flag() const;
  uint16_t distinct() const;

 private:
  struct Entry {
    bool     used;
    uint8_t  addr[6];
    uint32_t last_seen;   // absolute second this address last spam-advertised
  };

  Entry    addrs_[kMaxAddrs];
  uint32_t now_;          // latest observed second (for const queries)

  // True if this raw advert carries a spam-associated payload (watch-listed
  // manufacturer company id OR Google Fast Pair service data / UUID). Pure
  // parsing, delegated to adv_parser.
  static bool is_spam_pattern(const uint8_t* adv, uint8_t adv_len);

  // Count distinct addresses whose last_seen is within kWindowSec of now_sec.
  uint16_t count_in_window(uint32_t now_sec) const;

  static SpamFlag classify(uint16_t distinct);

  Entry* find(const uint8_t addr[6]);
  // Never returns null: reuses a free/aged slot, else evicts the least-recently-
  // seen address so the ongoing flood keeps being tracked.
  Entry* alloc(const uint8_t addr[6], uint32_t now_sec);
};

}  // namespace detect
