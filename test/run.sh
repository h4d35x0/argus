#!/usr/bin/env bash
# Host unit tests for ARGUS — no cmake, no make required.
#   bash test/run.sh
#
# Compiles the pure modules under src/mesh/ together with the self-registering
# test_*.cpp files and runs the binary. See test/README.md for details.
set -euo pipefail

# MinGW's cc1plus needs its runtime DLLs on PATH or it dies at load with no
# diagnostic; prepend the mingw bin dir if it exists (no-op on Linux/macOS).
MINGW_BIN="${MINGW_BIN:-/c/msys64/mingw64/bin}"
[ -d "$MINGW_BIN" ] && export PATH="$MINGW_BIN:$PATH"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
mkdir -p "$HERE/build"

MODULES=("$ROOT"/src/mesh/aes.cpp "$ROOT"/src/mesh/crypto.cpp \
         "$ROOT"/src/ble/adv_parser.cpp "$ROOT"/src/image_dims.cpp \
         "$ROOT"/src/geo_cell.cpp \
         "$ROOT"/src/detect/evil_twin.cpp "$ROOT"/src/detect/tail_detect.cpp \
         "$ROOT"/src/detect/deauth_flood.cpp "$ROOT"/src/detect/ble_spam.cpp \
         "$ROOT"/src/detect/beacon_flood.cpp \
         "$ROOT"/src/detect/threat_state.cpp "$ROOT"/src/detect/threat_map.cpp \
         "$ROOT"/src/detect/tracker_ident.cpp "$ROOT"/src/detect/threat_log.cpp \
         "$ROOT"/src/detect/surveillance_device.cpp \
         "$ROOT"/src/notify/notification_store.cpp \
         "$ROOT"/src/device_mode_plan.cpp)
BIN="$HERE/build/argus_tests.exe"

g++ -std=c++17 -Wall -Wextra -I "$HERE" -I "$ROOT/src/mesh" -I "$ROOT/src/ble" \
    -I "$ROOT/src/detect" -I "$ROOT/src/notify" -I "$ROOT/src" \
    "${MODULES[@]}" "$HERE"/test_*.cpp -o "$BIN"

"$BIN"
