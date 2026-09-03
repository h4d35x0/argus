# Host unit tests for ARGUS — pure logic, NO hardware.
# Run:  make -C test test        (from the repo root)
#
# These compile with the host g++, not the ESP32 toolchain, so the security-
# critical logic (mesh crypto, detectors) is testable in milliseconds without
# flashing. Ported from the sibling `argus` repo's proven test pattern.
#
# NOTE (this Windows/MSYS2 box): MinGW's cc1plus needs mingw64/bin on PATH or it
# fails to load its runtime DLLs with no diagnostic. MINGW_BIN handles that; it
# prepends a path that simply doesn't exist on Linux/macOS, so this stays portable.
MINGW_BIN ?= /c/msys64/mingw64/bin
export PATH := $(MINGW_BIN):$(PATH)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -I. -I../src/mesh -I../src/ble -I../src/detect -I../src/notify -I../src
BUILD    := build

# Pure modules under test (add more as they're extracted). NO hardware includes.
MODULES  := ../src/mesh/aes.cpp ../src/mesh/crypto.cpp ../src/ble/adv_parser.cpp ../src/image_dims.cpp ../src/geo_cell.cpp ../src/detect/evil_twin.cpp ../src/detect/tail_detect.cpp ../src/detect/deauth_flood.cpp ../src/detect/ble_spam.cpp ../src/detect/beacon_flood.cpp ../src/detect/threat_state.cpp ../src/detect/threat_map.cpp ../src/detect/tracker_ident.cpp ../src/detect/threat_log.cpp ../src/detect/surveillance_device.cpp ../src/notify/notification_store.cpp ../src/device_mode_plan.cpp ../src/detect/log_retention.cpp ../src/clock_time.cpp ../src/gps_gsv.cpp ../src/dst_rules.cpp
# Every test_*.cpp self-registers via WL_TEST; test_main.cpp provides main().
TESTS    := $(wildcard test_*.cpp)
BIN      := $(BUILD)/argus_tests.exe

$(BIN): $(MODULES) $(TESTS) wl_test.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(MODULES) $(TESTS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)

.PHONY: test clean
