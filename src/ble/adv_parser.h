// adv_parser.h — pure, host-testable BLE advertising-data parser.
//
// The AirTag / Flipper / card-skimmer detectors each re-walk the raw BLE
// advertising payload by hand (see airtag.cpp, flipper.cpp, skimmer.cpp).
// That duplicated, hand-rolled byte-walking is exactly where an over-read or a
// truncated-record bug hides. This module extracts ONE correct, bounds-checked
// parser for the standard BLE "AD structure" TLV format so those detectors can
// eventually share it instead of each re-parsing raw bytes.
//
// Design constraints (deliberate):
//   * Pure C++11/14. NO Arduino.h, NO ESP-IDF, NO LVGL, NO hardware. Compiles
//     and runs on the host g++ so the parsing logic is unit-tested in ms.
//   * No dynamic allocation. Every result is a bounds-checked VIEW (pointer +
//     length) into the caller's own buffer; the caller owns the memory.
//   * Const-correct. The input buffer is never modified.
//
// The BLE advertising payload is a sequence of AD structures, each:
//     [ length (1 byte) ][ AD type (1 byte) ][ value (length-1 bytes) ]
// where the length byte counts the type byte plus the value bytes. A length of
// 0 marks the end (zero padding). This parser rejects any record whose declared
// length would read past the end of the buffer.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ble {

// Standard BLE AD (advertising data) type codes. Values are from the Bluetooth
// "Assigned Numbers" - Generic Access Profile. Only the ones the detectors care
// about (plus the common service-data / UUID types) are named here.
enum AdType : uint8_t {
    AD_FLAGS               = 0x01,  // flags byte(s)
    AD_INCOMPLETE_UUID16   = 0x02,  // incomplete list of 16-bit service UUIDs
    AD_COMPLETE_UUID16     = 0x03,  // complete   list of 16-bit service UUIDs
    AD_INCOMPLETE_UUID32   = 0x04,  // incomplete list of 32-bit service UUIDs
    AD_COMPLETE_UUID32     = 0x05,  // complete   list of 32-bit service UUIDs
    AD_INCOMPLETE_UUID128  = 0x06,  // incomplete list of 128-bit service UUIDs
    AD_COMPLETE_UUID128    = 0x07,  // complete   list of 128-bit service UUIDs
    AD_SHORT_NAME          = 0x08,  // shortened local name
    AD_COMPLETE_NAME       = 0x09,  // complete  local name
    AD_TX_POWER            = 0x0A,  // TX power level
    AD_SERVICE_DATA_16     = 0x16,  // service data, 16-bit UUID
    AD_SERVICE_DATA_32     = 0x20,  // service data, 32-bit UUID
    AD_SERVICE_DATA_128    = 0x21,  // service data, 128-bit UUID
    AD_MANUFACTURER        = 0xFF,  // manufacturer-specific data
};

// Apple's Bluetooth company identifier (little-endian on the air as 4C 00).
static const uint16_t APPLE_COMPANY_ID = 0x004C;

// Apple Find My / "offline finding" manufacturer sub-type. This is the first
// payload byte AFTER the 2-byte company id in an Apple manufacturer record.
// Confirmed against airtag.cpp (fm_type == 0x12).
static const uint8_t APPLE_FINDMY_TYPE = 0x12;

// One parsed AD structure. `data`/`len` are a VIEW into the caller's buffer:
// `data` may be null when `len` is 0 (a type-only record, i.e. length byte 1).
// Never dereference `data` for more than `len` bytes.
struct AdStructure {
    uint8_t        type;  // AD type code (see AdType)
    const uint8_t* data;  // value bytes (points into the caller buffer)
    size_t         len;   // number of value bytes
};

// Safe forward iterator over the AD structures in [buf, buf+len).
//
// Usage:
//     size_t pos = 0;
//     ble::AdStructure ad;
//     while (ble::adv_next(buf, len, &pos, &ad)) {
//         // inspect ad.type / ad.data / ad.len
//     }
//
// Returns true and fills *out when a well-formed record was read, advancing
// *pos past it. Returns false at the clean end of the payload (pos exhausted or
// a zero length byte) AND on any malformed record (a length byte that would run
// past the buffer). It never reads past buf+len. On the false return *out is
// left unspecified; a malformed record simply stops iteration, matching the
// hand-rolled `break` the detectors use today.
bool adv_next(const uint8_t* buf, size_t len, size_t* pos, AdStructure* out);

// Count the well-formed AD structures in the payload. Stops at the first
// malformed record (does not count it). Tolerates a null/zero buffer (returns
// 0).
size_t adv_count(const uint8_t* buf, size_t len);

// Find the first AD structure of `type`. Returns true and fills *out on a hit.
bool adv_find(const uint8_t* buf, size_t len, uint8_t type, AdStructure* out);

// Extract the advertised local name into `out` (always null-terminated when
// out_sz > 0). Prefers the complete name (0x09) over the shortened name (0x08).
// Returns true if a name AD with at least one value byte was found. Copies at
// most out_sz-1 bytes; the name is NOT guaranteed to be a C string on the air,
// so callers wanting exact bytes should use adv_find directly.
bool adv_local_name(const uint8_t* buf, size_t len, char* out, size_t out_sz);

// Extract the flags byte (AD type 0x01). Returns true and sets *out_flags when
// a flags record with at least one byte is present.
bool adv_flags(const uint8_t* buf, size_t len, uint8_t* out_flags);

// Extract manufacturer-specific data (AD type 0xFF). A valid manufacturer
// record carries at least the 2-byte little-endian company id. On success sets
// *company_id, and (*payload,*payload_len) to the bytes AFTER the company id
// (payload may be null with payload_len 0 when the record is exactly the 2
// company-id bytes). Any of the out pointers may be null if unwanted.
bool adv_manufacturer(const uint8_t* buf, size_t len,
                      uint16_t* company_id,
                      const uint8_t** payload, size_t* payload_len);

// Convenience: the manufacturer company id, or false if there is no valid
// manufacturer record (one holding at least the 2 company-id bytes).
bool adv_manufacturer_company_id(const uint8_t* buf, size_t len,
                                 uint16_t* company_id);

// True if the payload carries the 16-bit service `uuid` in either the complete
// (0x03) or incomplete (0x02) 16-bit service UUID list. Malformed / odd-length
// UUID lists are walked safely (a trailing odd byte is ignored). This is what
// the Flipper detector keys off of (service UUID 0x3082).
bool adv_has_service_uuid16(const uint8_t* buf, size_t len, uint16_t uuid);

// Find 16-bit service data (AD type 0x16) whose leading 16-bit UUID matches
// `uuid`. On success sets (*data,*data_len) to the bytes AFTER the UUID (may be
// null with 0 length). The out pointers may be null if unwanted.
bool adv_find_service_data16(const uint8_t* buf, size_t len, uint16_t uuid,
                             const uint8_t** data, size_t* data_len);

// True if this is an Apple Find My / offline-finding advert: an Apple (0x004C)
// manufacturer record whose first post-company-id byte is the Find My type
// (0x12). This is the LOOSE Find My check (matches iPhones/AirPods/AirTags/
// third-party Find My accessories alike). The AirTag detector deliberately adds
// a further length filter (payload[1] == 0x19 and a >= 29-byte record) on top;
// that stricter AirTag-specific policy is left to the caller, not baked in here.
bool adv_is_apple_findmy(const uint8_t* buf, size_t len);

}  // namespace ble
