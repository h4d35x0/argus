// adv_parser.cpp — implementation of the pure BLE AD-structure parser.
// See adv_parser.h for the rationale and the API contract. No hardware deps.
#include "adv_parser.h"

#include <cstring>

namespace ble {

bool adv_next(const uint8_t* buf, size_t len, size_t* pos, AdStructure* out)
{
    if (!buf || !pos || !out) return false;
    size_t p = *pos;

    // Clean end: nothing left to read.
    if (p >= len) return false;

    // The length byte counts the AD type plus the value bytes. A zero length
    // is the end-of-data padding, not a record.
    uint8_t seg_len = buf[p];
    if (seg_len == 0) return false;

    // Reject any record that would read past the buffer. This is the whole
    // point of the module: never over-read a truncated / malformed advert.
    // Compare in a width that cannot overflow (size_t vs the <=255 seg_len).
    if (p + 1 + (size_t)seg_len > len) return false;

    out->type = buf[p + 1];
    out->len  = (size_t)seg_len - 1;              // value bytes only
    out->data = (out->len > 0) ? (buf + p + 2)    // view into caller buffer
                               : nullptr;

    *pos = p + 1 + (size_t)seg_len;
    return true;
}

size_t adv_count(const uint8_t* buf, size_t len)
{
    size_t pos = 0, n = 0;
    AdStructure ad;
    while (adv_next(buf, len, &pos, &ad)) n++;
    return n;
}

bool adv_find(const uint8_t* buf, size_t len, uint8_t type, AdStructure* out)
{
    size_t pos = 0;
    AdStructure ad;
    while (adv_next(buf, len, &pos, &ad)) {
        if (ad.type == type) {
            if (out) *out = ad;
            return true;
        }
    }
    return false;
}

bool adv_local_name(const uint8_t* buf, size_t len, char* out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';

    const uint8_t* best_data = nullptr;
    size_t         best_len  = 0;
    bool           have_complete = false;

    size_t pos = 0;
    AdStructure ad;
    while (adv_next(buf, len, &pos, &ad)) {
        if (ad.type != AD_COMPLETE_NAME && ad.type != AD_SHORT_NAME) continue;
        if (ad.len == 0) continue;
        bool is_complete = (ad.type == AD_COMPLETE_NAME);
        // Prefer the first complete name; fall back to a shortened name only
        // if no complete name has been seen.
        if (is_complete && !have_complete) {
            best_data = ad.data;
            best_len  = ad.len;
            have_complete = true;
        } else if (!have_complete && best_data == nullptr) {
            best_data = ad.data;
            best_len  = ad.len;
        }
    }

    if (!best_data) return false;

    size_t copy = best_len;
    if (copy > out_sz - 1) copy = out_sz - 1;
    memcpy(out, best_data, copy);
    out[copy] = '\0';
    return true;
}

bool adv_flags(const uint8_t* buf, size_t len, uint8_t* out_flags)
{
    AdStructure ad;
    if (!adv_find(buf, len, AD_FLAGS, &ad)) return false;
    if (ad.len < 1) return false;
    if (out_flags) *out_flags = ad.data[0];
    return true;
}

bool adv_manufacturer(const uint8_t* buf, size_t len,
                      uint16_t* company_id,
                      const uint8_t** payload, size_t* payload_len)
{
    AdStructure ad;
    if (!adv_find(buf, len, AD_MANUFACTURER, &ad)) return false;
    // A valid manufacturer record carries at least the 2-byte company id.
    if (ad.len < 2) return false;

    if (company_id)
        *company_id = (uint16_t)ad.data[0] | ((uint16_t)ad.data[1] << 8);
    if (payload)
        *payload = (ad.len > 2) ? (ad.data + 2) : nullptr;
    if (payload_len)
        *payload_len = ad.len - 2;
    return true;
}

bool adv_manufacturer_company_id(const uint8_t* buf, size_t len,
                                 uint16_t* company_id)
{
    return adv_manufacturer(buf, len, company_id, nullptr, nullptr);
}

bool adv_has_service_uuid16(const uint8_t* buf, size_t len, uint16_t uuid)
{
    size_t pos = 0;
    AdStructure ad;
    while (adv_next(buf, len, &pos, &ad)) {
        if (ad.type != AD_INCOMPLETE_UUID16 && ad.type != AD_COMPLETE_UUID16)
            continue;
        // Walk the list two bytes at a time; a trailing odd byte is ignored.
        for (size_t i = 0; i + 1 < ad.len; i += 2) {
            uint16_t u = (uint16_t)ad.data[i] | ((uint16_t)ad.data[i + 1] << 8);
            if (u == uuid) return true;
        }
    }
    return false;
}

bool adv_find_service_data16(const uint8_t* buf, size_t len, uint16_t uuid,
                             const uint8_t** data, size_t* data_len)
{
    size_t pos = 0;
    AdStructure ad;
    while (adv_next(buf, len, &pos, &ad)) {
        if (ad.type != AD_SERVICE_DATA_16) continue;
        // 16-bit service data leads with the 2-byte UUID, then the data bytes.
        if (ad.len < 2) continue;
        uint16_t u = (uint16_t)ad.data[0] | ((uint16_t)ad.data[1] << 8);
        if (u != uuid) continue;
        if (data)     *data     = (ad.len > 2) ? (ad.data + 2) : nullptr;
        if (data_len) *data_len = ad.len - 2;
        return true;
    }
    return false;
}

bool adv_is_apple_findmy(const uint8_t* buf, size_t len)
{
    uint16_t       company = 0;
    const uint8_t* payload = nullptr;
    size_t         plen    = 0;
    if (!adv_manufacturer(buf, len, &company, &payload, &plen)) return false;
    if (company != APPLE_COMPANY_ID) return false;
    // Need at least the Find My type byte after the company id.
    if (!payload || plen < 1) return false;
    return payload[0] == APPLE_FINDMY_TYPE;
}

}  // namespace ble
