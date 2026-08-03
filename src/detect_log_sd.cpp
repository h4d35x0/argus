#include "detect_log_sd.h"
#include "detect/log_retention.h"
#include "usb_sd.h"                 // usb_sd_is_running()
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>

void clock_screen_get_local_time(struct tm *out);   // main.cpp

// Every append-style detection log. Each writes one record per line, prefixed
// with the same "YYYY-MM-DD HH:MM:SS" stamp.
static const char *const kAppendLogs[] = {
    "/ThreatRadar/discovered.txt",
    "/AirTag/discovered.txt",
    "/Flipper/discovered.txt",
    "/Skimmers/discovered.txt",
    "/EvilTwin/discovered.txt",
};
static const int kAppendLogCount = (int)(sizeof(kAppendLogs) / sizeof(kAppendLogs[0]));

// Flock writes one file per hit, named "YYYYMMDD_HHMMSS.txt".
static const char *const kFlockDir = "/Flock";

// Longest record we will carry through a compaction pass. Real records run
// ~150-250 bytes; anything longer is truncated in the copy rather than
// dropped, so a malformed line can never silently delete evidence.
static const size_t kLineMax = 320;

static bool sd_usable()
{
    return instance.isCardReady() && !usb_sd_is_running();
}

// Seconds-since-epoch in the same LOCAL frame the detectors stamp with.
static long now_local_sec()
{
    struct tm t;
    clock_screen_get_local_time(&t);
    return detlog::days_from_civil(t.tm_year + 1900,
                                   (unsigned)(t.tm_mon + 1),
                                   (unsigned)t.tm_mday) * detlog::kSecondsPerDay
         + (long)t.tm_hour * 3600L + (long)t.tm_min * 60L + (long)t.tm_sec;
}

// Read one newline-terminated line into buf. Returns false at EOF.
// Over-long lines are truncated in buf but consumed to the newline, so the
// reader never desynchronises from record boundaries.
static bool read_line(File &f, char *buf, size_t cap, bool *truncated)
{
    size_t n = 0;
    int c;
    bool got = false;
    *truncated = false;
    while ((c = f.read()) >= 0) {
        got = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) buf[n++] = (char)c;
        else             *truncated = true;
    }
    buf[n] = '\0';
    return got;
}

void detect_log_enforce(const char *path)
{
    if (!sd_usable() || !path || !SD.exists(path)) return;

    const long now = now_local_sec();

    // --- Pass 1: count records and find the first one inside the age window.
    // Records are appended chronologically, so retention only ever removes a
    // prefix. That is what lets pass 2 be a single streaming copy.
    int  records        = 0;
    int  first_unexpired = -1;
    {
        File f = SD.open(path, FILE_READ);
        if (!f) return;
        char line[kLineMax];
        bool trunc;
        while (read_line(f, line, sizeof(line), &trunc)) {
            long stamp;
            if (!detlog::parse_stamp(line, &stamp)) continue;   // continuation line
            if (first_unexpired < 0 &&
                !detlog::is_expired(stamp, now, detlog::kMaxAgeSeconds))
                first_unexpired = records;
            records++;
        }
        f.close();
    }
    if (records == 0) return;
    if (first_unexpired < 0) first_unexpired = records;   // everything expired

    const int keep_from =
        detlog::first_kept(records, first_unexpired, detlog::kMaxEntries);
    if (keep_from <= 0) return;                            // nothing to drop

    // --- Pass 2: stream the survivors into a temp file, then swap.
    // Temp-then-rename rather than truncate-in-place: a power loss mid-rewrite
    // must not be able to destroy the log.
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    SD.remove(tmp);

    File in = SD.open(path, FILE_READ);
    if (!in) return;
    File out = SD.open(tmp, FILE_WRITE);
    if (!out) { in.close(); return; }

    char line[kLineMax];
    bool trunc;
    int  idx  = -1;      // index of the record the current line belongs to
    bool keep = false;
    while (read_line(in, line, sizeof(line), &trunc)) {
        long stamp;
        if (detlog::parse_stamp(line, &stamp)) {
            idx++;
            keep = (idx >= keep_from);
        }
        if (keep) { out.print(line); out.print("\n"); }
    }
    out.close();
    in.close();

    SD.remove(path);
    SD.rename(tmp, path);
}

// Parse "YYYYMMDD_HHMMSS" (the Flock filename shape) into local epoch seconds.
static bool parse_flock_name(const char *base, long *out_sec)
{
    // Reuse the pure parser by reshaping the name into "YYYY-MM-DD HH:MM:SS".
    if (!base) return false;
    for (int i = 0; i < 15; i++) {
        if (i == 8) { if (base[i] != '_') return false; continue; }
        if (base[i] < '0' || base[i] > '9') return false;
    }
    char re[20];
    re[0]=base[0]; re[1]=base[1]; re[2]=base[2];  re[3]=base[3];  re[4]='-';
    re[5]=base[4]; re[6]=base[5]; re[7]='-';
    re[8]=base[6]; re[9]=base[7]; re[10]=' ';
    re[11]=base[9];  re[12]=base[10]; re[13]=':';
    re[14]=base[11]; re[15]=base[12]; re[16]=':';
    re[17]=base[13]; re[18]=base[14]; re[19]='\0';
    return detlog::parse_stamp(re, out_sec);
}

static const char *basename_of(const char *nm)
{
    const char *slash = strrchr(nm, '/');
    return slash ? slash + 1 : nm;
}

void detect_log_prune_dir(const char *dir)
{
    if (!sd_usable() || !dir || !SD.exists(dir)) return;
    const long now = now_local_sec();

    // Age pass: delete anything outside the window, counting what survives.
    int kept = 0;
    {
        File d = SD.open(dir);
        if (!d) return;
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            if (e.isDirectory()) { e.close(); continue; }
            char full[96];
            snprintf(full, sizeof(full), "%s", e.name());
            const char *base = basename_of(full);
            long stamp;
            bool drop = parse_flock_name(base, &stamp) &&
                        detlog::is_expired(stamp, now, detlog::kMaxAgeSeconds);
            char path[96];
            snprintf(path, sizeof(path), "%s/%s", dir, base);
            e.close();
            if (drop) SD.remove(path);
            else      kept++;
        }
        d.close();
    }

    // Cap pass: evict oldest-first until under the cap. The filenames sort
    // lexicographically by time, so "oldest" is just the smallest name. This
    // rescans per eviction, which is O(n^2), but it only runs when a single
    // dense day blew past the cap and n is bounded by that cap.
    while (kept > detlog::kMaxEntries) {
        char oldest[96];
        oldest[0] = '\0';
        File d = SD.open(dir);
        if (!d) return;
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            if (!e.isDirectory()) {
                const char *base = basename_of(e.name());
                if (oldest[0] == '\0' || strcmp(base, oldest) < 0)
                    snprintf(oldest, sizeof(oldest), "%s", base);
            }
            e.close();
        }
        d.close();
        if (oldest[0] == '\0') break;
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", dir, oldest);
        if (!SD.remove(path)) break;   // cannot make progress; stop rather than spin
        kept--;
    }
}

void detect_log_sweep_all()
{
    if (!sd_usable()) return;
    for (int i = 0; i < kAppendLogCount; i++) detect_log_enforce(kAppendLogs[i]);
    detect_log_prune_dir(kFlockDir);
}

int detect_log_clear_all()
{
    if (!sd_usable()) return 0;
    int removed = 0;

    for (int i = 0; i < kAppendLogCount; i++) {
        if (SD.exists(kAppendLogs[i]) && SD.remove(kAppendLogs[i])) removed++;
    }

    if (SD.exists(kFlockDir)) {
        // Collect-and-delete one at a time: openNextFile while removing under
        // the same handle is not safe.
        bool more = true;
        while (more) {
            more = false;
            char victim[96];
            victim[0] = '\0';
            File d = SD.open(kFlockDir);
            if (!d) break;
            for (File e = d.openNextFile(); e; e = d.openNextFile()) {
                if (!e.isDirectory()) {
                    snprintf(victim, sizeof(victim), "%s", basename_of(e.name()));
                    e.close();
                    break;
                }
                e.close();
            }
            d.close();
            if (victim[0]) {
                char path[96];
                snprintf(path, sizeof(path), "%s/%s", kFlockDir, victim);
                if (SD.remove(path)) { removed++; more = true; }
            }
        }
    }
    return removed;
}

int detect_log_total_records()
{
    if (!sd_usable()) return 0;
    int total = 0;

    for (int i = 0; i < kAppendLogCount; i++) {
        if (!SD.exists(kAppendLogs[i])) continue;
        File f = SD.open(kAppendLogs[i], FILE_READ);
        if (!f) continue;
        char line[kLineMax];
        bool trunc;
        long stamp;
        while (read_line(f, line, sizeof(line), &trunc))
            if (detlog::parse_stamp(line, &stamp)) total++;
        f.close();
    }

    if (SD.exists(kFlockDir)) {
        File d = SD.open(kFlockDir);
        if (d) {
            for (File e = d.openNextFile(); e; e = d.openNextFile()) {
                if (!e.isDirectory()) total++;
                e.close();
            }
            d.close();
        }
    }
    return total;
}
