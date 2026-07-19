// image_dims.h — pure, host-testable image-header dimension probe.
//
// The SD-card wallpaper feature (background.cpp) hands whatever image it finds
// straight to LVGL, which decodes it at full source resolution into an ARGB
// buffer. A multi-megapixel phone photo decodes to tens of megabytes and OOMs
// the watch. This module reads ONLY the header bytes of an image buffer and
// reports its pixel dimensions, so the loader can reject an oversized source
// BEFORE any decode is attempted.
//
// Design constraints (deliberate, matching src/ble/adv_parser.h):
//   * Pure C++. NO Arduino.h, NO ESP-IDF, NO LVGL, NO SD, NO hardware. Compiles
//     and runs on the host g++ so the parsing logic is unit-tested in ms.
//   * No dynamic allocation. Reads a caller-owned buffer; never over-reads.
//   * Const-correct. The input buffer is never modified.
//
// Only the small fixed header of each format is inspected, so a 64-byte prefix
// of the file is enough to get the dimensions of a PNG, BMP or (typical) JPEG.
#pragma once

#include <cstddef>
#include <cstdint>

enum class ImgFormat : uint8_t {
    Unknown = 0,
    Png,
    Bmp,
    Jpeg,
};

struct ImageDims {
    ImgFormat format;   // detected container format
    uint32_t  width;    // pixels
    uint32_t  height;   // pixels
};

// Parse just the header of `buf` (length `len`) and fill `*out` with the pixel
// dimensions and detected format. Returns false if the buffer is null, the
// format is unrecognized, or the header is truncated (a short/garbage buffer
// must return false, never over-read). On false, `*out` is left untouched.
bool image_probe_dims(const uint8_t* buf, size_t len, ImageDims* out);

// True if width*height <= max_pixels. Overflow-safe: the multiply is done in
// 64-bit, so a width*height that would overflow uint32 still compares correctly
// (and therefore returns false against any sane budget). Boundary is inclusive:
// exactly max_pixels is within budget.
bool image_dims_within_budget(const ImageDims& d, uint32_t max_pixels);
