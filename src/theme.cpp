#include "theme.h"
#include "threat_radar.h"

// Runtime, state-aware brand accent (ARGUS -> HADES).
//
// The compile-time ARGUS_ACCENT macro paints the calm steel-blue resting brand
// at ~67 low-traffic sites. This function is its live, threat-aware sibling:
// callers that repaint frequently (the clock status bar, the Threat Radar
// screen) call argus_accent() instead of the macro so the accent tracks the
// threat state. When Threat Radar has a contact at TR_LVL_LIKELY or above — i.e.
// something is co-moving with the wearer — the accent flips to HADES_RED so the
// watch visibly "opens its red eyes"; otherwise it stays steel-blue. The flip is
// glanceable and returns to calm on its own once the tail clears the staleness
// window (threatradar_top_level() reads only live contacts).
lv_color_t argus_accent(void)
{
    return threatradar_top_level() >= TR_LVL_LIKELY ? HADES_RED : ARGUS_ACCENT;
}
