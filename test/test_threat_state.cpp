// test_threat_state.cpp - host unit tests for the pure threat-state aggregator
// (src/detect/threat_state). This is the DECISION layer, not a detector: it
// folds generic (domain, severity) reads from every detector into ONE overall
// ThreatLevel. These exercise the two behavior rules end to end - instant RISE
// with graceful DECAY (a re-reported ongoing threat stays hot; a silenced one
// steps down over kDecaySec), and CORRELATION ESCALATION (two credible domains
// push one step hotter than either alone) - plus dominant()/active_mask()/reset.
// Time is a plain input on every report() and tick(); no clock, no hardware, and
// deliberately NO detector header is included (the aggregator is decoupled).
#include "wl_test.h"
#include "threat_state.h"

#include <cstdint>

using namespace detect;

// ---- No signals: baseline posture is Calm and nothing is active. ------------
WL_TEST(threat_no_signals_is_calm) {
  ThreatState ts;
  WL_CHECK(ts.level() == ThreatLevel::Calm);
  WL_CHECK_EQ(ts.active_mask(), (uint8_t)0);
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::None);
  // All-None tie-break: dominant() returns the lowest enum (RogueAp).
  WL_CHECK(ts.dominant() == ThreatDomain::RogueAp);
}

// ---- A single High domain maps straight to Critical (instant rise). ---------
WL_TEST(threat_single_high_is_critical) {
  ThreatState ts;
  ts.report(ThreatDomain::DeauthFlood, Severity::High, 100);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  WL_CHECK(ts.dominant() == ThreatDomain::DeauthFlood);
  WL_CHECK_EQ(ts.active_mask(),
              (uint8_t)(1u << (uint8_t)ThreatDomain::DeauthFlood));
}

// ---- The 1:1 base mapping for each single-domain severity. -------------------
WL_TEST(threat_single_domain_maps_one_to_one) {
  ThreatState ts;
  ts.report(ThreatDomain::Tail, Severity::Low, 10);
  WL_CHECK(ts.level() == ThreatLevel::Watch);
  ts.report(ThreatDomain::Tail, Severity::Medium, 10);
  WL_CHECK(ts.level() == ThreatLevel::Alert);
  ts.report(ThreatDomain::Tail, Severity::High, 10);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  // Explicit lower report falls immediately (detector is authoritative).
  ts.report(ThreatDomain::Tail, Severity::Low, 10);
  WL_CHECK(ts.level() == ThreatLevel::Watch);
}

// ---- A single Medium is Alert - one domain never self-escalates. ------------
WL_TEST(threat_single_medium_no_false_escalation) {
  ThreatState ts;
  ts.report(ThreatDomain::RogueAp, Severity::Medium, 50);
  WL_CHECK(ts.level() == ThreatLevel::Alert);  // NOT Critical
}

// ---- Two Medium domains correlate -> one step hotter than Alert (Critical). --
WL_TEST(threat_two_medium_correlate_to_critical) {
  ThreatState ts;
  ts.report(ThreatDomain::RogueAp, Severity::Medium, 200);
  // Still just one credible domain -> Alert.
  WL_CHECK(ts.level() == ThreatLevel::Alert);
  ts.report(ThreatDomain::DeauthFlood, Severity::Medium, 200);
  // Now two domains at Medium+: escalate Alert -> Critical.
  WL_CHECK(ts.level() == ThreatLevel::Critical);
}

// ---- Correlation with Low does NOT count: Low is below kCorrelateSeverity. ---
WL_TEST(threat_low_does_not_correlate) {
  ThreatState ts;
  ts.report(ThreatDomain::RogueAp, Severity::Medium, 200);
  ts.report(ThreatDomain::Tail, Severity::Low, 200);
  // One Medium (correlating) + one Low (not correlating) -> no escalation.
  WL_CHECK(ts.level() == ThreatLevel::Alert);
}

// ---- Correlation escalation caps at Critical (two Highs stay Critical). ------
WL_TEST(threat_correlation_caps_at_critical) {
  ThreatState ts;
  ts.report(ThreatDomain::RogueAp, Severity::High, 200);
  ts.report(ThreatDomain::BleSpam, Severity::High, 200);
  WL_CHECK(ts.level() == ThreatLevel::Critical);  // capped, not overflowed
}

// ---- A quieted domain decays step-by-step to Calm past kDecaySec. -----------
WL_TEST(threat_silent_domain_decays_to_calm) {
  ThreatState ts;
  ts.report(ThreatDomain::BleSpam, Severity::High, 1000);
  WL_CHECK(ts.level() == ThreatLevel::Critical);

  // Inside the first decay window: still hot.
  ts.tick(1000 + ThreatState::kDecaySec - 1);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  WL_CHECK(ts.domain_severity(ThreatDomain::BleSpam) == Severity::High);

  // One kDecaySec elapsed with no re-report: High -> Medium (Alert).
  ts.tick(1000 + ThreatState::kDecaySec);
  WL_CHECK(ts.domain_severity(ThreatDomain::BleSpam) == Severity::Medium);
  WL_CHECK(ts.level() == ThreatLevel::Alert);

  // Two more windows: Medium -> Low -> None.
  ts.tick(1000 + 2 * ThreatState::kDecaySec);
  WL_CHECK(ts.domain_severity(ThreatDomain::BleSpam) == Severity::Low);
  WL_CHECK(ts.level() == ThreatLevel::Watch);

  ts.tick(1000 + 3 * ThreatState::kDecaySec);
  WL_CHECK(ts.domain_severity(ThreatDomain::BleSpam) == Severity::None);
  WL_CHECK(ts.level() == ThreatLevel::Calm);
  WL_CHECK_EQ(ts.active_mask(), (uint8_t)0);
}

// ---- A big time jump collapses multiple decay steps at once (down to None). --
WL_TEST(threat_large_gap_decays_all_the_way) {
  ThreatState ts;
  ts.report(ThreatDomain::Tail, Severity::High, 500);
  // Jump well past 3 decay windows in a single tick. Tail is a slow-cadence
  // domain, so use ITS period rather than the flood default.
  const uint32_t period = ThreatState::decay_sec_for(ThreatDomain::Tail);
  ts.tick(500 + 10 * period);
  WL_CHECK(ts.domain_severity(ThreatDomain::Tail) == Severity::None);
  WL_CHECK(ts.level() == ThreatLevel::Calm);
}

// ---- A re-reported ongoing High stays Critical across many ticks (no decay). -
WL_TEST(threat_ongoing_high_never_decays) {
  ThreatState ts;
  uint32_t t = 2000;
  ts.report(ThreatDomain::RogueAp, Severity::High, t);
  // Simulate many scan cycles: re-report each cycle, tick between them. The
  // fresh report keeps the decay clock reset, so it must stay Critical forever.
  for (int cycle = 0; cycle < 50; ++cycle) {
    t += 5;                        // 5s scan cadence, well under kDecaySec
    ts.tick(t);
    ts.report(ThreatDomain::RogueAp, Severity::High, t);
    WL_CHECK(ts.level() == ThreatLevel::Critical);
  }
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::High);
}

// ---- dominant() picks the highest severity, tie -> lowest enum. -------------
WL_TEST(threat_dominant_highest_then_tiebreak) {
  ThreatState ts;
  ts.report(ThreatDomain::DeauthFlood, Severity::Medium, 300);
  ts.report(ThreatDomain::BleSpam, Severity::High, 300);
  // BleSpam is strictly higher -> it is the headline despite the higher enum.
  WL_CHECK(ts.dominant() == ThreatDomain::BleSpam);

  // Now raise DeauthFlood to also-High: tie -> the lower enum (DeauthFlood).
  ts.report(ThreatDomain::DeauthFlood, Severity::High, 300);
  WL_CHECK(ts.dominant() == ThreatDomain::DeauthFlood);

  // RogueAp is lowest enum of all; make it tie at High too -> it wins.
  ts.report(ThreatDomain::RogueAp, Severity::High, 300);
  WL_CHECK(ts.dominant() == ThreatDomain::RogueAp);
}

// ---- active_mask reflects exactly the non-None domains. ---------------------
WL_TEST(threat_active_mask_reflects_nonnone) {
  ThreatState ts;
  ts.report(ThreatDomain::RogueAp, Severity::Low, 400);
  ts.report(ThreatDomain::BleSpam, Severity::Medium, 400);
  uint8_t expect = (uint8_t)((1u << (uint8_t)ThreatDomain::RogueAp) |
                             (1u << (uint8_t)ThreatDomain::BleSpam));
  WL_CHECK_EQ(ts.active_mask(), expect);

  // Reporting None clears just that bit.
  ts.report(ThreatDomain::RogueAp, Severity::None, 400);
  WL_CHECK_EQ(ts.active_mask(),
              (uint8_t)(1u << (uint8_t)ThreatDomain::BleSpam));
}

// ---- Reporting to the _Count sentinel is ignored, not a buffer overrun. -----
WL_TEST(threat_count_sentinel_ignored) {
  ThreatState ts;
  ts.report(ThreatDomain::_Count, Severity::High, 10);
  WL_CHECK(ts.level() == ThreatLevel::Calm);
  WL_CHECK_EQ(ts.active_mask(), (uint8_t)0);
  WL_CHECK(ts.domain_severity(ThreatDomain::_Count) == Severity::None);
}

// ---- reset() clears everything back to baseline. ----------------------------
WL_TEST(threat_reset_clears) {
  ThreatState ts;
  ts.report(ThreatDomain::RogueAp, Severity::High, 700);
  ts.report(ThreatDomain::DeauthFlood, Severity::High, 700);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  ts.reset();
  WL_CHECK(ts.level() == ThreatLevel::Calm);
  WL_CHECK_EQ(ts.active_mask(), (uint8_t)0);
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::None);
  WL_CHECK(ts.domain_severity(ThreatDomain::DeauthFlood) == Severity::None);
}

// ---- report_raise: a stronger read still rises immediately. -----------------
WL_TEST(threat_report_raise_still_rises) {
  ThreatState ts;
  ts.report_raise(ThreatDomain::Airtag, Severity::Low, 500);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::Low);
  ts.report_raise(ThreatDomain::Airtag, Severity::High, 500);  // a stronger entity
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
}

// ---- report_raise: a weaker interleaved read must NOT lower the level. -------
// Reproduces the 2026-07-29 field bug: a confirmed tracker (High) and benign
// devices (None) interleave on the per-advert feed. Last-wins report() would
// strobe the domain None<->High every benign advert; report_raise() holds High.
WL_TEST(threat_report_raise_holds_against_interleaved_weaker) {
  ThreatState ts;
  uint32_t t = 1000;
  for (int i = 0; i < 20; ++i) {
    ts.report_raise(ThreatDomain::Airtag, Severity::High, t);   // the real tracker
    WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
    ts.report_raise(ThreatDomain::Airtag, Severity::None, t);   // a benign device
    WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);  // not stomped
    t += 2;
    ts.tick(t);   // ongoing, re-reported inside kDecaySec: never decays
  }
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
}

// ---- report_raise: a weaker read does not refresh the decay clock, so once the
// real threat departs the domain still relaxes to None over its decay period. ---
WL_TEST(threat_report_raise_decays_after_threat_departs) {
  ThreatState ts;
  ts.report_raise(ThreatDomain::Airtag, Severity::High, 1000);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);

  // Tracker gone; only benign None reads keep arriving. They must be ignored
  // (not refresh the anchor), so decay steps High->Medium->Low->None on schedule.
  const uint32_t period = ThreatState::decay_sec_for(ThreatDomain::Airtag);
  uint32_t t = 1000;
  for (int i = 0; i < 3; ++i) {
    t += period;
    ts.report_raise(ThreatDomain::Airtag, Severity::None, t);  // ignored, no refresh
    ts.tick(t);
  }
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::None);
  WL_CHECK(ts.level() == ThreatLevel::Calm);
}

// ---- PER-DOMAIN DECAY PERIOD ------------------------------------------------
// Regression cover for the 2026-07-30 field finding: a LONE tracker adverts on
// its own schedule (measured avg 37.7s, ~60s mode), so under the flat 20s flood
// period it decayed High->None across a single advert gap and the HADES accent
// went cold mid-tail. Airtag/Tail now decay on kSlowDecaySec; the flood domains
// must NOT be slowed down with them.

// A slow-cadence domain HOLDS High across a realistic 60s advert gap.
WL_TEST(threat_slow_domain_holds_across_advert_gap) {
  ThreatState ts;
  ts.report_raise(ThreatDomain::Airtag, Severity::High, 1000);

  // 60s of silence - the observed modal gap between two adverts of one tracker.
  ts.tick(1060);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
  // Still hot enough for the accent (flips at Alert), which is the whole point.
  WL_CHECK(ts.level() == ThreatLevel::Critical);

  // The next advert arrives and re-raises; the domain never dipped.
  ts.report_raise(ThreatDomain::Airtag, Severity::High, 1060);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
}

// ...but it is not immortal: past its own period it still steps down.
WL_TEST(threat_slow_domain_still_decays_on_its_own_period) {
  ThreatState ts;
  const uint32_t period = ThreatState::decay_sec_for(ThreatDomain::Airtag);
  WL_CHECK(period > ThreatState::kDecaySec);   // genuinely slower than flood

  ts.report_raise(ThreatDomain::Airtag, Severity::High, 1000);
  ts.tick(1000 + period - 1);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);

  ts.tick(1000 + period);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::Medium);
  ts.tick(1000 + 3 * period);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::None);
}

// A flood domain keeps the fast 20s staircase - the split is real, not a
// global slowdown. Same elapsed time that leaves Airtag at High clears BeaconFlood.
WL_TEST(threat_flood_domain_keeps_fast_decay) {
  ThreatState ts;
  WL_CHECK(ThreatState::decay_sec_for(ThreatDomain::BeaconFlood) ==
           ThreatState::kDecaySec);

  ts.report(ThreatDomain::BeaconFlood, Severity::High, 1000);
  ts.report_raise(ThreatDomain::Airtag, Severity::High, 1000);

  ts.tick(1060);   // 60s: 3 flood steps, 0 slow steps
  WL_CHECK(ts.domain_severity(ThreatDomain::BeaconFlood) == Severity::None);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
}
