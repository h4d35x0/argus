#include "threat_map.h"

namespace detect {

// --- Severity mappings ------------------------------------------------------
// The scale is deliberately conservative: a single Medium is Alert, not
// Critical. The aggregator's correlation rule (two+ domains at Medium+) is what
// escalates concurrent threats, so each detector should report the severity of
// ITS evidence alone, without pre-inflating for "what if something else is
// happening too".

Severity severity_of(RogueFlag f)
{
    switch (f) {
    case RogueFlag::None:                  return Severity::None;
    // Benign causes exist (DFS, auto-channel selection), so a channel hop alone
    // is only a faint signal.
    case RogueFlag::ChannelChangeForBssid: return Severity::Low;
    // A second radio claiming a known SSID is the classic evil twin, but legit
    // roaming/mesh also shows many BSSIDs per SSID - credible, not certain.
    case RogueFlag::NewBssidForKnownSsid:  return Severity::Medium;
    // A known SSID appearing with weaker encryption is a credible harvest setup.
    case RogueFlag::SecurityDowngrade:     return Severity::Medium;
    // A normally-secured SSID now advertised fully OPEN is an unambiguous
    // credential-harvest twin - the severe case.
    case RogueFlag::OpenTwinOfSecuredSsid: return Severity::High;
    }
    return Severity::None;
}

Severity severity_of(TailFlag f)
{
    switch (f) {
    case TailFlag::None:          return Severity::None;
    // Learned benign (a fixture at a place the wearer frequents) - not a threat.
    case TailFlag::Familiar:      return Severity::None;
    case TailFlag::Watching:      return Severity::Low;      // early cross-cell
    case TailFlag::PossibleTail:  return Severity::Medium;   // sustained
    case TailFlag::ConfirmedTail: return Severity::High;     // long, wide follow
    }
    return Severity::None;
}

Severity severity_of(DeauthFlag f)
{
    switch (f) {
    case DeauthFlag::None:     return Severity::None;
    case DeauthFlag::Elevated: return Severity::Medium;  // abnormal mgmt-frame rate
    case DeauthFlag::Flood:    return Severity::High;    // active deauth attack
    }
    return Severity::None;
}

Severity severity_of(SpamFlag f)
{
    switch (f) {
    case SpamFlag::None:     return Severity::None;
    case SpamFlag::Elevated: return Severity::Low;
    // BLE spam is an active, disruptive attack but it harasses rather than
    // compromises the wearer, so it tops out at Medium (Alert) on its own; it
    // reaches Critical only in correlation with another concurrent threat.
    case SpamFlag::Spam:     return Severity::Medium;
    }
    return Severity::None;
}

Severity severity_of(BeaconFlag f)
{
    switch (f) {
    case BeaconFlag::None:     return Severity::None;
    case BeaconFlag::Elevated: return Severity::Low;
    // A WiFi beacon flood is disruptive (buries real APs, confuses scanners)
    // but harasses rather than compromises the wearer, so - like BLE spam - it
    // tops out at Medium (Alert) alone and reaches Critical only in correlation.
    case BeaconFlag::Flood:    return Severity::Medium;
    }
    return Severity::None;
}

// --- feed() convenience -----------------------------------------------------
// Each detector owns exactly one ThreatDomain; report the mapped severity there.

void feed(ThreatState &ts, RogueFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::RogueAp, severity_of(f), t_sec);
}

void feed(ThreatState &ts, TailFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::Tail, severity_of(f), t_sec);
}

void feed(ThreatState &ts, DeauthFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::DeauthFlood, severity_of(f), t_sec);
}

void feed(ThreatState &ts, SpamFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::BleSpam, severity_of(f), t_sec);
}

void feed(ThreatState &ts, BeaconFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::BeaconFlood, severity_of(f), t_sec);
}

void feed_tracker(ThreatState &ts, TailFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::Airtag, severity_of(f), t_sec);
}

} // namespace detect
