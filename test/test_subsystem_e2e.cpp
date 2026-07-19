// test_subsystem_e2e.cpp - END-TO-END detection subsystem test. The per-module
// tests prove each unit; this proves they COMPOSE: real detector verdicts ->
// threat_map -> threat_state (correlation + decay) -> threat_log, over a realistic
// attack timeline. It is the host-side proof of the exact chain the hardware
// integration will wire up, so a composition bug surfaces here, not on-device.
// Everything is pure and clockless: time is a plain t_sec threaded through.
#include "wl_test.h"
#include "evil_twin.h"
#include "deauth_flood.h"
#include "threat_map.h"
#include "threat_state.h"
#include "threat_log.h"

#include <cstdint>
#include <cstring>

using namespace detect;

static ApObservation mkAp(const uint8_t bssid[6], const char *ssid,
                          uint8_t ch, AuthMode a) {
  ApObservation o{};
  memcpy(o.bssid, bssid, 6);
  strncpy(o.ssid, ssid, sizeof(o.ssid) - 1);
  o.channel = ch;
  o.rssi = -50;
  o.auth_mode = a;
  return o;
}

static MgmtFrameEvent mkDeauth(const uint8_t bssid[6], uint32_t t) {
  MgmtFrameEvent e{};
  memcpy(e.bssid, bssid, 6);
  e.type = MgmtType::Deauth;
  e.t_sec = t;
  e.rssi = -30;
  return e;
}

// Evil twin appears, then a concurrent deauth flood (active MITM), then the
// attack stops and the posture decays back to Calm - asserting the whole chain
// and that the forensic log captured exactly the rises and the clears.
WL_TEST(e2e_rogue_ap_plus_deauth_flood_escalates_and_logs) {
  RogueApDetector rogue;
  DeauthFloodDetector deauth;
  ThreatState ts;
  ThreatLog log;

  const uint8_t bssidA[6] = {0xAA, 0, 0, 0, 0, 1};
  const uint8_t bssidB[6] = {0xAA, 0, 0, 0, 0, 2};  // 2nd radio, same SSID = twin
  const uint8_t bssidC[6] = {0xCC, 0, 0, 0, 0, 1};

  // Mirror the documented integration: every cycle, poll EVERY relevant domain
  // into the log (not just the one that changed). This sets each domain's None
  // baseline at boot so a later first-rise is captured, and is exactly what the
  // firmware loop does. Polling only on change would let a domain that goes hot
  // before its first poll have that rise swallowed as its baseline.
  auto poll = [&](uint32_t tt) {
    log.update(ThreatDomain::RogueAp,
               ts.domain_severity(ThreatDomain::RogueAp), tt);
    log.update(ThreatDomain::DeauthFlood,
               ts.domain_severity(ThreatDomain::DeauthFlood), tt);
  };

  uint32_t t = 1000;

  // Baseline: first sighting of HomeNet is normal discovery, nothing logged.
  {
    RogueVerdict v = rogue.ingest(mkAp(bssidA, "HomeNet", 6, AuthMode::WPA2));
    feed(ts, v.flag, t);
    poll(t);
    WL_CHECK(v.flag == RogueFlag::None);
    WL_CHECK(ts.level() == ThreatLevel::Calm);
    WL_CHECK(log.total_recorded() == 0);   // baselines only, nothing logged
  }

  // Evil twin: a 2nd BSSID advertises HomeNet -> RogueAp Medium -> Alert, logged.
  t += 5;
  {
    RogueVerdict v = rogue.ingest(mkAp(bssidB, "HomeNet", 6, AuthMode::WPA2));
    feed(ts, v.flag, t);
    poll(t);
    WL_CHECK(v.flag == RogueFlag::NewBssidForKnownSsid);
    WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::Medium);
    WL_CHECK(ts.level() == ThreatLevel::Alert);
    WL_CHECK(log.total_recorded() == 1);   // RogueAp None->Medium
  }

  // Concurrent deauth flood from a third radio -> DeauthFlood High. Now two
  // domains are hot (RogueAp Medium + DeauthFlood High) = active MITM -> Critical.
  t += 5;
  DeauthVerdict dv{};
  for (int i = 0; i < 60; i++) dv = deauth.ingest(mkDeauth(bssidC, t));  // >= 50
  {
    feed(ts, dv.flag, t);
    poll(t);
    WL_CHECK(dv.flag == DeauthFlag::Flood);
    WL_CHECK(ts.domain_severity(ThreatDomain::DeauthFlood) == Severity::High);
    WL_CHECK(ts.level() == ThreatLevel::Critical);
    WL_CHECK(log.total_recorded() == 2);   // + DeauthFlood None->High
  }
  WL_CHECK(ts.dominant() == ThreatDomain::DeauthFlood);

  // Attack stops: nothing re-reported, decay relaxes both domains to None.
  uint32_t t2 = t + ThreatState::kDecaySec * 4;  // enough steps to clear High
  ts.tick(t2);
  poll(t2);
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::None);
  WL_CHECK(ts.domain_severity(ThreatDomain::DeauthFlood) == Severity::None);
  WL_CHECK(ts.level() == ThreatLevel::Calm);
  WL_CHECK(log.total_recorded() == 4);  // + two clears

  // The log is reviewable: the oldest retained edge formats to a real line.
  char line[64];
  size_t n = ThreatLog::format(log.at(0), line, sizeof(line));
  WL_CHECK(n > 0);
  WL_CHECK(line[n] == '\0');
}

// Correlation value: two MEDIUM threats at once are worse than either alone.
// Neither an evil twin (Medium) nor an elevated-but-not-flood deauth rate
// (Medium) is Critical by itself (both Alert), but together the aggregator's
// correlation rule pushes the posture one step hotter -> Critical.
WL_TEST(e2e_two_medium_threats_correlate_to_critical) {
  RogueApDetector rogue;
  DeauthFloodDetector deauth;
  ThreatState ts;

  const uint8_t a[6] = {0xAA, 0, 0, 0, 0, 1};
  const uint8_t b[6] = {0xAA, 0, 0, 0, 0, 2};
  const uint8_t c[6] = {0xCC, 0, 0, 0, 0, 9};
  uint32_t t = 2000;

  rogue.ingest(mkAp(a, "Net", 6, AuthMode::WPA2));               // baseline
  RogueVerdict rv = rogue.ingest(mkAp(b, "Net", 6, AuthMode::WPA2));  // Medium
  feed(ts, rv.flag, t);
  WL_CHECK(ts.domain_severity(ThreatDomain::RogueAp) == Severity::Medium);
  WL_CHECK(ts.level() == ThreatLevel::Alert);  // one Medium alone

  DeauthVerdict dv{};
  for (int i = 0; i < 12; i++) dv = deauth.ingest(mkDeauth(c, t));  // >=10, <50
  feed(ts, dv.flag, t);
  WL_CHECK(dv.flag == DeauthFlag::Elevated);
  WL_CHECK(ts.domain_severity(ThreatDomain::DeauthFlood) == Severity::Medium);
  WL_CHECK(ts.level() == ThreatLevel::Critical);  // correlation escalation
}
