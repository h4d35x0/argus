// test_ble_spam.cpp - host unit tests for the pure BLE advertisement-spam /
// flood classifier (src/detect/ble_spam). DEFENSIVE detection only: these
// exercise the distinct-advertiser sliding-window escalation (None -> Elevated
// -> Spam) driven by the spam-pattern gate, dedup of one address re-advertising,
// the inert non-watch-listed path, window aging via tick(), graceful table-full
// saturation, and reset. Time is a plain input on every observation - no clock,
// no radio, no hardware. The advert byte buffers reuse the AD-structure patterns
// from test_ble_adv.cpp and are parsed through the shared adv_parser.
#include "wl_test.h"
#include "ble_spam.h"

#include <cstdint>
#include <cstring>

using namespace detect;

// ---- real-ish advert byte buffers -------------------------------------------

// Apple manufacturer advert (company 0x004C) - the Continuity / Nearby family
// that "fake pairing popup" BLE-spam tools spoof. flags + a 9-byte Apple blob.
static const uint8_t kAppleAdv[] = {
    0x02, 0x01, 0x1A,
    0x0A, 0xFF, 0x4C, 0x00, 0x10, 0x07, 0x38, 0x1F, 0x8B, 0x40, 0x62,
};

// Google Fast Pair advert: 16-bit service data under UUID 0xFE2C (2C FE on air),
// then a 3-byte model-id-ish payload. This is the Fast Pair spam signature.
static const uint8_t kFastPairAdv[] = {
    0x02, 0x01, 0x06,
    0x06, 0x16, 0x2C, 0xFE, 0xAA, 0xBB, 0xCC,
};

// A benign, NON-watch-listed manufacturer advert (company 0x0087, Garmin). Valid
// manufacturer record, but not a vendor BLE-spam abuses -> must be inert.
static const uint8_t kBenignMfrAdv[] = {
    0x02, 0x01, 0x06,
    0x05, 0xFF, 0x87, 0x00, 0x11, 0x22,
};

// A plain name-only advert: no manufacturer data, no Fast Pair -> inert.
static const uint8_t kNameAdv[] = {
    0x06, 0x09, 'W', 'a', 't', 'c', 'h',
};

// Build an observation whose address is derived from a 16-bit index, so tests
// can spin up many DISTINCT randomized advertisers cheaply. addr is locally-
// administered (0x02 prefix) to stay obviously synthetic.
static BleAdvObservation mk(uint16_t idx, const uint8_t* adv, uint8_t adv_len,
                            uint32_t t_sec, int8_t rssi = -50) {
  BleAdvObservation o;
  o.addr[0] = 0x02;
  o.addr[1] = 0x00;
  o.addr[2] = 0x00;
  o.addr[3] = 0x00;
  o.addr[4] = (uint8_t)(idx >> 8);
  o.addr[5] = (uint8_t)(idx & 0xFF);
  o.adv_data = adv;
  o.adv_len  = adv_len;
  o.t_sec    = t_sec;
  o.rssi     = rssi;
  return o;
}

// ---- A handful of ordinary nearby devices stays None. -----------------------
WL_TEST(ble_spam_few_normal_devices_none) {
  BleSpamDetector d;
  SpamVerdict v;
  // 5 distinct Apple/Fast-Pair devices around you (< kElevatedDistinct 6).
  for (uint16_t i = 0; i < 5; ++i) {
    const uint8_t* adv = (i & 1) ? kFastPairAdv : kAppleAdv;
    uint8_t len = (i & 1) ? (uint8_t)sizeof(kFastPairAdv) : (uint8_t)sizeof(kAppleAdv);
    v = d.ingest(mk(i, adv, len, 100));
    WL_CHECK(v.flag == SpamFlag::None);
  }
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)5);
  WL_CHECK_EQ(d.tracked(), (size_t)5);
  WL_CHECK(d.flag() == SpamFlag::None);
}

// ---- A flood of distinct spam-pattern advertisers climbs None -> Elevated ->
//      Spam. -------------------------------------------------------------------
WL_TEST(ble_spam_flood_escalates_to_spam) {
  BleSpamDetector d;
  SpamVerdict v;

  // First 5 distinct: below the Elevated gate (6) -> still None.
  for (uint16_t i = 0; i < 5; ++i)
    v = d.ingest(mk(i, kAppleAdv, sizeof(kAppleAdv), 200));
  WL_CHECK(v.flag == SpamFlag::None);

  // 6th distinct reaches Elevated.
  v = d.ingest(mk(5, kAppleAdv, sizeof(kAppleAdv), 200));
  WL_CHECK(v.flag == SpamFlag::Elevated);
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)6);

  // Distinct 7..15: Elevated but not yet Spam.
  for (uint16_t i = 6; i < 15; ++i)
    v = d.ingest(mk(i, kFastPairAdv, sizeof(kFastPairAdv), 200));
  WL_CHECK(v.flag == SpamFlag::Elevated);

  // 16th distinct reaches the Spam gate (kSpamDistinct).
  v = d.ingest(mk(15, kAppleAdv, sizeof(kAppleAdv), 200));
  WL_CHECK(v.flag == SpamFlag::Spam);
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)16);
}

// ---- A benign flood of NON-watch-listed advertisers never trips the gate. ---
WL_TEST(ble_spam_non_watchlisted_flood_inert) {
  BleSpamDetector d;
  // 40 distinct Garmin devices AND 40 distinct name-only devices, all at once.
  for (uint16_t i = 0; i < 40; ++i) {
    SpamVerdict v = d.ingest(mk(i, kBenignMfrAdv, sizeof(kBenignMfrAdv), 300));
    WL_CHECK(v.flag == SpamFlag::None);
    v = d.ingest(mk((uint16_t)(1000 + i), kNameAdv, sizeof(kNameAdv), 300));
    WL_CHECK(v.flag == SpamFlag::None);
  }
  WL_CHECK_EQ(d.tracked(), (size_t)0);        // nothing ever entered the table
  WL_CHECK(d.flag() == SpamFlag::None);
}

// ---- The same address re-advertising is deduped: one distinct device, never a
//      flood no matter how many adverts it sends. ------------------------------
WL_TEST(ble_spam_same_address_deduped) {
  BleSpamDetector d;
  SpamVerdict v;
  for (int i = 0; i < 100; ++i)
    v = d.ingest(mk(7, kAppleAdv, sizeof(kAppleAdv), 400));
  WL_CHECK(v.flag == SpamFlag::None);
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)1);
  WL_CHECK_EQ(d.tracked(), (size_t)1);
}

// ---- tick() ages a stopped flood back to None and frees the slots. ----------
WL_TEST(ble_spam_tick_relaxes_stopped_flood) {
  BleSpamDetector d;
  SpamVerdict v;
  for (uint16_t i = 0; i < 20; ++i)
    v = d.ingest(mk(i, kAppleAdv, sizeof(kAppleAdv), 500));
  WL_CHECK(v.flag == SpamFlag::Spam);
  WL_CHECK_EQ(d.tracked(), (size_t)20);

  // Still inside the window (age 4 < kWindowSec 5) -> flood still visible.
  d.tick(504);
  WL_CHECK(d.flag() == SpamFlag::Spam);
  WL_CHECK_EQ(d.tracked(), (size_t)20);

  // Advance past the window (age 5 == kWindowSec) -> everything drops out.
  d.tick(505);
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  WL_CHECK(d.flag() == SpamFlag::None);
  WL_CHECK_EQ(d.distinct(), (uint16_t)0);
}

// ---- A stale address is re-usable: after it ages out, feeding it again counts
//      as one fresh distinct device (window is not latched). -------------------
WL_TEST(ble_spam_window_not_latched) {
  BleSpamDetector d;
  // Fill to Elevated, then let it all age out.
  for (uint16_t i = 0; i < 8; ++i) d.ingest(mk(i, kAppleAdv, sizeof(kAppleAdv), 600));
  WL_CHECK(d.flag() == SpamFlag::Elevated);
  d.tick(610);
  WL_CHECK(d.flag() == SpamFlag::None);
  // A single spam advert later is just one device -> None again.
  SpamVerdict v = d.ingest(mk(0, kAppleAdv, sizeof(kAppleAdv), 620));
  WL_CHECK(v.flag == SpamFlag::None);
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)1);
}

// ---- Table full: many more distinct advertisers than the cap must saturate,
//      stay flagged Spam, and never crash or exceed the cap. -------------------
WL_TEST(ble_spam_table_full_saturates) {
  BleSpamDetector d;
  SpamVerdict v;
  // 200 distinct spam-pattern advertisers in one second: far past kMaxAddrs (64).
  for (uint16_t i = 0; i < 200; ++i)
    v = d.ingest(mk(i, kAppleAdv, sizeof(kAppleAdv), 700));
  WL_CHECK(v.flag == SpamFlag::Spam);
  // Distinct count saturates at the cap; it never exceeds kMaxAddrs.
  WL_CHECK_EQ(d.tracked(), (size_t)BleSpamDetector::kMaxAddrs);
  WL_CHECK(d.distinct() <= (uint16_t)BleSpamDetector::kMaxAddrs);
  WL_CHECK(d.flag() == SpamFlag::Spam);
}

// ---- reset() forgets everything; a fresh flood starts from baseline again. ---
WL_TEST(ble_spam_reset_clears) {
  BleSpamDetector d;
  for (uint16_t i = 0; i < 20; ++i) d.ingest(mk(i, kAppleAdv, sizeof(kAppleAdv), 800));
  WL_CHECK(d.tracked() >= (size_t)1);
  d.reset();
  WL_CHECK_EQ(d.tracked(), (size_t)0);
  WL_CHECK(d.flag() == SpamFlag::None);
  SpamVerdict v = d.ingest(mk(0, kAppleAdv, sizeof(kAppleAdv), 900));
  WL_CHECK(v.flag == SpamFlag::None);
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)1);
}

// ---- A Fast-Pair-only flood (no manufacturer data) trips the gate too, proving
//      the service-data path is wired independently of the manufacturer path. --
WL_TEST(ble_spam_fastpair_only_path) {
  BleSpamDetector d;
  SpamVerdict v;
  for (uint16_t i = 0; i < 16; ++i)
    v = d.ingest(mk(i, kFastPairAdv, sizeof(kFastPairAdv), 1000));
  WL_CHECK(v.flag == SpamFlag::Spam);
  WL_CHECK_EQ(v.distinct_addrs_per_win, (uint16_t)16);
}
