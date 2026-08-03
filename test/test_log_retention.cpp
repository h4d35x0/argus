// test_log_retention.cpp - retention policy for the on-SD detection logs.
//
// These logs hold other people's device identifiers and roughly where they
// stood. The failure mode that matters is NOT "kept slightly too long" - it is
// silently deleting a wearer's evidence, or silently keeping a stranger's
// record forever. Both directions are asserted here.

#include "wl_test.h"
#include "log_retention.h"

using namespace detlog;

// 2026-08-03 12:00:00 in the local frame the detectors stamp with.
static long kNow = days_from_civil(2026, 8, 3) * kSecondsPerDay + 12 * 3600L;

static long at(int y, unsigned m, unsigned d, int hh = 0, int mm = 0, int ss = 0)
{
    return days_from_civil(y, m, d) * kSecondsPerDay + hh * 3600L + mm * 60L + ss;
}

WL_TEST(civil_days_anchors)
{
    WL_CHECK(days_from_civil(1970, 1, 1) == 0);
    WL_CHECK(days_from_civil(1969, 12, 31) == -1);
    WL_CHECK(days_from_civil(1970, 1, 2) == 1);
    WL_CHECK(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28) == 2);  // leap
    WL_CHECK(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 28) == 1);  // not leap
    WL_CHECK(days_from_civil(2026, 1, 1) - days_from_civil(2025, 1, 1) == 365);
}

WL_TEST(parse_stamp_accepts_real_detector_lines)
{
    // Exactly the shape every detector's f.printf emits.
    long s = 0;
    WL_CHECK(parse_stamp("2026-08-03 09:24:17\tMAC AA:BB:CC:DD:EE:FF\tRSSI -60", &s));
    WL_CHECK(s == at(2026, 8, 3, 9, 24, 17));

    WL_CHECK(parse_stamp("2026-01-01 00:00:00", &s));
    WL_CHECK(s == at(2026, 1, 1));
}

WL_TEST(parse_stamp_rejects_non_records)
{
    long s = 0;
    WL_CHECK(!parse_stamp("MAC:    AA:BB:CC:DD:EE:FF", &s));   // Flock body line
    WL_CHECK(!parse_stamp("Vendor: Example", &s));
    WL_CHECK(!parse_stamp("", &s));
    WL_CHECK(!parse_stamp("2026-08-03", &s));                  // date only
    WL_CHECK(!parse_stamp("2026/08/03 09:24:17", &s));         // wrong separators
    WL_CHECK(!parse_stamp("20260803 09:24:17", &s));
    WL_CHECK(!parse_stamp("not a timestamp at all", &s));
    WL_CHECK(!parse_stamp(nullptr, &s));
    // Out-of-range fields must not become bogus epochs.
    WL_CHECK(!parse_stamp("2026-13-03 09:24:17", &s));
    WL_CHECK(!parse_stamp("2026-08-03 25:24:17", &s));
}

WL_TEST(expiry_window)
{
    WL_CHECK(!is_expired(kNow, kNow, kMaxAgeSeconds));
    WL_CHECK(!is_expired(kNow - kMaxAgeSeconds + 1, kNow, kMaxAgeSeconds));
    WL_CHECK( is_expired(kNow - kMaxAgeSeconds - 1, kNow, kMaxAgeSeconds));
    // Exactly at the boundary is still inside the window.
    WL_CHECK(!is_expired(kNow - kMaxAgeSeconds, kNow, kMaxAgeSeconds));
}

WL_TEST(future_stamp_is_never_deleted)
{
    // If the RTC was wrong when the record was written, or is wrong now, the
    // safe failure is to KEEP the wearer's evidence, not to silently destroy it.
    WL_CHECK(!is_expired(kNow + 86400L * 365, kNow, kMaxAgeSeconds));
    WL_CHECK(!is_expired(kNow + 1, kNow, kMaxAgeSeconds));
}

WL_TEST(retention_disabled_keeps_everything)
{
    WL_CHECK(!is_expired(kNow - 86400L * 10000, kNow, 0));
    WL_CHECK(!is_expired(kNow - 86400L * 10000, kNow, -1));
}

WL_TEST(first_kept_empty_and_clean_logs)
{
    WL_CHECK(first_kept(0, 0, kMaxEntries) == 0);
    // Nothing expired, under the cap: keep everything.
    WL_CHECK(first_kept(10, 0, kMaxEntries) == 0);
}

WL_TEST(first_kept_age_only)
{
    // 40 records, the first 12 are outside the window.
    WL_CHECK(first_kept(40, 12, kMaxEntries) == 12);
    // All expired.
    WL_CHECK(first_kept(40, 40, kMaxEntries) == 40);
}

WL_TEST(first_kept_cap_only)
{
    // Nothing expired, but well over the cap: evict oldest-first.
    int n = kMaxEntries + 25;
    WL_CHECK(first_kept(n, 0, kMaxEntries) == 25);
    // Exactly at the cap: nothing dropped.
    WL_CHECK(first_kept(kMaxEntries, 0, kMaxEntries) == 0);
}

WL_TEST(first_kept_age_and_cap_take_the_stricter)
{
    // Age would drop 5, cap would drop 30. The cap wins.
    int n = kMaxEntries + 30;
    WL_CHECK(first_kept(n, 5, kMaxEntries) == 30);
    // Age would drop 100, cap would drop 30. Age wins.
    WL_CHECK(first_kept(n, 100, kMaxEntries) == 100);
}

WL_TEST(first_kept_cap_disabled)
{
    WL_CHECK(first_kept(1000, 3, 0)  == 3);
    WL_CHECK(first_kept(1000, 3, -1) == 3);
}

WL_TEST(first_kept_clamps_bad_input)
{
    // A caller that miscounts must never produce an out-of-range index, or the
    // SD-side copy loop would drop the whole file.
    WL_CHECK(first_kept(10, -5, 0)  == 0);
    WL_CHECK(first_kept(10, 99, 0)  == 10);
    WL_CHECK(first_kept(-1, 0, 100) == 0);
}

WL_TEST(retention_only_ever_drops_a_prefix)
{
    // The SD compaction is a single streaming copy, which is only correct if
    // retention can never punch a hole in the middle of the file.
    for (int n = 0; n < 40; n++)
        for (int fu = 0; fu <= n; fu++) {
            int k = first_kept(n, fu, 7);
            WL_CHECK(k >= 0 && k <= n);
            WL_CHECK(k >= fu || fu > n);   // never keeps an expired record
            WL_CHECK((n - k) <= 7 || n == 0);
        }
}

WL_TEST(thirty_day_policy_end_to_end)
{
    // A record from 31 days ago goes; 29 days ago stays.
    long old29 = at(2026, 7, 5, 12);    // 29 days before kNow
    long old31 = at(2026, 7, 3, 11);    // >30 days before kNow
    WL_CHECK(!is_expired(old29, kNow, kMaxAgeSeconds));
    WL_CHECK( is_expired(old31, kNow, kMaxAgeSeconds));
}
