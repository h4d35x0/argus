// test_threat_log.cpp - host unit tests for the forensic threat log
// (src/detect/threat_log). Verifies the edge-only change detection, the baseline
// discipline (first read per domain never logs), independence across domains,
// the fixed ring with oldest-eviction on wrap, bounds-safe formatting, the
// domain-name table, and reset. Pure: time is a plain input, no hardware.
#include "wl_test.h"
#include "threat_log.h"

#include <cstdint>
#include <cstring>

using namespace detect;

// ---- Baseline: the first update() for a domain records no event ------------
WL_TEST(log_first_update_is_baseline_no_event) {
  ThreatLog log;
  // Even a "None" first read just establishes the baseline.
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::None, 100) == false);
  WL_CHECK_EQ(log.count(), (size_t)0);
  WL_CHECK_EQ(log.total_recorded(), (size_t)0);
  // A non-None first read is likewise only a baseline, never a false rise.
  ThreatLog log2;
  WL_CHECK(log2.update(ThreatDomain::Tail, Severity::High, 100) == false);
  WL_CHECK_EQ(log2.count(), (size_t)0);
}

// ---- A None->High change logs exactly one event with correct from/to -------
WL_TEST(log_rise_records_one_event) {
  ThreatLog log;
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::None, 100) == false);
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::High, 150) == true);
  WL_CHECK_EQ(log.count(), (size_t)1);
  WL_CHECK_EQ(log.total_recorded(), (size_t)1);
  const ThreatEvent& e = log.at(0);
  WL_CHECK(e.domain == ThreatDomain::RogueAp);
  WL_CHECK(e.from_sev == Severity::None);
  WL_CHECK(e.to_sev == Severity::High);
  WL_CHECK_EQ(e.t_sec, (uint32_t)150);
}

// ---- Holding High across many polls logs nothing (no duplicate spam) -------
WL_TEST(log_hold_high_records_no_dupes) {
  ThreatLog log;
  log.update(ThreatDomain::DeauthFlood, Severity::None, 0);   // baseline
  WL_CHECK(log.update(ThreatDomain::DeauthFlood, Severity::High, 10) == true);
  for (uint32_t t = 11; t < 100; ++t) {
    WL_CHECK(log.update(ThreatDomain::DeauthFlood, Severity::High, t) == false);
  }
  WL_CHECK_EQ(log.count(), (size_t)1);
  WL_CHECK_EQ(log.total_recorded(), (size_t)1);
}

// ---- A High->None clear logs its own event ---------------------------------
WL_TEST(log_clear_records_event) {
  ThreatLog log;
  log.update(ThreatDomain::RogueAp, Severity::None, 0);   // baseline
  log.update(ThreatDomain::RogueAp, Severity::High, 10);  // rise (event 1)
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::None, 60) == true);  // clear
  WL_CHECK_EQ(log.count(), (size_t)2);
  const ThreatEvent& clr = log.at(1);  // newest retained
  WL_CHECK(clr.from_sev == Severity::High);
  WL_CHECK(clr.to_sev == Severity::None);
  WL_CHECK_EQ(clr.t_sec, (uint32_t)60);
}

// ---- Two domains keep independent baselines/last-severity ------------------
WL_TEST(log_two_domains_independent) {
  ThreatLog log;
  log.update(ThreatDomain::RogueAp, Severity::None, 0);  // baseline A
  log.update(ThreatDomain::Tail, Severity::None, 0);     // baseline B
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::Medium, 5) == true);
  WL_CHECK(log.update(ThreatDomain::Tail, Severity::Low, 6) == true);
  // Re-reporting each at its held level logs nothing for either.
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::Medium, 7) == false);
  WL_CHECK(log.update(ThreatDomain::Tail, Severity::Low, 8) == false);
  WL_CHECK_EQ(log.count(), (size_t)2);
  WL_CHECK(log.at(0).domain == ThreatDomain::RogueAp);
  WL_CHECK(log.at(1).domain == ThreatDomain::Tail);
}

// ---- Ring wraps past kCapacity: count caps, total climbs, oldest evicted ---
WL_TEST(log_ring_wraps_and_evicts_oldest) {
  ThreatLog log;
  const size_t cap = ThreatLog::kCapacity;
  // Baseline one domain, then toggle it None<->High so each update is an edge.
  // Encode a distinct t_sec per event so we can identify the retained window.
  log.update(ThreatDomain::RogueAp, Severity::None, 0);  // baseline (no event)
  size_t events = 0;
  const size_t kExtra = 5;  // push kExtra past capacity to force eviction
  for (size_t k = 0; k < cap + kExtra; ++k) {
    Severity s = (k % 2 == 0) ? Severity::High : Severity::None;
    // t_sec = k + 1 (1-based), unique and monotonically increasing per event.
    bool logged = log.update(ThreatDomain::RogueAp, s, (uint32_t)(k + 1));
    WL_CHECK(logged == true);
    ++events;
  }
  WL_CHECK_EQ(events, cap + kExtra);
  WL_CHECK_EQ(log.count(), cap);                    // retained window capped
  WL_CHECK_EQ(log.total_recorded(), cap + kExtra);  // lifetime keeps climbing
  // Oldest retained is the (kExtra)-th event overall: its t_sec == kExtra + 1.
  WL_CHECK_EQ(log.at(0).t_sec, (uint32_t)(kExtra + 1));
  // Newest retained is the last event: t_sec == cap + kExtra.
  WL_CHECK_EQ(log.at(cap - 1).t_sec, (uint32_t)(cap + kExtra));
  // Retained window is contiguous and ascending.
  for (size_t i = 0; i < log.count(); ++i) {
    WL_CHECK_EQ(log.at(i).t_sec, (uint32_t)(kExtra + 1 + i));
  }
  // Out-of-range index is clamped to the newest retained (bounds-safe).
  WL_CHECK_EQ(log.at(9999).t_sec, (uint32_t)(cap + kExtra));
}

// ---- at() on an empty log is bounds-safe -----------------------------------
WL_TEST(log_at_empty_is_safe) {
  ThreatLog log;
  WL_CHECK_EQ(log.count(), (size_t)0);
  const ThreatEvent& e = log.at(0);          // must not read out of bounds
  WL_CHECK(e.to_sev == Severity::None);
  const ThreatEvent& e2 = log.at(12345);     // clamped, still safe
  WL_CHECK(e2.from_sev == Severity::None);
}

// ---- format() produces the documented string -------------------------------
WL_TEST(log_format_exact_string) {
  ThreatEvent e;
  e.t_sec = 1720000000u;
  e.domain = ThreatDomain::RogueAp;
  e.from_sev = Severity::None;   // 0
  e.to_sev = Severity::High;     // 3
  char out[64];
  size_t n = ThreatLog::format(e, out, sizeof(out));
  WL_CHECK(std::strcmp(out, "1720000000 RogueAp 0->3") == 0);
  WL_CHECK_EQ(n, std::strlen("1720000000 RogueAp 0->3"));
}

// ---- format() is bounds-safe into a short buffer ---------------------------
WL_TEST(log_format_truncates_safely) {
  ThreatEvent e;
  e.t_sec = 1720000000u;
  e.domain = ThreatDomain::BeaconFlood;
  e.from_sev = Severity::Low;
  e.to_sev = Severity::Medium;
  char small[8];
  std::memset(small, 'X', sizeof(small));
  size_t n = ThreatLog::format(e, small, sizeof(small));
  WL_CHECK(n <= sizeof(small) - 1);            // never overruns
  WL_CHECK_EQ(small[sizeof(small) - 1], '\0'); // NUL-terminated
  WL_CHECK_EQ(std::strlen(small), n);          // reported length matches
  // out_sz == 0 must write nothing and report 0.
  char none[1] = {'Z'};
  WL_CHECK_EQ(ThreatLog::format(e, none, 0), (size_t)0);
  WL_CHECK_EQ(none[0], 'Z');                    // untouched
}

// ---- domain_name covers every ThreatDomain ---------------------------------
WL_TEST(log_domain_name_all) {
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::RogueAp), "RogueAp") == 0);
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::Tail), "Tail") == 0);
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::DeauthFlood), "DeauthFlood") == 0);
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::BleSpam), "BleSpam") == 0);
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::BeaconFlood), "BeaconFlood") == 0);
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::Airtag), "Airtag") == 0);
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::Skimmer), "Skimmer") == 0);
  // Every real domain yields a non-empty, non-"Unknown" name.
  for (size_t i = 0; i < (size_t)ThreatDomain::_Count; ++i) {
    const char* nm = ThreatLog::domain_name((ThreatDomain)i);
    WL_CHECK(nm[0] != '\0');
    WL_CHECK(std::strcmp(nm, "Unknown") != 0);
  }
  // The sentinel / out-of-range maps to "Unknown".
  WL_CHECK(std::strcmp(ThreatLog::domain_name(ThreatDomain::_Count), "Unknown") == 0);
}

// ---- update() ignores an out-of-range domain -------------------------------
WL_TEST(log_ignores_unknown_domain) {
  ThreatLog log;
  WL_CHECK(log.update(ThreatDomain::_Count, Severity::High, 10) == false);
  WL_CHECK_EQ(log.count(), (size_t)0);
  WL_CHECK_EQ(log.total_recorded(), (size_t)0);
}

// ---- reset() clears the ring, baselines, and lifetime counter --------------
WL_TEST(log_reset_clears) {
  ThreatLog log;
  log.update(ThreatDomain::RogueAp, Severity::None, 0);
  log.update(ThreatDomain::RogueAp, Severity::High, 10);
  WL_CHECK_EQ(log.count(), (size_t)1);
  WL_CHECK_EQ(log.total_recorded(), (size_t)1);
  log.reset();
  WL_CHECK_EQ(log.count(), (size_t)0);
  WL_CHECK_EQ(log.total_recorded(), (size_t)0);
  // After reset the next read per domain is again a baseline (no event).
  WL_CHECK(log.update(ThreatDomain::RogueAp, Severity::High, 20) == false);
  WL_CHECK_EQ(log.count(), (size_t)0);
}
