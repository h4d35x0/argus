// threat_state.cpp - implementation of the pure threat-state aggregator.
// See threat_state.h for the full design and the two behavior rules (DECAY /
// HYSTERESIS and CORRELATION ESCALATION). Integer-only, no allocation, no clock.
#include "threat_state.h"

namespace detect {

void ThreatState::reset() {
  for (size_t i = 0; i < kDomainCount; ++i) {
    dom_[i].sev = Severity::None;
    dom_[i].last_report = 0;
  }
}

void ThreatState::report(ThreatDomain d, Severity s, uint32_t t_sec) {
  const size_t i = static_cast<size_t>(d);
  if (i >= kDomainCount) return;  // guard: not a real domain (e.g. _Count)
  // Rise or fall, applied immediately; refresh the decay clock for this domain.
  dom_[i].sev = s;
  dom_[i].last_report = t_sec;
}

void ThreatState::tick(uint32_t now_sec) {
  for (size_t i = 0; i < kDomainCount; ++i) {
    // Nothing to decay, and never rewind time on an out-of-order / stale tick.
    if (dom_[i].sev == Severity::None) continue;
    if (now_sec <= dom_[i].last_report) continue;

    // One Severity step drops per whole kDecaySec elapsed since the last report.
    // Advancing last_report by the consumed whole periods (rather than snapping
    // it to now_sec) keeps the remainder toward the next step, so decay is a
    // smooth staircase and never loses partial progress.
    uint32_t elapsed = now_sec - dom_[i].last_report;
    uint32_t steps = elapsed / kDecaySec;
    if (steps == 0) continue;

    uint8_t sev = static_cast<uint8_t>(dom_[i].sev);
    if (steps >= sev) {
      sev = 0;  // decayed all the way to None
    } else {
      sev = static_cast<uint8_t>(sev - steps);
    }
    dom_[i].sev = static_cast<Severity>(sev);
    dom_[i].last_report += steps * kDecaySec;
  }
}

Severity ThreatState::domain_severity(ThreatDomain d) const {
  const size_t i = static_cast<size_t>(d);
  if (i >= kDomainCount) return Severity::None;
  return dom_[i].sev;
}

uint8_t ThreatState::active_mask() const {
  uint8_t mask = 0;
  for (size_t i = 0; i < kDomainCount; ++i) {
    if (dom_[i].sev != Severity::None) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

ThreatDomain ThreatState::dominant() const {
  // Iterate in ascending enum order and update only on a STRICT increase, so the
  // first (lowest-enum) domain holding the maximum severity wins the tie.
  size_t best = 0;
  uint8_t best_sev = static_cast<uint8_t>(dom_[0].sev);
  for (size_t i = 1; i < kDomainCount; ++i) {
    uint8_t sev = static_cast<uint8_t>(dom_[i].sev);
    if (sev > best_sev) {
      best_sev = sev;
      best = i;
    }
  }
  return static_cast<ThreatDomain>(best);
}

ThreatLevel ThreatState::level() const {
  // Base: the max domain severity mapped 1:1 onto ThreatLevel (the enums share
  // the same 0..3 numbering, so the max severity value IS the base level).
  uint8_t max_sev = 0;
  uint8_t correlated = 0;  // domains at kCorrelateSeverity or above
  const uint8_t corr_thresh = static_cast<uint8_t>(kCorrelateSeverity);
  for (size_t i = 0; i < kDomainCount; ++i) {
    uint8_t sev = static_cast<uint8_t>(dom_[i].sev);
    if (sev > max_sev) max_sev = sev;
    if (sev >= corr_thresh) ++correlated;
  }

  uint8_t lvl = max_sev;  // 1:1 base mapping

  // CORRELATION ESCALATION: multiple simultaneous credible threats are worse
  // than any single one - push one step hotter, capped at Critical.
  if (correlated >= kCorrelateDomains) {
    ++lvl;
    const uint8_t cap = static_cast<uint8_t>(ThreatLevel::Critical);
    if (lvl > cap) lvl = cap;
  }
  return static_cast<ThreatLevel>(lvl);
}

}  // namespace detect
