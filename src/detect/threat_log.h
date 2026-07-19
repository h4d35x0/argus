// threat_log.h - pure, host-testable FORENSIC THREAT LOG (edge recorder).
//
// Phase 4 blue-team capability: a timestamped history of what the watch saw and
// when, so the wearer can review the incident after the fact ("RogueAp escalated
// to High at t=..., cleared at t=..."). The aggregator (src/detect/threat_state)
// is polled continuously; writing a record every tick would flood the log with
// thousands of identical lines. So this module records EDGES, not ticks: a
// discrete ThreatEvent is appended only when a domain's Severity CHANGES (rises,
// falls, or clears). A threat that holds steady at High for ten minutes is ONE
// event (the rise), not ten minutes of duplicates.
//
// PURE CORE ONLY. This is the change-detection + fixed in-memory ring + record
// formatting. It composes with the existing vocabulary (ThreatDomain / Severity
// from threat_state.h) and adds no new severity concepts. Standard headers only,
// integer math, fixed-size state, no dynamic allocation. No Arduino.h, no LVGL,
// no ESP-IDF, no SD, no clock. Time arrives as a plain t_sec on update(), so the
// whole decision is deterministic and reproducible off-device.
//
// --- FIRMWARE INTEGRATION (documented here, implemented in the thin firmware
//     wrapper that is OUT OF SCOPE for this pure core) ------------------------
// On each scan poll, after threat_state.report()/tick() and reading level(), the
// firmware feeds each domain's current severity to a single long-lived ThreatLog
// and appends to SD only on a genuine edge:
//
//   for (size_t i = 0; i < ThreatState::kDomainCount; ++i) {
//     ThreatDomain d = (ThreatDomain)i;
//     if (log.update(d, ts.domain_severity(d), now_sec)) {
//       // an edge was recorded -> persist just this one line
//       char line[64];
//       ThreatLog::format(log.at(log.count() - 1), line, sizeof(line));
//       // Append line + '\n' to /Settings/threat_log.txt, but ONLY when the
//       // firmware owns the card. Guard EXACTLY like gps_save_power() in
//       // src/gps_screen.cpp:
//       //   if (!instance.isCardReady() || usb_sd_is_running()) return;
//       // (skip the write when there is no card or the USB host has the SD
//       //  mounted; the in-memory ring still holds the event for on-watch review.)
//     }
//   }
//
// The SD append is a few lines of hardware glue; it is deliberately NOT in this
// file so the interesting logic stays 100% host-unit-tested. total_recorded()
// backs an "N events" counter in the UI even after the ring has wrapped.
#pragma once
#include <cstddef>
#include <cstdint>

#include "threat_state.h"  // ThreatDomain, Severity (do NOT redefine)

namespace detect {

// One recorded transition: at t_sec, `domain` moved from from_sev to to_sev.
// A rise has to_sev > from_sev, a fall/clear has to_sev < from_sev. Equal
// severities are never recorded (that is a non-edge and update() drops it).
struct ThreatEvent {
  uint32_t     t_sec;     // caller-supplied time of the transition
  ThreatDomain domain;    // which channel changed
  Severity     from_sev;  // severity before the change
  Severity     to_sev;    // severity after the change
};

// Fixed-size, allocation-free edge recorder. One long-lived instance is owned by
// the scan pipeline. Feed each domain's current severity every poll; the log
// remembers the last severity per domain and appends a ThreatEvent to its ring
// ONLY on a genuine change.
class ThreatLog {
 public:
  // Retained-event ring capacity. When full, the OLDEST retained event is
  // evicted (overwritten) so the newest kCapacity edges are always available for
  // on-watch review; total_recorded() keeps climbing past this. 48 covers a long
  // incident's worth of distinct transitions while staying tiny in RAM.
  static constexpr size_t kCapacity = 48;

  ThreatLog() { reset(); }

  // Feed the current severity for one domain this poll. On the FIRST update()
  // for a domain, its last-severity baseline is established WITHOUT logging (so
  // the None-default at boot never produces a spurious "None->None" or a false
  // rise). On any subsequent call whose severity DIFFERS from the remembered
  // last, a ThreatEvent is appended and true is returned; an unchanged severity
  // records nothing and returns false. A domain d >= _Count is ignored (returns
  // false). t_sec is caller-supplied; no clock is read.
  bool update(ThreatDomain d, Severity s, uint32_t t_sec);

  // Forget everything: empty the ring, clear per-domain baselines, zero the
  // lifetime counter. The next update() per domain re-establishes a baseline.
  void reset();

  // Number of events currently retained in the ring (0 .. kCapacity).
  size_t count() const { return size_; }

  // The i-th retained event, i == 0 being the OLDEST still retained. Bounds-
  // guarded: an out-of-range i is clamped to the newest retained event, and when
  // the ring is empty a zeroed static event is returned, so this never reads out
  // of bounds.
  const ThreatEvent& at(size_t i) const;

  // Lifetime count of recorded events, INCLUDING those already evicted from the
  // ring. Backs an "N events seen" UI counter that must not reset on wrap.
  size_t total_recorded() const { return total_; }

  // Format one event as a compact, parseable ASCII log line into out, e.g.
  //   "1720000000 RogueAp 0->3"
  // (t_sec, domain name, from->to severity as the raw 0..3 scale). Always NUL-
  // terminates when out_sz > 0 and truncates safely into a short buffer. Returns
  // the number of characters written excluding the NUL (the truncated length
  // when the line did not fit).
  static size_t format(const ThreatEvent& e, char* out, size_t out_sz);

  // Stable, ASCII human name for a domain (e.g. "RogueAp"). A d >= _Count
  // returns "Unknown". Used by format() and by UI rendering.
  static const char* domain_name(ThreatDomain d);

 private:
  static constexpr size_t kDomainCount =
      static_cast<size_t>(ThreatDomain::_Count);

  ThreatEvent buf_[kCapacity];          // ring storage
  size_t      next_;                    // next write slot
  size_t      size_;                    // retained count (<= kCapacity)
  size_t      total_;                   // lifetime recorded count

  Severity    last_sev_[kDomainCount];  // remembered severity per domain
  bool        seen_[kDomainCount];      // has this domain been baselined yet
};

}  // namespace detect
