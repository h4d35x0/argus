// argus_mode.cpp - Daily/Defense/Offense state machine. See argus_mode.h and
// tasks/MODE-ARCHITECTURE-PLAN.md. Orthogonal to device_mode.* (radio arbiter);
// mirrors its Preferences persistence style.
#include "argus_mode.h"
#include <Preferences.h>

// NVS namespace. "argusnotify" (device_mode) and "argussec" (PIN store, P5) are
// taken; this one is distinct.
static const char *NS = "argusmode";

static ArgusMode s_mode        = ArgusMode::Daily;   // RAM; boot forces Daily/Defense
static ArgusMode s_prev_mode   = ArgusMode::Daily;   // where lock_offense() returns to
static bool      s_unlocked    = false;              // RAM-only Offense session flag
static bool      s_locked_out  = false;              // cache of the persisted flag
static bool      s_def_persist = false;              // cache of the persisted setting

static ArgusWipeFn s_wipe_hook = nullptr;

#define ARGUS_MODE_MAX_CBS 6
static ArgusModeCb s_cbs[ARGUS_MODE_MAX_CBS] = {0};
static uint8_t     s_cb_n = 0;

static void broadcast()
{
    for (uint8_t i = 0; i < s_cb_n; i++)
        if (s_cbs[i]) s_cbs[i](s_mode);
}

// Persist the boot mode only when Defense-persistence is on (Offense is never
// written). Daily/Defense stored as the ArgusMode value in "lastmode".
static void persist_last_mode()
{
    if (!s_def_persist) return;
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putUChar("lastmode", (uint8_t)s_mode);
    p.end();
}

void argus_mode_init()
{
    Preferences p;
    if (p.begin(NS, true)) {
        s_locked_out  = p.getBool("lockout", false);
        s_def_persist = p.getBool("defpersist", false);
        uint8_t last  = p.getUChar("lastmode", (uint8_t)ArgusMode::Daily);
        p.end();
        // Boot into Daily unless the user opted into Defense-persistence AND the
        // last mode was Defense. Offense (2) is never restored.
        s_mode = (s_def_persist && last == (uint8_t)ArgusMode::Defense)
                     ? ArgusMode::Defense
                     : ArgusMode::Daily;
    } else {
        s_mode = ArgusMode::Daily;
    }
    s_prev_mode = s_mode;
    s_unlocked  = false;
}

ArgusMode argus_mode_current() { return s_mode; }

bool argus_mode_set(ArgusMode m)
{
    if (m == ArgusMode::Offense) return false;   // must go through enter_offense()
    if (s_mode == ArgusMode::Offense) return false;  // leave Offense via lock_offense()
    if (m == s_mode) return false;               // no change
    s_mode      = m;
    s_prev_mode = m;
    persist_last_mode();
    broadcast();
    return true;
}

bool is_offense_unlocked() { return s_unlocked; }

bool enter_offense()
{
    if (s_locked_out) return false;              // shred is permanent (until reflash)
    if (s_mode != ArgusMode::Offense) s_prev_mode = s_mode;
    s_mode     = ArgusMode::Offense;
    s_unlocked = true;
    broadcast();                                 // NOT persisted, by design
    return true;
}

void lock_offense()
{
    s_unlocked = false;
    s_mode = (s_prev_mode == ArgusMode::Offense) ? ArgusMode::Daily : s_prev_mode;
    broadcast();
}

void offense_shred()
{
    // Burn the lockout flag FIRST so a power loss mid-wipe still disables Offense.
    s_locked_out = true;
    {
        Preferences p;
        if (p.begin(NS, false)) { p.putBool("lockout", true); p.end(); }
    }
    if (s_wipe_hook) s_wipe_hook();              // P5's SD/NVS payload wipe
    s_unlocked = false;
    // Leave s_mode to the caller (the duress UI shows a fake-empty Offense screen);
    // enter_offense() now permanently fails regardless.
}

bool offense_locked_out() { return s_locked_out; }

void argus_mode_set_wipe_hook(ArgusWipeFn fn) { s_wipe_hook = fn; }

void argus_mode_on_change(ArgusModeCb cb)
{
    if (cb && s_cb_n < ARGUS_MODE_MAX_CBS) s_cbs[s_cb_n++] = cb;
}

bool argus_mode_defense_persist() { return s_def_persist; }

void argus_mode_set_defense_persist(bool on)
{
    s_def_persist = on;
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putBool("defpersist", on);
    // Turning it on now snapshots the current mode as the boot mode; turning it
    // off clears the stored mode so a stale Defense can't resurrect later.
    if (on) p.putUChar("lastmode", (uint8_t)s_mode);
    else    p.putUChar("lastmode", (uint8_t)ArgusMode::Daily);
    p.end();
}
