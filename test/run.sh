#!/usr/bin/env bash
# Host unit tests for ARGUS — no cmake, no make required.
#   bash test/run.sh
#
# Compiles the pure modules under src/ together with the self-registering
# test_*.cpp files and runs the binary. See test/README.md for details.
set -euo pipefail

# MinGW's cc1plus needs its runtime DLLs on PATH or it dies at load with no
# diagnostic; prepend the mingw bin dir if it exists (no-op on Linux/macOS).
MINGW_BIN="${MINGW_BIN:-/c/msys64/mingw64/bin}"
[ -d "$MINGW_BIN" ] && export PATH="$MINGW_BIN:$PATH"

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
mkdir -p build

# The Makefile's MODULES line is the SINGLE SOURCE OF TRUTH for what gets
# linked; this script parses it rather than keeping its own copy. It used to
# keep a copy, and the two drifted by five modules (log_retention, clock_time,
# gps_gsv, dst_rules, clock_sync), so this script no longer linked at all while
# `make -C test test` was green. Parsing keeps the two from ever disagreeing.
# Word splitting on the result is intentional: the paths carry no spaces.
MODULES=$(sed -n 's/^MODULES[[:space:]]*:=[[:space:]]*//p' Makefile)
if [ -z "$MODULES" ]; then
    echo "run.sh: could not read MODULES from test/Makefile" >&2
    exit 1
fi

BIN="build/argus_tests.exe"

# shellcheck disable=SC2086  # $MODULES must word-split into separate arguments
g++ -std=c++17 -Wall -Wextra -I. -I../src/mesh -I../src/ble \
    -I../src/detect -I../src/notify -I../src \
    $MODULES test_*.cpp -o "$BIN"

"$BIN"
