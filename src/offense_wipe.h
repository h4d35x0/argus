#pragma once

// Registers the Tier-1 offensive-data shred as the argus_mode wipe hook.
// Call once at setup(), after the SD subsystem is up. offense_shred() (the
// duress path) invokes the hook after burning the persistent Offense lockout.
// Scope + rationale live in offense_wipe.cpp and tasks/OFFENSE-UNLOCK-PLAN.md.
void offense_wipe_register(void);
