// test_mesh_crypto.cpp — AES + AES-CTR validated against published standard
// vectors (FIPS-197 for the cipher, NIST SP800-38A for CTR mode). Passing the
// NIST CTR vector confirms our counter-increment matches mbedTLS/Meshtastic, so
// the ciphertext is byte-compatible on the air.
#include "wl_test.h"
#include "aes.h"
#include "crypto.h"

#include <cstdint>
#include <vector>

using namespace wl::mesh;

// Parse an even-length hex string into bytes.
static std::vector<uint8_t> hex(const char* s) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  std::vector<uint8_t> out;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2)
    out.push_back(static_cast<uint8_t>((val(s[i]) << 4) | val(s[i + 1])));
  return out;
}

static bool eq(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
  return true;
}

WL_TEST(aes128_fips197_vector) {
  auto key = hex("000102030405060708090a0b0c0d0e0f");
  auto blk = hex("00112233445566778899aabbccddeeff");
  auto want = hex("69c4e0d86a7b0430d8cdb78070b4c55a");
  Aes aes;
  WL_CHECK(aes.init(key.data(), key.size()));
  aes.encrypt_block(blk.data());
  WL_CHECK(eq(blk, want));
}

WL_TEST(aes256_fips197_vector) {
  auto key = hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  auto blk = hex("00112233445566778899aabbccddeeff");
  auto want = hex("8ea2b7ca516745bfeafc49904b496089");
  Aes aes;
  WL_CHECK(aes.init(key.data(), key.size()));
  aes.encrypt_block(blk.data());
  WL_CHECK(eq(blk, want));
}

WL_TEST(aes_rejects_bad_key_size) {
  Aes aes;
  uint8_t k[24] = {0};
  WL_CHECK(!aes.init(k, 24));  // AES-192 not supported
  WL_CHECK(!aes.init(k, 0));
}

WL_TEST(ctr_nist_sp800_38a_aes128_two_blocks) {
  auto key   = hex("2b7e151628aed2a6abf7158809cf4f3c");
  auto nonce = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
  auto pt    = hex("6bc1bee22e409f96e93d7e117393172a"   // block 1
                   "ae2d8a571e03ac9c9eb76fac45af8e51"); // block 2 (tests carry)
  auto want  = hex("874d6191b620e3261bef6864990db6ce"
                   "9806f66b7970fdff8617187bb9fffdff");
  std::vector<uint8_t> out(pt.size());
  WL_CHECK(aes_ctr_xcrypt(key.data(), key.size(), nonce.data(), pt.data(),
                          out.data(), pt.size()));
  WL_CHECK(eq(out, want));
}

WL_TEST(ctr_is_symmetric_roundtrip_partial_block) {
  auto key   = hex("000102030405060708090a0b0c0d0e0f");
  uint8_t nonce[16];
  build_nonce(nonce, 0x12345678u, 0xDEADBEEFu);
  // 37 bytes -> exercises the partial final block (nc_off wraparound).
  std::vector<uint8_t> pt(37);
  for (size_t i = 0; i < pt.size(); ++i) pt[i] = static_cast<uint8_t>(i * 7 + 1);
  std::vector<uint8_t> ct(pt.size()), back(pt.size());
  WL_CHECK(aes_ctr_xcrypt(key.data(), key.size(), nonce, pt.data(), ct.data(), pt.size()));
  WL_CHECK(aes_ctr_xcrypt(key.data(), key.size(), nonce, ct.data(), back.data(), ct.size()));
  WL_CHECK(eq(pt, back));
  WL_CHECK(!eq(pt, ct));  // it actually encrypted
}

WL_TEST(build_nonce_layout) {
  uint8_t n[16];
  build_nonce(n, 0x01020304u, 0x0A0B0C0Du);
  // packet_id LE in [0..3], zero [4..7]; from_node LE in [8..11], zero [12..15].
  WL_CHECK_EQ(static_cast<int>(n[0]), 0x04);
  WL_CHECK_EQ(static_cast<int>(n[1]), 0x03);
  WL_CHECK_EQ(static_cast<int>(n[2]), 0x02);
  WL_CHECK_EQ(static_cast<int>(n[3]), 0x01);
  WL_CHECK_EQ(static_cast<int>(n[4]), 0x00);
  WL_CHECK_EQ(static_cast<int>(n[8]), 0x0D);
  WL_CHECK_EQ(static_cast<int>(n[11]), 0x0A);
  WL_CHECK_EQ(static_cast<int>(n[15]), 0x00);
}
