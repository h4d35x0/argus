// aes.h — vendored pure AES forward cipher (encrypt only). NO hardware, NO libs.
// Meshtastic uses AES-CTR, which needs only the forward cipher (CTR is symmetric:
// the same keystream encrypts and decrypts). Vendoring a tiny pure AES (instead
// of seaming out to mbedTLS) keeps the crypto byte-identical on host and device
// and lets host tests assert against the FIPS-197 / NIST standard test vectors.
// Algorithm is the textbook AES (tiny-AES-c lineage, public domain).
#pragma once
#include <cstddef>
#include <cstdint>

namespace wl {
namespace mesh {

class Aes {
 public:
  // Initialize with a 16-byte (AES-128) or 32-byte (AES-256) key. Returns false
  // for any other key size.
  bool init(const uint8_t* key, size_t key_bytes);

  // Encrypt one 16-byte block in place. init() must have succeeded first.
  void encrypt_block(uint8_t block[16]) const;

 private:
  uint8_t round_key_[240] = {0};  // max expanded key (AES-256: 15 round keys)
  int nr_ = 0;                    // number of rounds (10 or 14); 0 = uninitialized
};

}  // namespace mesh
}  // namespace wl
