#pragma once
// Maps each detector's own verdict enum onto the shared (ThreatDomain, Severity)
// vocabulary the ThreatState aggregator consumes. This is the ONE place the
// per-detector -> unified translation lives, so the eventual scan-loop
// integration is a single feed() call per detector instead of severity
// judgments scattered across call sites. Pure: same-directory includes only,
// no Arduino / LVGL / hardware.
#include "threat_state.h"
#include "evil_twin.h"
#include "tail_detect.h"
#include "deauth_flood.h"
#include "ble_spam.h"
#include "beacon_flood.h"

namespace detect {

// Translate a detector's live verdict to a unified Severity. Rationale for each
// mapping lives in threat_map.cpp. Overloaded on the (distinct) flag types.
Severity severity_of(RogueFlag f);
Severity severity_of(TailFlag f);
Severity severity_of(DeauthFlag f);
Severity severity_of(SpamFlag f);
Severity severity_of(BeaconFlag f);

// Convenience: translate a detector verdict and report it to the aggregator
// under that detector's fixed domain, in one call at the scan-loop call site.
// e.g. detect::feed(threat, evil_verdict.flag, now_sec);
void feed(ThreatState &ts, RogueFlag f, uint32_t t_sec);
void feed(ThreatState &ts, TailFlag f, uint32_t t_sec);
void feed(ThreatState &ts, DeauthFlag f, uint32_t t_sec);
void feed(ThreatState &ts, SpamFlag f, uint32_t t_sec);
void feed(ThreatState &ts, BeaconFlag f, uint32_t t_sec);

// AirTag / Find My unwanted-tracker follow path. The follow evidence reuses a
// SEPARATE TailDetector instance fed ONLY sightings that tracker_ident flags as
// unwanted trackers (is_unwanted_tracker) - see src/detect/README.md. That
// TailFlag is reported under the Airtag domain (not Tail), so the aggregator
// tracks a physical tracker following the wearer distinctly from generic device
// tailing. Same TailFlag severities as feed(TailFlag), different domain.
void feed_tracker(ThreatState &ts, TailFlag f, uint32_t t_sec);

} // namespace detect
