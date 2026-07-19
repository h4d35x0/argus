// deauth_flood.h - pure, host-testable deauth / disassoc FLOOD DETECTION logic.
//
// DEFENSIVE ONLY. This module never transmits, deauthenticates, disassociates,
// or attacks anything. It ingests a stream of OBSERVED 802.11 management-frame
// events (deauthentication / disassociation frames, as the on-device
// promiscuous-mode sniffer already receives them) and classifies whether a
// nearby WiFi deauth attack is in progress - i.e. someone flooding management
// frames to knock clients off an access point. It reads the air and raises a
// verdict; it produces no radio output.
//
// On-device grounding (confirmed, not assumed): src/wifi_beacon_manager.cpp
// already runs the radio in STA + promiscuous with WIFI_PROMIS_FILTER_MASK_MGMT
// and forwards every WIFI_PKT_MGMT frame's raw payload (plus rx_ctrl.rssi and
// channel) to its dispatcher. A deauth frame is management subtype 0xC and a
// disassoc is subtype 0xA (the low nibble of the 802.11 Frame Control field's
// type/subtype byte); the transmitting AP's BSSID is addr3 of the MAC header.
// Reducing such a frame to a MgmtFrameEvent (map subtype -> MgmtType, copy the
// BSSID, stamp the seconds, carry rssi) is a trivial parse that lives at the
// device-only call site, NOT here. This module stays hardware-free so the
// decision logic is unit-testable on the host.
//
// Self-contained by design: it defines its own event struct and its own type
// enum, includes only standard headers, and uses integer math only. No
// Arduino.h, no esp_wifi, no LVGL, no clock, no dynamic allocation. Time arrives
// as a plain t_sec on every event (and to tick()), so the whole decision is
// deterministic and reproducible off-device.
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// The management-frame subtypes this detector cares about. Everything that is
// not a client-disconnect frame is Other and contributes NOTHING to the
// decision (beacons, probes, auth, assoc, action frames, ...).
enum class MgmtType : uint8_t {
  Deauth,     // 802.11 mgmt subtype 0xC - deauthentication
  Disassoc,   // 802.11 mgmt subtype 0xA - disassociation
  Other,      // any other frame; ignored by ingest()
};

// One observed management frame, as the on-device promiscuous path would reduce
// it. bssid is the TRANSMITTER / AP address (addr3). Note: a classic mass-
// disconnect deauth is sent with a BROADCAST destination (ff:ff:ff:ff:ff:ff),
// but its SOURCE is still the AP's BSSID - so keying on bssid here means a
// broadcast flood accrues against that one AP and escalates correctly.
// t_sec is caller-supplied monotonic seconds; the module never reads a clock.
// rssi is carried for telemetry/proximity only and is not used in the decision.
struct MgmtFrameEvent {
  uint8_t  bssid[6];
  MgmtType type;
  uint32_t t_sec;
  int8_t   rssi;
};

// Escalation state for the current sliding window. NOT latched - it reflects the
// live window rate, so when an attack stops the flag relaxes back to None on its
// own (via tick() aging or the next ingest()). This is the opposite of the tail
// detector's upward-latching level, and deliberately so: a flood alarm should
// clear once the flooding actually ceases.
enum class DeauthFlag : uint8_t {
  None = 0,   // baseline: the odd deauth is normal (roaming / idle timeout)
  Elevated,   // an abnormal but not yet damning rate on one AP
  Flood,      // a sustained burst consistent with an active deauth attack
};

// The verdict for a single ingest(), carrying the evidence (the observed rate)
// so a caller can alert / log without re-deriving it.
struct DeauthVerdict {
  DeauthFlag flag;
  uint16_t   rate_per_min;   // deauth+disassoc frames/min for THIS bssid, in-window
};

// Stateful flood classifier. Fixed-size internal tables, no dynamic allocation.
// A single long-lived instance is fed every observed management frame.
class DeauthFloodDetector {
 public:
  // --- Sliding window. Counts are kept in kBuckets one-second buckets; a frame
  // ages out of the window exactly kWindowSec seconds after it arrived. Because
  // kBuckets == kWindowSec and each bucket is indexed by (sec % kBuckets), the
  // bucket kWindowSec seconds old is naturally overwritten by the incoming one,
  // so the window self-cleans with no separate sweep. ---
  static const uint32_t kWindowSec = 10;
  static const uint8_t  kBuckets   = 10;   // one bucket per second of the window

  // --- Per-BSSID escalation thresholds (frames counted within the window).
  // Rationale: a healthy AP emits only the occasional deauth/disassoc (a client
  // roaming away, an idle-session timeout) - far below one per second sustained.
  // A deauth tool (aireplay-ng, mdk4, ...) emits dozens-to-hundreds per second.
  //   Elevated: >= 10 in 10s (~1/sec sustained) - already abnormal for one AP.
  //   Flood:    >= 50 in 10s (~5/sec sustained) - unambiguous attack traffic.
  // Kept conservative so ordinary roaming never trips the alarm. ---
  static const uint16_t kElevatedPerWin = 10;
  static const uint16_t kFloodPerWin    = 50;

  // --- Global (all-BSSID) escalation thresholds. A "scattergun" attacker that
  // sprays a few frames at MANY APs may keep every single BSSID under its own
  // threshold while the aggregate air is plainly hostile. The global window sums
  // every deauth/disassoc regardless of source so that pattern is still caught.
  // Set higher than the per-BSSID gates because it aggregates many sources. ---
  static const uint16_t kGlobalElevatedPerWin = 20;
  static const uint16_t kGlobalFloodPerWin    = 80;

  // --- Bounded per-BSSID table. When full, ingest() evicts the LEAST-active
  // tracked BSSID (smallest in-window count) to make room, so the actual
  // attacker - which by definition has the highest count - is never the one
  // recycled. No crash, no corruption; idle one-off APs are dropped first. The
  // global window is independent of this table, so aggregate detection keeps
  // working even when the per-BSSID table is churning. ---
  static const uint8_t kMaxBssids = 16;

  DeauthFloodDetector() { reset(); }

  // Ingest one observed management frame; return the per-BSSID verdict now.
  // MgmtType::Other frames update nothing and return {None, 0}. The frame's own
  // t_sec is the window "now"; out-of-order (older) frames are folded in but do
  // not rewind the detector's notion of now.
  DeauthVerdict ingest(const MgmtFrameEvent& e);

  // Age the windows against a caller-supplied "now" even when no new frame has
  // arrived, so a stopped attack relaxes: any BSSID with zero in-window frames
  // is dropped from the table (freeing the slot), and the global rate/flag
  // queried afterward reflect the advanced clock. tick() only ever moves now
  // forward.
  void tick(uint32_t now_sec);

  // Forget all state.
  void reset();

  // How many BSSIDs currently have in-table state.
  size_t tracked() const;

  // Aggregate (all-BSSID) evidence for the current window at the latest known
  // now. Lets a scattergun attack surface even when no single BSSID trips.
  uint16_t   global_rate_per_min() const;
  DeauthFlag global_flag() const;

 private:
  struct Bucket {
    uint32_t sec;     // the absolute second this bucket currently represents
    uint16_t count;   // frames observed during that second (saturating)
  };
  struct Window {
    Bucket b[kBuckets];
  };
  struct BssidEntry {
    bool    used;
    uint8_t bssid[6];
    Window  win;
  };

  BssidEntry bssids_[kMaxBssids];
  Window     global_;
  uint32_t   now_;    // latest observed second (for const queries after tick/ingest)

  static void     win_reset(Window& w);
  static void     win_add(Window& w, uint32_t sec);
  static uint16_t win_count(const Window& w, uint32_t now_sec);
  static DeauthFlag classify(uint16_t count, uint16_t elevated, uint16_t flood);

  BssidEntry* find(const uint8_t bssid[6]);
  // Never returns null: reuses a free slot, else evicts the least-active BSSID.
  BssidEntry* alloc(const uint8_t bssid[6], uint32_t now_sec);
};

}  // namespace detect
