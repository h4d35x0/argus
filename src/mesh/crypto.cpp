// crypto.cpp — Meshtastic AES-CTR. Pure logic.
#include "crypto.h"

#include "aes.h"

namespace wl {
namespace mesh {

void build_nonce(uint8_t nonce[16], uint32_t packet_id, uint32_t from_node) {
  for (int i = 0; i < 16; ++i) nonce[i] = 0;
  // packet_id as u64-LE in [0..7]: low 32 bits are packet_id, high 32 stay zero.
  nonce[0] = static_cast<uint8_t>(packet_id & 0xFF);
  nonce[1] = static_cast<uint8_t>((packet_id >> 8) & 0xFF);
  nonce[2] = static_cast<uint8_t>((packet_id >> 16) & 0xFF);
  nonce[3] = static_cast<uint8_t>((packet_id >> 24) & 0xFF);
  // from_node as u64-LE in [8..15].
  nonce[8]  = static_cast<uint8_t>(from_node & 0xFF);
  nonce[9]  = static_cast<uint8_t>((from_node >> 8) & 0xFF);
  nonce[10] = static_cast<uint8_t>((from_node >> 16) & 0xFF);
  nonce[11] = static_cast<uint8_t>((from_node >> 24) & 0xFF);
}

bool aes_ctr_xcrypt(const uint8_t* key, size_t key_bytes, const uint8_t nonce[16],
                    const uint8_t* in, uint8_t* out, size_t len) {
  Aes aes;
  if (!aes.init(key, key_bytes)) return false;

  uint8_t counter[16];
  for (int i = 0; i < 16; ++i) counter[i] = nonce[i];
  uint8_t stream[16] = {0};
  size_t nc_off = 0;

  for (size_t i = 0; i < len; ++i) {
    if (nc_off == 0) {
      // Encrypt the current counter block to produce the next keystream block.
      for (int j = 0; j < 16; ++j) stream[j] = counter[j];
      aes.encrypt_block(stream);
      // Increment the 16-byte counter big-endian, from the last byte (mbedTLS).
      for (int j = 15; j >= 0; --j) {
        if (++counter[j] != 0) break;  // stop at first non-wrapping byte
      }
    }
    out[i] = static_cast<uint8_t>(in[i] ^ stream[nc_off]);
    nc_off = (nc_off + 1) & 0x0F;
  }
  return true;
}

}  // namespace mesh
}  // namespace wl
