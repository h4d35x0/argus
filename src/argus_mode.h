#pragma once
#include <stdint.h>

// ARGUS mode state machine (Daily / Defense / Offense) - the single source of
// truth the whole UI, theming and gating hang off. Kept STRICTLY ORTHOGONAL to
// device_mode.{h,cpp} (the WiFi<->BLE radio/notification arbiter): that stays as
// is. See tasks/MODE-ARCHITECTURE-PLAN.md.
//
//   DAILY   - default boot. Innocent smartwatch; Tools grid + detectors +
//             offensive tiles hidden. A reboot always lands here UNLESS the user
//             has explicitly enabled Defense-persistence (a setting).
//   DEFENSE - openly selectable. Passive anti-surveillance detectors + alerting.
//             No offensive tiles.
//   OFFENSE - HIDDEN. Reached only via the side-button (BOOT/GPIO0) knock -> PIN
//             pad -> correct UNLOCK-PIN. NEVER persisted; always re-unlock after
//             a reboot. Blocked entirely once the device is "shred" locked out.

enum class ArgusMode : uint8_t { Daily = 0, Defense = 1, Offense = 2 };

// Call once, EARLY in setup() (before screens are created). Loads the persisted
// lockout + Defense-persistence flags and sets the boot mode: Daily by default,
// or Defense iff Defense-persistence is enabled and Defense was the last mode.
// Offense is never restored.
void argus_mode_init();

ArgusMode argus_mode_current();

// Switch between Daily and Defense only. REJECTS Offense (use enter_offense()).
// Returns false on a rejected/again-same request. Persists the new mode as the
// boot mode only while Defense-persistence is enabled.
bool argus_mode_set(ArgusMode m);

// RAM-only session flag; true only between a successful enter_offense() and the
// next lock_offense()/reboot.
bool is_offense_unlocked();

// Enter Offense. CALLER MUST HAVE VERIFIED THE UNLOCK-PIN. RAM-only, never
// persisted. Returns false (and does nothing) if the device is locked out.
bool enter_offense();

// Leave Offense back to the previous non-offense mode (Daily/Defense).
void lock_offense();

// Duress self-destruct. Burns the persistent lockout flag FIRST (so a power loss
// mid-wipe still leaves Offense disabled), then runs the registered wipe hook
// (P5 supplies the real SD/NVS wipe). After this, enter_offense() always fails
// until an out-of-band full flash+NVS erase + reflash. The fake-unlock decoy UI
// is the PIN pad's job (P5), not this function's.
void offense_shred();

// True once shred has run (persisted). Firmware honours this at boot.
bool offense_locked_out();

// P5 registers the actual offensive-data wipe here; offense_shred() calls it
// after burning the lockout flag. Null until P5 wires it (P1 just locks out).
typedef void (*ArgusWipeFn)(void);
void argus_mode_set_wipe_hook(ArgusWipeFn fn);

// Broadcast on every mode change (incl. enter/lock offense, shred). Theme + UI
// subscribe to re-render. Up to ARGUS_MODE_MAX_CBS subscribers.
typedef void (*ArgusModeCb)(ArgusMode now);
void argus_mode_on_change(ArgusModeCb cb);

// Defense-persistence SETTING (user-toggleable, persisted). Default OFF so the
// out-of-the-box behaviour keeps the "a reboot always shows Daily" opsec
// guarantee; enabling it lets the watch boot back into Defense for convenience.
bool argus_mode_defense_persist();
void argus_mode_set_defense_persist(bool on);
