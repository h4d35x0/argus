#!/usr/bin/env python3
"""
Paste the three [MESH] log lines from Serial monitor into this script,
then run it to find (or verify) the correct Meshtastic AES key.

Usage:
    python3 find_mesh_key.py

Paste example (replace with your actual captured values):
    [MESH] from=AABBCCDD pkt_id=12345678 ct_len=47
    [MESH] NONCE: 78563412 00000000 DDCCBBAA 00000000
    [MESH] CT: 1A 2B 3C 4D ...
"""

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend
import binascii

# --- PASTE YOUR OWN CAPTURED VALUES HERE -------------------------------------
#
# These are PLACEHOLDERS. The upstream 13-37 project shipped this file with real
# packets captured off a live mesh (a node id, nonce and ciphertext). Those were
# someone else's traffic, so they were removed rather than republished here.
# Capture your own from the [MESH] serial lines and paste them below.
#
# Expected shape, from the serial log:
#   [MESH] from=AABBCCDD pkt_id=11223344 ct_len=17
#   [MESH] NONCE: 44332211 00000000 DDCCBBAA 00000000
#   [MESH] CT: 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 00

NONCE_HEX = ""   # e.g. "44332211 00000000 DDCCBBAA 00000000"
CT_HEX    = ""   # e.g. "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 00"

if not NONCE_HEX or not CT_HEX:
    raise SystemExit(
        "Paste your own NONCE_HEX and CT_HEX (from the [MESH] serial lines) "
        "before running. This script ships with no captured data on purpose."
    )
# -----------------------------------------------------------------------------

# Known Meshtastic default-channel key candidates to test
CANDIDATES = {
    "AQ== zero-padded (0x01 0x00*15)":
        bytes([0x01] + [0x00]*15),

    "well-known LongFast (d4 f1 bb ...)":
        bytes([0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,
               0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01]),

    "AQ== base64-decoded then SHA-256 first 16 bytes":
        # base64.b64decode("AQ==") = b'\x01'; sha256(b'\x01')[:16]
        bytes.fromhex("55a54008ad1ba589aa210d2629c1df41"),

    "all-zeros":
        bytes(16),

    "Meshtastic firmware default (1.0 era)":
        bytes([0x4c,0x4f,0x52,0x41,0x77,0x61,0x6e,0x00,
               0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00]),
}

def aes_ctr_decrypt(key: bytes, nonce: bytes, ct: bytes) -> bytes:
    cipher = Cipher(algorithms.AES(key), modes.CTR(nonce), backend=default_backend())
    d = cipher.decryptor()
    return d.update(ct) + d.finalize()

def parse_hex(s: str) -> bytes:
    return bytes.fromhex(s.replace(" ", "").replace("\n", ""))

nonce = parse_hex(NONCE_HEX)
ct    = parse_hex(CT_HEX)

# CTR mode needs a 128-bit nonce; Python's cryptography uses it as the full counter block
if len(nonce) != 16:
    raise ValueError(f"Nonce must be 16 bytes, got {len(nonce)}")

print(f"Nonce : {nonce.hex()}")
print(f"CT    : {ct.hex()}")
print()

found = False
for label, key in CANDIDATES.items():
    pt = aes_ctr_decrypt(key, nonce, ct)
    verdict = "GOOD KEY ✓" if pt[0:1] == b'\x08' else "bad"
    print(f"[{verdict}] {label}")
    print(f"         key   = {key.hex()}")
    print(f"         plain = {pt[:16].hex()}")
    if pt[0:1] == b'\x08':
        found = True
        portnum = pt[1] if len(pt) > 1 else 0
        portnames = {1:"TEXT", 3:"POSITION", 4:"NODEINFO"}
        print(f"         portnum = {portnum} ({portnames.get(portnum,'UNKNOWN')})")
    print()

if not found:
    print("No candidate key worked. Try fetching the key from the Meshtastic Python library:")
    print()
    print("  pip install meshtastic")
    print("  python3 -c \"")
    print("  from meshtastic.mesh_pb2 import Channel")
    print("  from meshtastic.channel_pb2 import ChannelSettings")
    print("  from meshtastic.crypto import generate_key")
    print("  # or look at meshtastic/crypto.py for DEFAULT_KEY constant")
    print("  \"")
    print()
    print("Or check the Meshtastic firmware source:")
    print("  https://github.com/meshtastic/firmware/blob/master/src/mesh/CryptoEngine.cpp")
    print("  search for 'defaultpsk' or 'AQ=='")
