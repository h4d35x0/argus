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

Severity severity_of(const DeviceVerdict &v)
{
    // A surveillance sighting's severity is the class's CEILING capped by how
    // sure we are it is that class. Two independent axes:
    //   * class ceiling - how alarming the device is if the match is real. A
    //     concealed camera in a private space is the worst privacy violation
    //     (High); an overt action cam / drone is the least covert (Low).
    //   * confidence cap - a name/SSID guess (Low) can never exceed Severity::Low
    //     no matter the class, while a verified UUID/OUI (High) lets the class
    //     ceiling stand. This keeps a broad "ipcam" SSID guess from alarming like
    //     a confirmed hidden camera.
    // severity = min(class_ceiling, confidence_cap).
    Severity ceiling;
    switch (v.cls) {
    case DeviceClass::None:          return Severity::None;
    // Covert recording of the wearer without consent; hidden cameras top the
    // privacy scale.
    case DeviceClass::HiddenCamera:  ceiling = Severity::High;   break;
    // Camera glasses and body cameras are credible covert-recording threats.
    case DeviceClass::CameraGlasses: ceiling = Severity::Medium; break;
    case DeviceClass::BodyCamera:    ceiling = Severity::Medium; break;
    // A non-Apple tracker (Tile / SmartTag / Chipolo) that could be a plant -
    // credible, mirrors the AirTag path's per-sighting weight (the FOLLOW proof
    // is tail_detect's job, not this gate), so Medium.
    case DeviceClass::BleTracker:    ceiling = Severity::Medium; break;
    // Action cams / drones record openly and are usually overt; awareness, not
    // alarm.
    case DeviceClass::ActionCamera:  ceiling = Severity::Low;    break;
    default:                         return Severity::None;
    }

    Severity cap;
    switch (v.conf) {
    case Confidence::None:   return Severity::None;
    case Confidence::Low:    cap = Severity::Low;    break;
    case Confidence::Medium: cap = Severity::Medium; break;
    case Confidence::High:   cap = Severity::High;   break;
    default:                 return Severity::None;
    }

    return (static_cast<uint8_t>(ceiling) < static_cast<uint8_t>(cap)) ? ceiling
                                                                       : cap;
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

void feed(ThreatState &ts, const DeviceVerdict &v, uint32_t t_sec)
{
    ts.report(ThreatDomain::Surveillance, severity_of(v), t_sec);
}

void feed_tracker(ThreatState &ts, TailFlag f, uint32_t t_sec)
{
    ts.report(ThreatDomain::Airtag, severity_of(f), t_sec);
}

} // namespace detect
