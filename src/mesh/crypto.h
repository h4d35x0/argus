// crypto.h — Meshtastic AES-CTR over the vendored pure AES. NO hardware.
// CTR is symmetric, so one call both encrypts and decrypts. The counter-block
// increment matches mbedTLS's mbedtls_aes_crypt_ctr (big-endian, incremented
// from the last byte), which is what 13:37 / Meshtastic use — so ciphertext is
// byte-compatible on the air.
#pragma once
#include <cstddef>
#include <cstdint>

namespace wl {
namespace mesh {

// Build the 16-byte Meshtastic AES-CTR nonce/initial-counter from the packet id
// and sender node id: packet_id as u64-LE in bytes [0..7] (high 32 zero),
// from_node as u64-LE in bytes [8..15] (high 32 zero).
void build_nonce(uint8_t nonce[16], uint32_t packet_id, uint32_t from_node);

// AES-CTR transform `len` bytes from `in` to `out` (may alias). key_bytes must be
// 16 or 32; returns false otherwise. `nonce` is the initial counter block.
bool aes_ctr_xcrypt(const uint8_t* key, size_t key_bytes, const uint8_t nonce[16],
                    const uint8_t* in, uint8_t* out, size_t len);

}  // namespace mesh
}  // namespace wl
