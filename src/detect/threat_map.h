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

namespace detect {

// Translate a detector's live verdict to a unified Severity. Rationale for each
// mapping lives in threat_map.cpp. Overloaded on the (distinct) flag types.
Severity severity_of(RogueFlag f);
Severity severity_of(TailFlag f);
Severity severity_of(DeauthFlag f);
Severity severity_of(SpamFlag f);

// Convenience: translate a detector verdict and report it to the aggregator
// under that detector's fixed domain, in one call at the scan-loop call site.
// e.g. detect::feed(threat, evil_verdict.flag, now_sec);
void feed(ThreatState &ts, RogueFlag f, uint32_t t_sec);
void feed(ThreatState &ts, TailFlag f, uint32_t t_sec);
void feed(ThreatState &ts, DeauthFlag f, uint32_t t_sec);
void feed(ThreatState &ts, SpamFlag f, uint32_t t_sec);

} // namespace detect
