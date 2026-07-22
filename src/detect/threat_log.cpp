// threat_log.cpp - implementation of the pure forensic threat-log edge recorder.
// See threat_log.h for the design rationale and firmware-integration note.
#include "threat_log.h"

#include <cstdio>   // snprintf

namespace detect {

// Human names for each domain, indexed by (uint8_t)ThreatDomain. Order MUST
// track the enum in threat_state.h; the compile-time check below guards it.
static const char* const kDomainNames[] = {
    "RogueAp",      // ThreatDomain::RogueAp
    "Tail",         // ThreatDomain::Tail
    "DeauthFlood",  // ThreatDomain::DeauthFlood
    "BleSpam",      // ThreatDomain::BleSpam
    "BeaconFlood",  // ThreatDomain::BeaconFlood
    "Airtag",       // ThreatDomain::Airtag
    "Skimmer",      // ThreatDomain::Skimmer
    "Surveillance", // ThreatDomain::Surveillance
};
static_assert(sizeof(kDomainNames) / sizeof(kDomainNames[0]) ==
                  static_cast<size_t>(ThreatDomain::_Count),
              "kDomainNames must have one entry per ThreatDomain");

const char* ThreatLog::domain_name(ThreatDomain d) {
  size_t i = static_cast<size_t>(d);
  if (i >= static_cast<size_t>(ThreatDomain::_Count)) return "Unknown";
  return kDomainNames[i];
}

void ThreatLog::reset() {
  next_ = 0;
  size_ = 0;
  total_ = 0;
  for (size_t i = 0; i < kDomainCount; ++i) {
    last_sev_[i] = Severity::None;
    seen_[i] = false;
  }
}

bool ThreatLog::update(ThreatDomain d, Severity s, uint32_t t_sec) {
  size_t i = static_cast<size_t>(d);
  if (i >= kDomainCount) return false;  // ignore unknown domain

  // Baseline: the first read for a domain only records its starting severity so
  // the None default at boot never fabricates a transition.
  if (!seen_[i]) {
    seen_[i] = true;
    last_sev_[i] = s;
    return false;
  }

  // Non-edge: severity unchanged since the last read -> record nothing.
  if (s == last_sev_[i]) return false;

  // Genuine edge (rise, fall, or clear): append to the ring.
  ThreatEvent e;
  e.t_sec = t_sec;
  e.domain = d;
  e.from_sev = last_sev_[i];
  e.to_sev = s;

  buf_[next_] = e;
  next_ = (next_ + 1) % kCapacity;
  if (size_ < kCapacity) ++size_;  // else the oldest slot was just overwritten
  ++total_;

  last_sev_[i] = s;
  return true;
}

const ThreatEvent& ThreatLog::at(size_t i) const {
  static const ThreatEvent kEmpty = {0, ThreatDomain::RogueAp, Severity::None,
                                     Severity::None};
  if (size_ == 0) return kEmpty;
  if (i >= size_) i = size_ - 1;  // clamp to newest retained
  // Oldest retained slot: (next_ - size_) modulo capacity.
  size_t oldest = (next_ + kCapacity - size_) % kCapacity;
  size_t idx = (oldest + i) % kCapacity;
  return buf_[idx];
}

size_t ThreatLog::format(const ThreatEvent& e, char* out, size_t out_sz) {
  if (out == nullptr || out_sz == 0) return 0;
  int n = std::snprintf(out, out_sz, "%lu %s %u->%u",
                        static_cast<unsigned long>(e.t_sec),
                        domain_name(e.domain),
                        static_cast<unsigned>(e.from_sev),
                        static_cast<unsigned>(e.to_sev));
  if (n < 0) {  // encoding error: guarantee a valid empty string
    out[0] = '\0';
    return 0;
  }
  // snprintf returns the length it WOULD have written; the actual written length
  // (excluding NUL) is clamped to out_sz - 1 on truncation. out is always NUL-
  // terminated by snprintf.
  size_t want = static_cast<size_t>(n);
  return (want < out_sz) ? want : (out_sz - 1);
}

}  // namespace detect
