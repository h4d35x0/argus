// wifi_pass_store.h - remembered WiFi passwords on the SD card.
//
// File: /Settings/wifi_passwords.txt, one "SSID=password" per line (lines
// starting with # are comments). Lets the watch reconnect to known networks
// without re-typing: tapping a saved network connects straight away; an unknown
// one still prompts. You can also just drop the file on the SD by hand.
//
// Stored in plaintext, like every desktop OS's WiFi store - it lives on the
// user's own removable SD. SSIDs containing '=' are not supported (the first '='
// splits SSID from password, so passwords may contain '=').
#pragma once
#include <stddef.h>

// Look up the saved password for `ssid`. On a hit, copies it into `out` (always
// NUL-terminated) and returns true. Returns false if there's no SD, no file, or
// no matching entry.
bool wifi_pass_lookup(const char *ssid, char *out, size_t out_len);

// Remember `ssid` -> `password` (inserts or updates the line). No-op if the SD
// isn't available. Called after a successful connect so a typed password is
// remembered for next time.
void wifi_pass_save(const char *ssid, const char *password);
