#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Distributed tracker reputation over the Meshtastic mesh.
//
// Threat Radar already decides, per watch, whether a device is following its
// wearer. This shares that verdict across the group: when a tail is CONFIRMED,
// the watch broadcasts a compact "TRFLAG|<hash>|<cat>" text on the active mesh
// channel. Every peer folds the hash into a local reputation store; when a
// peer's own detectors later see a MAC whose hash matches, Threat Radar
// escalates it immediately — so a stalker flagged by one rider warns the whole
// pack before local co-movement has had time to prove it independently.
//
// The MAC is shared only as a 32-bit FNV-1a hash, never in the clear, so the
// mesh carries "this device is bad" without broadcasting trackable addresses.
// (Caveat: trackers that rotate their BLE MAC — e.g. AirTags every ~15 min —
// only match across nodes within a rotation window; static-MAC devices
// — Flipper/skimmer/Flock/rogue-AP — match indefinitely.)
// ---------------------------------------------------------------------------

// Stable 32-bit hash of a 6-byte MAC. Same on every node, so hashes match.
uint32_t tracker_rep_hash(const uint8_t *mac6);

// Called by Threat Radar when a contact reaches CONFIRMED. Records the flag
// locally and, if it is new and the LoRa radio is active, broadcasts it once
// over the mesh. Deduplicated per hash so a persistent tail isn't re-announced.
void tracker_rep_flag_local(const uint8_t *mac6, uint8_t category);

// Mesh RX hook: inspect one received TEXT body. If it is a TRFLAG message it is
// folded into the reputation store and the function returns true — the caller
// should then NOT store it as a chat message. Returns false for normal text.
bool tracker_rep_handle_text(uint32_t from_node, const char *text, uint32_t len);

// Query: has this MAC been flagged as a stalker (by us or a peer)? On a match
// fills out_cat (category byte) and out_from (0 = flagged locally, else the
// peer node id) when the pointers are non-null.
bool tracker_rep_is_flagged(const uint8_t *mac6, uint8_t *out_cat, uint32_t *out_from);

int  tracker_rep_count();          // distinct, non-expired flags held
int  tracker_rep_remote_count();   // of those, how many came from peers
