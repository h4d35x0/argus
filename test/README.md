# ARGUS — host unit tests

Pure-logic unit tests that run on the **host** (g++), no watch required. This is
the engineering-rigor layer we bring to the 13-37 base: the security-critical
logic (Meshtastic crypto, detector decisions) is validated in milliseconds
against known-answer vectors instead of only being exercised by flashing.

## Run

```sh
bash test/run.sh
```

(`make -C test test` also works if you have `make`; this box has neither `make`
nor `cmake`, so `run.sh` is the primary entry point — it needs only g++.)

Expected:

```
Running 6 test(s)...
[PASS] aes128_fips197_vector
[PASS] aes256_fips197_vector
[PASS] aes_rejects_bad_key_size
[PASS] ctr_nist_sp800_38a_aes128_two_blocks
[PASS] ctr_is_symmetric_roundtrip_partial_block
[PASS] build_nonce_layout

20 check(s), 0 failure(s)
```

## Windows / MSYS2 note

MinGW's `cc1plus` needs `C:\msys64\mingw64\bin` on `PATH` or it fails to load its
runtime DLLs with no error message. The `Makefile` prepends it via `MINGW_BIN`
(override if your MSYS2 lives elsewhere). On Linux/macOS that path just doesn't
exist and is ignored.

## Layout

- `wl_test.h` — tiny header-only harness (self-registering `WL_TEST`, `WL_CHECK*`).
- `test_main.cpp` — entry point; runs all registered tests.
- `test_*.cpp` — one file per module under test.
- Modules under test live in `../src/mesh/` (pure, no hardware includes) and are
  compiled straight into the test binary.

## What's covered so far

| Module | Test | Validates |
|--------|------|-----------|
| `src/mesh/aes.*`, `crypto.*` | `test_mesh_crypto.cpp` | AES-128/256 (FIPS-197), AES-CTR (NIST SP800-38A), Meshtastic nonce layout |

Ported from the sibling `argus` repo, which has the fuller suite (detectors,
dedup, store-format). Next candidates to extract from 13-37 and cover here:
Evil-Twin decision logic, the BLE AD-record parser (AirTag/Flipper/Skimmer),
and wiring `src/mesh/` into `src/meshtastic.cpp` so the on-device path uses the
tested crypto.
