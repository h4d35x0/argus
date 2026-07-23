#include "offense_wipe.h"
#include "argus_mode.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdint.h>

// Tier-1 offensive-data shred (scope signed off 2026-07-22). offense_shred()
// (the duress-PIN path) calls this via argus_mode's wipe hook AFTER it has
// already burned the persistent Offense lockout, so a power loss mid-wipe still
// leaves Offense disabled.
//
// SCOPE (Tier 1): destroy ONLY the directories that hold direct evidence of
// offensive activity, and nothing else:
//   /pwn         WPA handshake captures (.pcap)
//   /Wardrive    geolocated WiFi/BT recon logs (.csv)
//   /PingSweeps  network ping sweeps + port scans (.txt)
//   /Screenshots screen grabs (may show the offensive UI / capture lists)
//
// Everything else is deliberately PRESERVED, and that is the point:
//  - the defensive detection logs (/AirTag, /Flipper, /Skimmers, /EvilTwin,
//    /Flock, /ThreatRadar, /CounterTail, /HexHound) are the wearer's cover -
//    a privacy-conscious victim, not an operator;
//  - comms / config / saved creds keep the watch functional and unremarkable
//    (an emptied watch reads as tampering);
//  - NVS is untouched. The argus_mode lockout flag (set by offense_shred) and
//    the argussec PIN store MUST survive, or the shred would unlock itself.
// See tasks/OFFENSE-UNLOCK-PLAN.md for the full decision.
//
// Best-effort secure erase: each file is overwritten with zeros in place before
// unlink. On a wear-levelled SD this is not a guaranteed forensic wipe (the
// controller may remap physical blocks), but it defeats trivial undelete and is
// the most we can do without raw-block access. Directories are emptied then
// removed so no incriminating directory *name* survives either.

static const char *TIER1_DIRS[] = {
    "/pwn", "/Wardrive", "/PingSweeps", "/Screenshots",
};

// Overwrite a file's bytes with zeros in place (no truncate), then unlink it.
static void overwrite_and_remove(const char *path, uint32_t size)
{
    File f = SD.open(path, "r+");                 // read/write, keep existing length
    if (f) {
        static const uint8_t zeros[512] = {0};
        f.seek(0);
        uint32_t left = size;
        while (left) {
            size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
            if (f.write(zeros, n) != n) break;    // give up overwriting; still unlink
            left -= n;
        }
        f.flush();
        f.close();
    }
    SD.remove(path);
}

// Empty a directory (recursively) then remove the directory itself. Reopens the
// directory after each deletion rather than deleting mid-iteration, which some
// FAT drivers mishandle - simple and correct over clever. File counts here are
// modest, so the extra opens are cheap. Child paths are rebuilt from the parent
// plus the basename, so it does not matter whether name() returns a full path.
static void wipe_tree(const char *dirpath)
{
    for (;;) {
        File dir = SD.open(dirpath);
        if (!dir) return;                         // gone / never existed
        File e = dir.openNextFile();
        if (!e) { dir.close(); break; }           // empty -> done

        const char *nm    = e.name();
        const char *slash = strrchr(nm, '/');
        const char *base  = slash ? slash + 1 : nm;
        char child[160];
        snprintf(child, sizeof(child), "%s/%s", dirpath, base);
        bool     is_dir = e.isDirectory();
        uint32_t sz     = e.size();
        e.close();
        dir.close();

        if (is_dir) wipe_tree(child);
        else        overwrite_and_remove(child, sz);
    }
    SD.rmdir(dirpath);
}

// Shared guard: the host owns the card while USB-MSC is exposing it, and there is
// nothing to wipe without a mounted card. Both the duress hook and the operator
// Loot screen refuse to run unless this holds.
static bool wipe_allowed(void)
{
    return instance.isCardReady() && !usb_sd_is_running();
}

// Is dirpath one of the canonical Tier-1 loot dirs? Scope guard so an operator
// call can never shred a directory outside the signed-off set.
static bool is_loot_dir(const char *dirpath)
{
    if (!dirpath) return false;
    for (size_t i = 0; i < sizeof(TIER1_DIRS) / sizeof(TIER1_DIRS[0]); i++)
        if (!strcmp(dirpath, TIER1_DIRS[i])) return true;
    return false;
}

const char *const *offense_loot_dirs(size_t *count)
{
    if (count) *count = sizeof(TIER1_DIRS) / sizeof(TIER1_DIRS[0]);
    return TIER1_DIRS;
}

bool offense_wipe_dir(const char *dirpath)
{
    if (!wipe_allowed())       return false;
    if (!is_loot_dir(dirpath)) return false;   // scope guard
    if (SD.exists(dirpath)) wipe_tree(dirpath);
    return true;
}

bool offense_wipe_loot_all(void)
{
    if (!wipe_allowed()) return false;
    for (size_t i = 0; i < sizeof(TIER1_DIRS) / sizeof(TIER1_DIRS[0]); i++)
        if (SD.exists(TIER1_DIRS[i])) wipe_tree(TIER1_DIRS[i]);
    return true;
}

// The duress-PIN wipe hook, run after offense_shred() burns the lockout. Now
// shares the exact all-dirs path the operator Loot screen uses.
static void offense_tier1_wipe(void)
{
    offense_wipe_loot_all();
}

void offense_wipe_register(void)
{
    argus_mode_set_wipe_hook(offense_tier1_wipe);
}
