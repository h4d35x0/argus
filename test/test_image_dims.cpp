// test_image_dims.cpp — host unit tests for the pure image-header dimension
// probe (src/image_dims). These pin the exact byte offsets the wallpaper guard
// relies on for PNG / BMP / JPEG, PLUS the truncated / garbage buffers the
// probe exists to survive without over-reading, PLUS the overflow-safe budget
// check that keeps a multi-megapixel photo from OOMing the decode.
#include "wl_test.h"
#include "image_dims.h"

#include <cstdint>
#include <vector>

// ------------------------------- PNG --------------------------------------

// Minimal valid PNG header: 8-byte signature, IHDR chunk (length 0x0D, "IHDR",
// width, height). width=800 (0x00000320), height=600 (0x00000258).
static std::vector<uint8_t> make_png(uint32_t w, uint32_t h) {
    std::vector<uint8_t> v = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // signature
        0x00, 0x00, 0x00, 0x0D,                          // IHDR length = 13
        'I', 'H', 'D', 'R',                              // chunk type
    };
    // width (big-endian) at offset 16
    v.push_back((uint8_t)(w >> 24)); v.push_back((uint8_t)(w >> 16));
    v.push_back((uint8_t)(w >> 8));  v.push_back((uint8_t)w);
    // height (big-endian) at offset 20
    v.push_back((uint8_t)(h >> 24)); v.push_back((uint8_t)(h >> 16));
    v.push_back((uint8_t)(h >> 8));  v.push_back((uint8_t)h);
    // a few trailing IHDR fields so the buffer is realistic (>= 24 bytes)
    v.push_back(0x08); v.push_back(0x06); v.push_back(0x00);
    return v;
}

WL_TEST(png_valid_header_dims) {
    auto v = make_png(800, 600);
    ImageDims d{};
    WL_CHECK(image_probe_dims(v.data(), v.size(), &d));
    WL_CHECK(d.format == ImgFormat::Png);
    WL_CHECK_EQ(d.width, (uint32_t)800);
    WL_CHECK_EQ(d.height, (uint32_t)600);
}

WL_TEST(png_truncated_returns_false) {
    auto v = make_png(800, 600);
    ImageDims d{};
    // Only the first 20 bytes: height field is not fully present -> must fail.
    WL_CHECK(!image_probe_dims(v.data(), 20, &d));
    // Just the signature, nothing else.
    WL_CHECK(!image_probe_dims(v.data(), 8, &d));
}

WL_TEST(png_bad_ihdr_tag_returns_false) {
    auto v = make_png(800, 600);
    v[12] = 'X';  // corrupt the "IHDR" tag
    ImageDims d{};
    WL_CHECK(!image_probe_dims(v.data(), v.size(), &d));
}

// ------------------------------- BMP --------------------------------------

// Minimal BMP: 'BM', then enough header so offsets 18 (width) and 22 (height)
// are populated as signed int32 little-endian.
static std::vector<uint8_t> make_bmp(int32_t w, int32_t h) {
    std::vector<uint8_t> v(26, 0);
    v[0] = 'B'; v[1] = 'M';
    uint32_t uw = (uint32_t)w, uh = (uint32_t)h;
    v[18] = (uint8_t)uw; v[19] = (uint8_t)(uw >> 8);
    v[20] = (uint8_t)(uw >> 16); v[21] = (uint8_t)(uw >> 24);
    v[22] = (uint8_t)uh; v[23] = (uint8_t)(uh >> 8);
    v[24] = (uint8_t)(uh >> 16); v[25] = (uint8_t)(uh >> 24);
    return v;
}

WL_TEST(bmp_positive_height) {
    auto v = make_bmp(410, 502);
    ImageDims d{};
    WL_CHECK(image_probe_dims(v.data(), v.size(), &d));
    WL_CHECK(d.format == ImgFormat::Bmp);
    WL_CHECK_EQ(d.width, (uint32_t)410);
    WL_CHECK_EQ(d.height, (uint32_t)502);
}

WL_TEST(bmp_negative_height_top_down) {
    // Top-down BMP: height stored as -502. abs() must recover 502.
    auto v = make_bmp(410, -502);
    ImageDims d{};
    WL_CHECK(image_probe_dims(v.data(), v.size(), &d));
    WL_CHECK_EQ(d.width, (uint32_t)410);
    WL_CHECK_EQ(d.height, (uint32_t)502);
}

WL_TEST(bmp_truncated_returns_false) {
    auto v = make_bmp(410, 502);
    ImageDims d{};
    WL_CHECK(!image_probe_dims(v.data(), 25, &d));  // one byte short of offset 26
    WL_CHECK(!image_probe_dims(v.data(), 2, &d));   // just "BM"
}

// ------------------------------- JPEG -------------------------------------

// Build a JPEG: SOI, an APP0 (JFIF) segment, a DQT segment (both skipped by
// length), then an SOF0 segment carrying the dimensions.
static void push_be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
static std::vector<uint8_t> make_jpeg(uint16_t w, uint16_t h) {
    std::vector<uint8_t> v;
    v.push_back(0xFF); v.push_back(0xD8);            // SOI

    // APP0 segment: FF E0, length, then (length-2) payload bytes.
    v.push_back(0xFF); v.push_back(0xE0);
    push_be16(v, 6);                                 // length covers 4 payload
    v.push_back('J'); v.push_back('F'); v.push_back('I'); v.push_back('F');

    // DQT segment: FF DB, length 5 -> 3 payload bytes.
    v.push_back(0xFF); v.push_back(0xDB);
    push_be16(v, 5);
    v.push_back(0x00); v.push_back(0x11); v.push_back(0x22);

    // SOF0 segment: FF C0, length 17, precision 8, height, width, 3 components.
    v.push_back(0xFF); v.push_back(0xC0);
    push_be16(v, 17);
    v.push_back(0x08);                               // sample precision
    push_be16(v, h);                                 // height (BE)
    push_be16(v, w);                                 // width  (BE)
    v.push_back(0x03);                               // component count
    for (int i = 0; i < 9; i++) v.push_back(0x00);   // 3 components x 3 bytes
    return v;
}

WL_TEST(jpeg_sof0_after_skipped_segments) {
    auto v = make_jpeg(1024, 768);
    ImageDims d{};
    WL_CHECK(image_probe_dims(v.data(), v.size(), &d));
    WL_CHECK(d.format == ImgFormat::Jpeg);
    WL_CHECK_EQ(d.width, (uint32_t)1024);
    WL_CHECK_EQ(d.height, (uint32_t)768);
}

WL_TEST(jpeg_truncated_before_sof_returns_false) {
    auto v = make_jpeg(1024, 768);
    ImageDims d{};
    // Cut the buffer inside the SOF0 dimension bytes: probe must not over-read.
    // SOI(2)+APP0(8)+DQT(5) = 15 bytes consumed before SOF; SOF marker+len = 4,
    // precision = 1, so height starts at offset 20. Truncate at 21.
    WL_CHECK(!image_probe_dims(v.data(), 21, &d));
    // Just the SOI marker, no segments at all.
    WL_CHECK(!image_probe_dims(v.data(), 2, &d));
}

WL_TEST(jpeg_bad_segment_length_returns_false) {
    // SOI then a segment claiming length 0 (< 2 minimum): malformed -> false.
    static const uint8_t buf[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x00 };
    ImageDims d{};
    WL_CHECK(!image_probe_dims(buf, sizeof(buf), &d));
}

// ---------------------------- Unknown / garbage ---------------------------

WL_TEST(unknown_buffer_returns_false) {
    static const uint8_t garbage[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                       0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22 };
    ImageDims d{};
    WL_CHECK(!image_probe_dims(garbage, sizeof(garbage), &d));
}

WL_TEST(null_and_empty_buffer_return_false) {
    ImageDims d{};
    WL_CHECK(!image_probe_dims(nullptr, 64, &d));
    static const uint8_t one[1] = { 0x89 };
    WL_CHECK(!image_probe_dims(one, 0, &d));
    WL_CHECK(!image_probe_dims(one, 1, &d));
}

// ------------------------------ Budget check ------------------------------

WL_TEST(budget_within_and_over) {
    // 410x502 = 205,820 px: comfortably within a 1.2M budget.
    ImageDims small{ImgFormat::Png, 410, 502};
    WL_CHECK(image_dims_within_budget(small, 1200000u));

    // 4000x3000 = 12,000,000 px: a typical phone photo, well over budget.
    ImageDims big{ImgFormat::Jpeg, 4000, 3000};
    WL_CHECK(!image_dims_within_budget(big, 1200000u));
}

WL_TEST(budget_boundary_exact) {
    // Exactly max_pixels is inclusive-within-budget; one more pixel is over.
    ImageDims exact{ImgFormat::Png, 1000, 1200};      // 1,200,000
    WL_CHECK(image_dims_within_budget(exact, 1200000u));
    ImageDims over{ImgFormat::Png, 1000, 1201};        // 1,201,000
    WL_CHECK(!image_dims_within_budget(over, 1200000u));
}

WL_TEST(budget_overflow_safe) {
    // width*height overflows uint32 (0xFFFFFFFF * 2 wraps in 32-bit math). The
    // 64-bit multiply must see it as enormous and reject it.
    ImageDims huge{ImgFormat::Png, 0xFFFFFFFFu, 2u};
    WL_CHECK(!image_dims_within_budget(huge, 1200000u));
    // Even against the largest possible uint32 budget it stays over.
    WL_CHECK(!image_dims_within_budget(huge, 0xFFFFFFFFu));
}
