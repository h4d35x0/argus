// test_ble_adv.cpp — host unit tests for the pure BLE AD-structure parser
// (src/ble/adv_parser). These exercise the exact byte checks the AirTag,
// Flipper and card-skimmer detectors rely on, PLUS the malformed / truncated
// buffers the parser exists to survive without over-reading.
//
// Byte-level expectations are cross-checked against the real detectors:
//   * Apple Find My:  airtag.cpp  (company 0x004C, fm_type 0x12, len 0x19)
//   * Flipper:        flipper.cpp (name prefix "Flipper ", service UUID 0x3082)
//   * Skimmer:        skimmer.cpp (name prefix "HC-05")
#include "wl_test.h"
#include "adv_parser.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace ble;

// A well-formed multi-AD packet: flags, complete local name "Watch",
// a 16-bit service UUID list containing 0x180F, and an Apple manufacturer blob.
//   02 01 06                      flags = LE General + BR/EDR NotSupported
//   06 09 57 61 74 63 68          complete name "Watch"
//   03 03 0F 18                   complete 16-bit UUID list: 0x180F
//   05 FF 4C 00 12 34             manufacturer: Apple, payload 12 34
static const uint8_t kMultiAd[] = {
    0x02, 0x01, 0x06,
    0x06, 0x09, 'W', 'a', 't', 'c', 'h',
    0x03, 0x03, 0x0F, 0x18,
    0x05, 0xFF, 0x4C, 0x00, 0x12, 0x34,
};

WL_TEST(adv_iterates_all_records) {
    WL_CHECK_EQ(adv_count(kMultiAd, sizeof(kMultiAd)), (size_t)4);

    // Manually walk and confirm each record's type + length view.
    size_t pos = 0;
    AdStructure ad;
    WL_CHECK(adv_next(kMultiAd, sizeof(kMultiAd), &pos, &ad));
    WL_CHECK_EQ(ad.type, (uint8_t)AD_FLAGS);
    WL_CHECK_EQ(ad.len, (size_t)1);
    WL_CHECK_EQ(ad.data[0], (uint8_t)0x06);

    WL_CHECK(adv_next(kMultiAd, sizeof(kMultiAd), &pos, &ad));
    WL_CHECK_EQ(ad.type, (uint8_t)AD_COMPLETE_NAME);
    WL_CHECK_EQ(ad.len, (size_t)5);

    WL_CHECK(adv_next(kMultiAd, sizeof(kMultiAd), &pos, &ad));
    WL_CHECK_EQ(ad.type, (uint8_t)AD_COMPLETE_UUID16);

    WL_CHECK(adv_next(kMultiAd, sizeof(kMultiAd), &pos, &ad));
    WL_CHECK_EQ(ad.type, (uint8_t)AD_MANUFACTURER);

    // No more records.
    WL_CHECK(!adv_next(kMultiAd, sizeof(kMultiAd), &pos, &ad));
}

WL_TEST(adv_helpers_on_multi_ad) {
    uint8_t flags = 0;
    WL_CHECK(adv_flags(kMultiAd, sizeof(kMultiAd), &flags));
    WL_CHECK_EQ(flags, (uint8_t)0x06);

    char name[16] = {0};
    WL_CHECK(adv_local_name(kMultiAd, sizeof(kMultiAd), name, sizeof(name)));
    WL_CHECK(strcmp(name, "Watch") == 0);

    WL_CHECK(adv_has_service_uuid16(kMultiAd, sizeof(kMultiAd), 0x180F));
    WL_CHECK(!adv_has_service_uuid16(kMultiAd, sizeof(kMultiAd), 0x1234));

    uint16_t company = 0;
    const uint8_t* payload = nullptr;
    size_t plen = 0;
    WL_CHECK(adv_manufacturer(kMultiAd, sizeof(kMultiAd), &company, &payload, &plen));
    WL_CHECK_EQ(company, (uint16_t)APPLE_COMPANY_ID);
    WL_CHECK_EQ(plen, (size_t)2);
    WL_CHECK(payload != nullptr);
    WL_CHECK_EQ(payload[0], (uint8_t)0x12);
    WL_CHECK_EQ(payload[1], (uint8_t)0x34);

    uint16_t cid = 0;
    WL_CHECK(adv_manufacturer_company_id(kMultiAd, sizeof(kMultiAd), &cid));
    WL_CHECK_EQ(cid, (uint16_t)0x004C);
}

// A real-ish Apple Find My / AirTag "separated" manufacturer advert.
//   1E FF 4C 00 12 19 <status> <22-byte key> <key-byte0> <hint>
// seg_len 0x1E = 30 -> 29 value bytes: matches airtag.cpp kMinFindMyRecord.
//   value[0..1] = 4C 00 (Apple), value[2] = 0x12 (fm_type), value[3] = 0x19.
static std::vector<uint8_t> make_airtag_adv() {
    std::vector<uint8_t> v;
    v.push_back(0x1E);        // length: 30 (type + 29 value bytes)
    v.push_back(0xFF);        // AD type: manufacturer
    v.push_back(0x4C);        // company LE lo (Apple 0x004C)
    v.push_back(0x00);        // company LE hi
    v.push_back(0x12);        // fm_type: Find My offline finding
    v.push_back(0x19);        // payload length byte (25) = AirTag lost-mode
    v.push_back(0xA0);        // status byte
    for (int i = 0; i < 22; i++) v.push_back((uint8_t)(0x10 + i));  // EC key
    v.push_back(0x3F);        // public-key byte 0 (upper bits)
    v.push_back(0x00);        // hint byte
    return v;
}

WL_TEST(adv_apple_findmy_airtag) {
    auto v = make_airtag_adv();
    // value bytes = seg_len - 1 = 29, matching airtag.cpp kMinFindMyRecord.
    WL_CHECK_EQ(v.size(), (size_t)31);

    WL_CHECK(adv_is_apple_findmy(v.data(), v.size()));

    uint16_t company = 0;
    const uint8_t* payload = nullptr;
    size_t plen = 0;
    WL_CHECK(adv_manufacturer(v.data(), v.size(), &company, &payload, &plen));
    WL_CHECK_EQ(company, (uint16_t)0x004C);
    // payload is everything after the 2-byte company id: 27 bytes.
    WL_CHECK_EQ(plen, (size_t)27);
    // Reproduce the AirTag detector's stricter filter on top of the loose
    // Find My predicate: fm_type 0x12 and payload-length byte 0x19.
    WL_CHECK_EQ(payload[0], (uint8_t)APPLE_FINDMY_TYPE);  // 0x12
    WL_CHECK_EQ(payload[1], (uint8_t)0x19);
}

WL_TEST(adv_non_apple_manufacturer_is_not_findmy) {
    // Microsoft (0x0006) manufacturer beacon: valid record, not Find My.
    static const uint8_t ms[] = { 0x05, 0xFF, 0x06, 0x00, 0x01, 0x02 };
    WL_CHECK(!adv_is_apple_findmy(ms, sizeof(ms)));
    uint16_t cid = 0;
    WL_CHECK(adv_manufacturer_company_id(ms, sizeof(ms), &cid));
    WL_CHECK_EQ(cid, (uint16_t)0x0006);

    // Apple company id but the wrong sub-type byte -> not Find My.
    static const uint8_t apple_other[] = { 0x05, 0xFF, 0x4C, 0x00, 0x10, 0x02 };
    WL_CHECK(!adv_is_apple_findmy(apple_other, sizeof(apple_other)));
}

WL_TEST(adv_flipper_name_and_service) {
    // Stock Flipper: complete local name "Flipper Wasp".
    static const uint8_t flip_name[] = {
        0x0D, 0x09, 'F', 'l', 'i', 'p', 'p', 'e', 'r', ' ', 'W', 'a', 's', 'p'
    };
    char name[34];  // Flipper detector caps the name at 33 chars + null
    WL_CHECK(adv_local_name(flip_name, sizeof(flip_name), name, sizeof(name)));
    WL_CHECK(strncmp(name, "Flipper ", 8) == 0);

    // Custom firmware (randomized name) still advertises service UUID 0x3082.
    //   02 01 06                 flags
    //   03 03 82 30              complete 16-bit UUID list: 0x3082
    //   05 09 57 61 6E 6B        name "Wank"
    static const uint8_t flip_uuid[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0x82, 0x30,
        0x05, 0x09, 'W', 'a', 'n', 'k',
    };
    WL_CHECK(adv_has_service_uuid16(flip_uuid, sizeof(flip_uuid), 0x3082));
    WL_CHECK(!adv_has_service_uuid16(flip_uuid, sizeof(flip_uuid), 0x180F));
}

WL_TEST(adv_skimmer_hc05_name) {
    // Card-skimmer HC-05 module: complete local name "HC-05".
    static const uint8_t hc[] = { 0x06, 0x09, 'H', 'C', '-', '0', '5' };
    char name[16];
    WL_CHECK(adv_local_name(hc, sizeof(hc), name, sizeof(name)));
    WL_CHECK(strncmp(name, "HC-05", 5) == 0);
}

WL_TEST(adv_prefers_complete_over_short_name) {
    // Shortened name comes first, complete name second: complete wins.
    static const uint8_t both[] = {
        0x03, 0x08, 'H', 'i',                 // shortened "Hi"
        0x06, 0x09, 'H', 'e', 'l', 'l', 'o',  // complete  "Hello"
    };
    char name[16];
    WL_CHECK(adv_local_name(both, sizeof(both), name, sizeof(name)));
    WL_CHECK(strcmp(name, "Hello") == 0);

    // Only a shortened name present -> that is returned.
    static const uint8_t short_only[] = { 0x03, 0x08, 'H', 'i' };
    WL_CHECK(adv_local_name(short_only, sizeof(short_only), name, sizeof(name)));
    WL_CHECK(strcmp(name, "Hi") == 0);
}

WL_TEST(adv_name_truncated_into_small_buffer) {
    static const uint8_t hc[] = { 0x06, 0x09, 'H', 'C', '-', '0', '5' };
    char name[4];  // room for 3 chars + null
    WL_CHECK(adv_local_name(hc, sizeof(hc), name, sizeof(name)));
    WL_CHECK(strcmp(name, "HC-") == 0);   // copied 3, null-terminated
}

WL_TEST(adv_service_data16) {
    //   05 16 0F 18 AB CD   service data for UUID 0x180F, data AB CD
    static const uint8_t sd[] = { 0x05, 0x16, 0x0F, 0x18, 0xAB, 0xCD };
    const uint8_t* data = nullptr;
    size_t dlen = 0;
    WL_CHECK(adv_find_service_data16(sd, sizeof(sd), 0x180F, &data, &dlen));
    WL_CHECK_EQ(dlen, (size_t)2);
    WL_CHECK(data != nullptr);
    WL_CHECK_EQ(data[0], (uint8_t)0xAB);
    WL_CHECK_EQ(data[1], (uint8_t)0xCD);
    // Wrong UUID -> miss.
    WL_CHECK(!adv_find_service_data16(sd, sizeof(sd), 0x1234, &data, &dlen));
}

// ---- Malformed / boundary buffers: the whole reason this module exists. ----

WL_TEST(adv_zero_length_buffer) {
    static const uint8_t dummy[1] = { 0x00 };
    WL_CHECK_EQ(adv_count(dummy, 0), (size_t)0);
    size_t pos = 0;
    AdStructure ad;
    WL_CHECK(!adv_next(dummy, 0, &pos, &ad));
    // Every helper must report a clean miss, not crash.
    char name[8];
    WL_CHECK(!adv_local_name(dummy, 0, name, sizeof(name)));
    WL_CHECK(!adv_is_apple_findmy(dummy, 0));
    WL_CHECK(!adv_has_service_uuid16(dummy, 0, 0x3082));
}

WL_TEST(adv_null_buffer) {
    WL_CHECK_EQ(adv_count(nullptr, 8), (size_t)0);
    size_t pos = 0;
    AdStructure ad;
    WL_CHECK(!adv_next(nullptr, 8, &pos, &ad));
    WL_CHECK(!adv_is_apple_findmy(nullptr, 8));
}

WL_TEST(adv_zero_length_byte_ends_iteration) {
    // A valid flags record, then a 0x00 length byte = end padding. The trailing
    // garbage after the zero must never be read.
    static const uint8_t buf[] = { 0x02, 0x01, 0x06, 0x00, 0xFF, 0xFF, 0xFF };
    WL_CHECK_EQ(adv_count(buf, sizeof(buf)), (size_t)1);
    uint8_t flags = 0;
    WL_CHECK(adv_flags(buf, sizeof(buf), &flags));
    WL_CHECK_EQ(flags, (uint8_t)0x06);
}

WL_TEST(adv_length_past_end_is_rejected) {
    // Length byte 0x05 claims 5 bytes but only 2 follow. Must be rejected with
    // NO over-read, and the earlier valid record must still be counted.
    static const uint8_t buf[] = { 0x02, 0x01, 0x06, 0x05, 0x09, 'H', 'i' };
    WL_CHECK_EQ(adv_count(buf, sizeof(buf)), (size_t)1);   // only the flags
    // The malformed name record must not be extractable.
    char name[8];
    WL_CHECK(!adv_local_name(buf, sizeof(buf), name, sizeof(name)));
}

WL_TEST(adv_one_past_end_length_exact) {
    // seg_len exactly reaches the last byte -> valid (boundary inclusive).
    static const uint8_t ok[] = { 0x02, 0x01, 0x06 };
    WL_CHECK_EQ(adv_count(ok, sizeof(ok)), (size_t)1);
    // seg_len one larger than the buffer allows -> rejected.
    static const uint8_t bad[] = { 0x03, 0x01, 0x06 };  // claims 3, only 2 left
    WL_CHECK_EQ(adv_count(bad, sizeof(bad)), (size_t)0);
}

WL_TEST(adv_manufacturer_too_short_for_company_id) {
    // Manufacturer record with only ONE value byte -> cannot hold a 2-byte
    // company id, so it is not a valid manufacturer record.
    static const uint8_t buf[] = { 0x02, 0xFF, 0x4C };
    uint16_t cid = 0;
    WL_CHECK(!adv_manufacturer_company_id(buf, sizeof(buf), &cid));
    WL_CHECK(!adv_is_apple_findmy(buf, sizeof(buf)));

    // Exactly the 2 company-id bytes: valid, but no payload for Find My.
    static const uint8_t just_id[] = { 0x03, 0xFF, 0x4C, 0x00 };
    WL_CHECK(adv_manufacturer_company_id(just_id, sizeof(just_id), &cid));
    WL_CHECK_EQ(cid, (uint16_t)0x004C);
    WL_CHECK(!adv_is_apple_findmy(just_id, sizeof(just_id)));  // no fm_type byte
}

WL_TEST(adv_type_only_record_has_null_data) {
    // A record with length byte 1 = type, zero value bytes. data must be null,
    // len 0, and iteration must continue safely to the next record.
    static const uint8_t buf[] = { 0x01, 0x0A, 0x02, 0x01, 0x06 };
    size_t pos = 0;
    AdStructure ad;
    WL_CHECK(adv_next(buf, sizeof(buf), &pos, &ad));
    WL_CHECK_EQ(ad.type, (uint8_t)AD_TX_POWER);
    WL_CHECK_EQ(ad.len, (size_t)0);
    WL_CHECK(ad.data == nullptr);
    WL_CHECK(adv_next(buf, sizeof(buf), &pos, &ad));  // flags record follows
    WL_CHECK_EQ(ad.type, (uint8_t)AD_FLAGS);
}

WL_TEST(adv_odd_length_uuid16_list_no_overread) {
    // UUID16 list with a trailing odd byte: 0x82 0x30 (0x3082) then a lone 0x99.
    // The parser must find 0x3082 and safely ignore the dangling byte.
    static const uint8_t buf[] = { 0x04, 0x03, 0x82, 0x30, 0x99 };
    WL_CHECK(adv_has_service_uuid16(buf, sizeof(buf), 0x3082));
    WL_CHECK(!adv_has_service_uuid16(buf, sizeof(buf), 0x9982));
}
