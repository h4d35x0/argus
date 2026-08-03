#pragma once
//
// detect_log_sd.h - SD-side enforcement of the detection-log retention policy.
//
// The policy itself (ages, caps, timestamp parsing) is pure and lives in
// src/detect/log_retention.h so the host suite can test it. This file is the
// hardware half: it walks the actual files on the card.
//
// Every entry point here is a no-op unless the card is mounted AND the host is
// not holding it over USB mass storage, matching the guard every other SD
// writer in this codebase uses.

#include <stdint.h>

// Enforce retention on one append-style log, e.g. "/AirTag/discovered.txt".
// Call immediately after appending a record. Cheap in the common case: it only
// rewrites the file when entries actually need to be dropped.
void detect_log_enforce(const char *path);

// Enforce retention on a directory that stores one file per hit (/Flock).
// Deletes files whose "YYYYMMDD_HHMMSS" name falls outside the age window, then
// evicts oldest-first if the count still exceeds the cap.
void detect_log_prune_dir(const char *dir);

// Sweep every detection log at once. Called at boot so records also expire on a
// watch that simply has not detected anything in a month.
void detect_log_sweep_all();

// Delete every detection record on the card. Returns the number of files
// removed or emptied. Backs the "Clear detection logs" action in Settings.
int detect_log_clear_all();

// Total retained records across all detection logs, for the Settings readout.
// Counts lines in the append logs plus files under /Flock.
int detect_log_total_records();
