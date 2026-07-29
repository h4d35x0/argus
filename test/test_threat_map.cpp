// test_threat_map.cpp - host unit tests for the detector->aggregator mapping
// layer (src/detect/threat_map). Verifies each detector flag maps to the
// intended unified Severity, and that feed() reports it to the aggregator under
// the correct domain so a single call drives the overall ThreatLevel. Pure:
// time is a plain input, no hardware.
#include "wl_test.h"
#include "threat_map.h"

#include <cstdint>

using namespace detect;

// ---- Per-flag severity mappings --------------------------------------------
WL_TEST(map_rogue_severity) {
  WL_CHECK(severity_of(RogueFlag::None) == Severity::None);
  WL_CHECK(severity_of(RogueFlag::ChannelChangeForBssid) == Severity::Low);
  WL_CHECK(severity_of(RogueFlag::NewBssidForKnownSsid) == Severity::Medium);
  WL_CHECK(severity_of(RogueFlag::SecurityDowngrade) == Severity::Medium);
  WL_CHECK(severity_of(RogueFlag::OpenTwinOfSecuredSsid) == Severity::High);
}

WL_TEST(map_tail_severity) {
  WL_CHECK(severity_of(TailFlag::None) == Severity::None);
  // Learned-benign must NOT register as any threat.
  WL_CHECK(severity_of(TailFlag::Familiar) == Severity::None);
  WL_CHECK(severity_of(TailFlag::Watching) == Severity::Low);
  WL_CHECK(severity_of(TailFlag::PossibleTail) == Severity::Medium);
  WL_CHECK(severity_of(TailFlag::ConfirmedTail) == Severity::High);
}

WL_TEST(map_deauth_severity) {
  WL_CHECK(severity_of(DeauthFlag::None) == Severity::None);
  WL_CHECK(severity_of(DeauthFlag::Elevated) == Severity::Medium);
  WL_CHECK(severity_of(DeauthFlag::Flood) == Severity::High);
}

WL_TEST(map_spam_severity) {
  WL_CHECK(severity_of(SpamFlag::None) == Severity::None);
  WL_CHECK(severity_of(SpamFlag::Elevated) == Severity::Low);
  // BLE spam tops out at Medium on its own (Alert, not Critical).
  WL_CHECK(severity_of(SpamFlag::Spam) == Severity::Medium);
}

WL_TEST(map_beacon_severity) {
  WL_CHECK(severity_of(BeaconFlag::None) == Severity::None);
  WL_CHECK(severity_of(BeaconFlag::Elevated) == Severity::Low);
  // WiFi beacon flood, like BLE spam, tops out at Medium on its own.
  WL_CHECK(severity_of(BeaconFlag::Flood) == Severity::Medium);
}

WL_TEST(map_feed_beacon_routes_to_its_domain) {
  ThreatState ts;
  feed(ts, BeaconFlag::Flood, 10);
  WL_CHECK(ts.domain_severity(ThreatDomain::BeaconFlood) == Severity::Medium);
  WL_CHECK(ts.level() == ThreatLevel::Alert);
  WL_CHECK_EQ(ts.active_mask(),
              (uint8_t)(1u << (uint8_t)ThreatDomain::BeaconFlood));
}

// A confirmed FindMy-tracker follow reports under the Airtag domain (not Tail).
WL_TEST(map_feed_tracker_routes_to_airtag_domain) {
  ThreatState ts;
  feed_tracker(ts, TailFlag::ConfirmedTail, 100);
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
  WL_CHECK(ts.domain_severity(ThreatDomain::Tail) == Severity::None);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  WL_CHECK(ts.dominant() == ThreatDomain::Airtag);
}

// The Airtag domain is fed one verdict PER ADVERT, so a confirmed tracker and
// benign devices interleave. feed_tracker() must hold the confirmed level across
// the benign adverts instead of strobing to None (2026-07-29 field bug).
WL_TEST(map_feed_tracker_holds_through_benign_interleave) {
  ThreatState ts;
  feed_tracker(ts, TailFlag::ConfirmedTail, 100);        // the real tag
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);
  feed_tracker(ts, TailFlag::None, 100);                 // an unrelated device's advert
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);  // held, not stomped
  feed_tracker(ts, TailFlag::Familiar, 100);             // a benign familiar device
  WL_CHECK(ts.domain_severity(ThreatDomain::Airtag) == Severity::High);  // still held
  WL_CHECK(ts.level() == ThreatLevel::Critical);
}

// The per-AP RogueAp feed has the same shape: a benign AP classified between two
// sightings of a rogue AP must not drop the domain that the rogue raised.
WL_TEST(map_feed_rogue_holds_through_benign_interleave) {
  ThreatState ts;
  feed(ts, RogueFlag::OpenTwinOfSecuredSsid, 100);       // High: an evil twin
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::High);
  feed(ts, RogueFlag::None, 100);                        // a benign AP in the same scan
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::High);  // held
}

// ---- feed() reports under the correct domain and drives the aggregator ------
WL_TEST(map_feed_confirmed_tail_is_critical) {
  ThreatState ts;
  feed(ts, TailFlag::ConfirmedTail, 100);
  WL_CHECK(ts.level() == ThreatLevel::Critical);
  WL_CHECK(ts.dominant() == ThreatDomain::Tail);
  WL_CHECK_EQ(ts.active_mask(), (uint8_t)(1u << (uint8_t)ThreatDomain::Tail));
}

WL_TEST(map_feed_familiar_stays_calm) {
  ThreatState ts;
  feed(ts, TailFlag::Familiar, 100);
  WL_CHECK(ts.level() == ThreatLevel::Calm);
  WL_CHECK_EQ(ts.active_mask(), (uint8_t)0);
}

WL_TEST(map_feed_routes_each_detector_to_its_domain) {
  ThreatState ts;
  feed(ts, RogueFlag::NewBssidForKnownSsid, 10);  // RogueAp  -> Medium
  feed(ts, DeauthFlag::Flood, 10);                // DeauthFlood -> High
  feed(ts, SpamFlag::Elevated, 10);               // BleSpam -> Low
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::Medium);
  WL_CHECK(ts.domain_severity(ThreatDomain::DeauthFlood) == Severity::High);
  WL_CHECK(ts.domain_severity(ThreatDomain::BleSpam) == Severity::Low);
  WL_CHECK(ts.dominant() == ThreatDomain::DeauthFlood);   // highest severity
}

// ---- Correlation through the mapping layer: a rogue AP AND a deauth flood ---
// are each Medium+; together the aggregator pushes one step hotter.
WL_TEST(map_feed_two_credible_domains_escalate) {
  ThreatState ts;
  feed(ts, RogueFlag::SecurityDowngrade, 10);  // Medium -> Alert alone
  WL_CHECK(ts.level() == ThreatLevel::Alert);
  feed(ts, DeauthFlag::Elevated, 10);          // second Medium domain
  WL_CHECK(ts.level() == ThreatLevel::Critical);
}
