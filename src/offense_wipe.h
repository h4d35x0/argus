#pragma once
#include <stddef.h>

// Registers the Tier-1 offensive-data shred as the argus_mode wipe hook.
// Call once at setup(), after the SD subsystem is up. offense_shred() (the
// duress path) invokes the hook after burning the persistent Offense lockout.
// Scope + rationale live in offense_wipe.cpp and tasks/OFFENSE-UNLOCK-PLAN.md.
void offense_wipe_register(void);

// Canonical Tier-1 loot directories (the SAME set the duress shred targets):
// /pwn /Wardrive /PingSweeps /Screenshots. Single source of truth so the Loot
// screen's scope can never drift from the shred's. Do not free the return.
const char *const *offense_loot_dirs(size_t *count);

// Operator-initiated shred of ONE loot directory (overwrite-then-unlink, then
// rmdir), reusing the exact duress-shred code path. Returns false and does
// nothing if the SD isn't ready / is exposed over USB, or dirpath is not one of
// offense_loot_dirs() (scope guard).
bool offense_wipe_dir(const char *dirpath);

// Operator-initiated shred of ALL loot directories (same guard). This is the
// same function the duress wipe hook now calls internally.
bool offense_wipe_loot_all(void);
