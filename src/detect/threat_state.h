// threat_state.h - pure, host-testable THREAT-STATE AGGREGATOR.
//
// The decision layer that folds every detector's verdict into ONE overall
// wearer-facing threat posture. The pile of independent detectors (evil-twin,
// tail, deauth-flood, BLE-spam, plus future airtag/skimmer/handshake) each read
// their own slice of the air; this module is the glue that turns those separate
// verdicts into a single coherent posture. That posture is what will later drive
// the HADES-red accent flip (see src/theme.h argus_accent()) and the HexHound
// mascot reactions.
//
// DECOUPLED BY DESIGN: this module includes NO detector header. Instead of
// knowing about RogueFlag / TailLevel / DeauthFlag / SpamFlag, it takes a
// GENERIC (domain, severity) signal. Each detector maps its own verdict onto a
// (ThreatDomain, Severity) pair at the future call site, so the aggregator stays
// dependency-free and no cross-directory include tangle is created. Adding a new
// detector means adding a ThreatDomain and a mapping at the call site - never a
// change here.
//
// Self-contained: standard headers only, integer math only, fixed-size state, no
// dynamic allocation. No Arduino.h, no LVGL, no ESP-IDF, no clock. Time arrives
// as a plain t_sec on report() and tick(), so the whole decision is
// deterministic and reproducible off-device.
#pragma once
#include <cstddef>
#include <cstdint>

namespace detect {

// The independent threat "channels" the aggregator folds together. One per kind
// of detector. _Count is the domain count (kept last) and is NOT a real domain;
// it sizes the internal table and bounds iteration. Declaration order is also
// the tie-break order for dominant() (lower enum wins a severity tie), so the
// most classic/most-actionable threats sort first.
enum class ThreatDomain : uint8_t {
  RogueAp = 0,   // evil-twin / rogue-AP (src/detect/evil_twin)
  Tail,          // physical follow / device-tail (src/detect/tail_detect)
  DeauthFlood,   // deauth / disassoc flood (src/detect/deauth_flood)
  BleSpam,       // BLE advertisement spam / flood (src/detect/ble_spam)
  BeaconFlood,   // WiFi beacon-flood / fake-AP spam (src/detect/beacon_flood)
  Airtag,        // future: unwanted-tracker (AirTag / Tile) tail
  Skimmer,       // future: card-skimmer BLE beacon
  _Count,        // sentinel: number of domains (keep last)
};

// A detector's read for its domain, normalized to a common 0..3 scale so the
// aggregator can compare across domains without knowing any detector's private
// verdict enum. None is the clean zero default (no threat on this channel).
enum class Severity : uint8_t {
  None = 0,   // nothing of note on this channel
  Low = 1,    // a faint / early signal
  Medium = 2, // a credible, developing threat
  High = 3,   // an active, unambiguous threat
};

// The single overall posture the whole system presents to the wearer. Maps 1:1
// onto Severity numerically (Calm<->None ... Critical<->High) so the max-domain
// base mapping is a direct cast; the correlation rule (see .cpp) can then push it
// one step hotter. This is what drives the brand/accent state and mascot mood.
enum class ThreatLevel : uint8_t {
  Calm = 0,      // steel-blue at-rest
  Watch = 1,     // something faint worth noticing
  Alert = 2,     // a real threat is developing
  Critical = 3,  // HADES-red: active attack / act now
};

// Stateful aggregator. Fixed one slot per domain, no dynamic allocation, integer
// only, const-correct. A single long-lived instance is owned by the scan
// pipeline; each detector calls report() with its current read every cycle.
class ThreatState {
 public:
  // --- DECAY / HYSTERESIS ---------------------------------------------------
  // Threat should RISE instantly but FALL gracefully, so a one-off blip does not
  // flicker the UI between calm and alarmed. report() applies the reported value
  // immediately (rise is instant). Falling is handled two ways:
  //   * an explicit lower report() (a detector that now reads a weaker threat)
  //     lowers the domain at once - the detector is authoritative for its live
  //     read; and
  //   * DECAY covers the detector going SILENT (no report at all): in tick(), a
  //     domain that has not been re-reported for kDecaySec drops exactly one
  //     Severity step (High->Medium->Low->None), one step per elapsed kDecaySec.
  // So a real ongoing threat - re-reported at its level every cycle - keeps its
  // last_report time fresh and never decays, while a transient that stops being
  // reported relaxes smoothly over kDecaySec-sized steps instead of snapping to
  // Calm. 20s is long enough to ride out a scan gap or a momentary loss of the
  // offending signal, short enough that a genuinely departed threat clears in a
  // reasonable time.
  static constexpr uint32_t kDecaySec = 20;

  // --- CORRELATION ESCALATION -----------------------------------------------
  // Two or more domains simultaneously at Medium-or-worse is materially more
  // dangerous than any single one of them: a rogue AP AND a deauth flood at the
  // same time is an active man-in-the-middle attack in progress, not two
  // coincidences. When at least kCorrelateDomains domains sit at
  // kCorrelateSeverity or above, level() is pushed one step hotter than the
  // plain max-domain mapping (capped at Critical).
  static constexpr uint8_t  kCorrelateDomains  = 2;
  static constexpr Severity kCorrelateSeverity = Severity::Medium;

  static constexpr size_t kDomainCount = static_cast<size_t>(ThreatDomain::_Count);

  ThreatState() { reset(); }

  // A detector's current read for its domain. Sets the domain's severity to the
  // reported value (rise or fall, applied immediately) and stamps t_sec as the
  // domain's last-report time, refreshing its decay clock. A domain d >= _Count
  // is ignored. t_sec is caller-supplied seconds; no clock is read.
  void report(ThreatDomain d, Severity s, uint32_t t_sec);

  // Age the domains against a caller-supplied "now" even when no new report has
  // arrived: any domain not re-reported for kDecaySec decays one Severity step
  // per elapsed kDecaySec, down to None. tick() only moves time forward; a
  // now_sec at or before a domain's last report leaves that domain untouched.
  void tick(uint32_t now_sec);

  // Forget all state: every domain back to None.
  void reset();

  // Overall posture: max-domain mapping, escalated one step by the correlation
  // rule when kCorrelateDomains+ domains are at kCorrelateSeverity+.
  ThreatLevel level() const;

  // The stored severity for one domain (None for d >= _Count).
  Severity domain_severity(ThreatDomain d) const;

  // Bitmask of domains currently non-None (bit (uint8_t)domain set). For the UI
  // to show WHICH threats are live. 0 means all-clear.
  uint8_t active_mask() const;

  // The highest-severity domain, for the headline. Tie -> lowest enum (the
  // declaration order). When every domain is None they all tie at None and this
  // returns RogueAp (the lowest enum); callers distinguish "no threat" via
  // level() == Calm or active_mask() == 0.
  ThreatDomain dominant() const;

 private:
  struct DomainState {
    Severity sev;         // current folded severity for this domain
    uint32_t last_report; // t_sec of the most recent report() (decay anchor)
  };

  DomainState dom_[kDomainCount];
};

}  // namespace detect
