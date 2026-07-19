// image_dims.cpp — see image_dims.h. Pure header parsing, no allocation, every
// multi-byte read is bounds-checked against `len` before it happens.
#include "image_dims.h"

namespace {

// Big-endian uint32 at buf[off..off+3]. Caller must ensure off+4 <= len.
inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// Big-endian uint16 at buf[off..off+1]. Caller must ensure off+2 <= len.
inline uint32_t be16(const uint8_t* p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

// Little-endian int32 at buf[off..off+3]. Caller must ensure off+4 <= len.
inline int32_t le32s(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

inline uint32_t abs_i32(int32_t v) {
    // Avoid UB on INT32_MIN by widening to int64 before negating.
    return (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
}

// --- PNG: 8-byte signature, then IHDR (len,"IHDR",width BE, height BE). ---
// Width is the big-endian uint32 at offset 16, height at offset 20.
bool probe_png(const uint8_t* buf, size_t len, ImageDims* out) {
    static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (len < 24) return false;
    for (int i = 0; i < 8; i++)
        if (buf[i] != kSig[i]) return false;
    // Bytes 12..15 should be "IHDR"; verify to avoid mis-reading a truncated PNG.
    if (buf[12] != 'I' || buf[13] != 'H' || buf[14] != 'D' || buf[15] != 'R')
        return false;
    out->format = ImgFormat::Png;
    out->width  = be32(buf + 16);
    out->height = be32(buf + 20);
    return true;
}

// --- BMP: 'B''M', DIB header. width int32 LE at 18, height int32 LE at 22. ---
// Height may be negative for a top-down bitmap, so take the absolute value.
bool probe_bmp(const uint8_t* buf, size_t len, ImageDims* out) {
    if (len < 26) return false;
    if (buf[0] != 'B' || buf[1] != 'M') return false;
    out->format = ImgFormat::Bmp;
    out->width  = abs_i32(le32s(buf + 18));
    out->height = abs_i32(le32s(buf + 22));
    return true;
}

// --- JPEG: FF D8, then a chain of marker segments. Walk them, skipping each
// by its big-endian length, until a Start-Of-Frame (SOF) marker carries the
// dimensions: [precision:1][height:2 BE][width:2 BE] after the length field. ---
bool probe_jpeg(const uint8_t* buf, size_t len, ImageDims* out) {
    if (len < 4) return false;
    if (buf[0] != 0xFF || buf[1] != 0xD8) return false;  // SOI

    size_t pos = 2;
    while (pos + 2 <= len) {
        if (buf[pos] != 0xFF) return false;   // markers must be 0xFF-aligned
        uint8_t marker = buf[pos + 1];

        // 0xFF fill bytes between markers: consume one and retry.
        if (marker == 0xFF) { pos++; continue; }

        // Standalone markers with no length field: SOI/EOI, RST0..RST7, TEM.
        if (marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            pos += 2;
            continue;
        }

        // Every other marker is followed by a 2-byte big-endian segment length
        // (which counts those 2 length bytes but not the FF+marker).
        if (pos + 4 > len) return false;
        uint32_t seglen = be16(buf + pos + 2);
        if (seglen < 2) return false;         // malformed length

        // SOF markers 0xC0..0xCF EXCEPT DHT(C4), JPG(C8), DAC(CC) hold the frame
        // dimensions. Any of these is what we are scanning for.
        bool is_sof = (marker >= 0xC0 && marker <= 0xCF) &&
                      marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            // Need 5 data bytes at pos+4 (precision, height:2, width:2).
            if (pos + 9 > len) return false;
            out->format = ImgFormat::Jpeg;
            out->height = be16(buf + pos + 5);
            out->width  = be16(buf + pos + 7);
            return true;
        }

        // Not an SOF: skip the whole segment (marker + declared length).
        pos += 2 + (size_t)seglen;
    }
    return false;   // ran out of buffer before any SOF marker
}

}  // namespace

bool image_probe_dims(const uint8_t* buf, size_t len, ImageDims* out) {
    if (!buf || !out) return false;
    if (probe_png(buf, len, out))  return true;
    if (probe_bmp(buf, len, out))  return true;
    if (probe_jpeg(buf, len, out)) return true;
    return false;
}

bool image_dims_within_budget(const ImageDims& d, uint32_t max_pixels) {
    // 64-bit product cannot overflow for uint32 operands, so an image whose
    // width*height exceeds uint32 still compares as larger than any budget.
    uint64_t pixels = (uint64_t)d.width * (uint64_t)d.height;
    return pixels <= (uint64_t)max_pixels;
}
