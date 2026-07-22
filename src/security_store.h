#pragma once
#include <stdint.h>

// Two-PIN store for the Offense unlock. PINs are NEVER stored - only salted
// PBKDF2-HMAC-SHA256 hashes in NVS ("argussec"). One PIN reveals Offense; a
// separate, longer SHRED PIN triggers the duress self-destruct. Only an EXACT
// hash match returns Unlock/Shred; every other input is None (and rate-limited).
//
// NOTE: on this un-encrypted board a flash dump can read the salt+hash and brute
// force offline - the on-device rate-limit here is defence-in-depth, not the main
// protection (that would be flash encryption, deliberately deferred).

enum class PinResult : uint8_t { None = 0, Unlock, Shred };

bool        security_pins_set(void);   // true once both PINs are configured
// Validate + store the pair. Returns nullptr on success, else a reason string
// (4-8 digits; shred >= unlock length + 1; distinct; no shared prefix).
const char *security_set_pins(const char *unlock_pin, const char *shred_pin);
PinResult   security_check(const char *pin);      // rate-limited exact-match check
uint32_t    security_lockout_ms(void);            // >0 while rate-limit-locked
