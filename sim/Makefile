# ARGUS simulator - builds the REAL screen source against LVGL's SDL backend on
# this laptop, so demo footage can be captured from current source at any frame
# rate without touching the watch.
#
# SIM ONLY. Nothing here is compiled into firmware. The device build is
# untouched: this uses its own sim/lv_conf.h (see the header in that file for
# the three flags that differ) and its own shim headers, which shadow
# LilyGoLib.h / SD.h / Arduino.h / Preferences.h via -Isim/shim.
#
#   make            build build/argus_sim.exe
#   make run        build and open a live SDL window
#   make frames     build and dump PNG frames to build/frames/
#   make clean      drop objects (keeps the LVGL lib)
#   make distclean  drop everything
#
# Requires msys2 mingw64 gcc + SDL2 (already present on this box:
# /c/msys64/mingw64, sdl2-config on PATH).

# DO NOT set SHELL here. `make` on this box is a WinGet build whose default
# SHELL is Git Bash sh.exe, and that shell is the one that works: it has
# TMP/TEMP set to /tmp, and its /usr/bin/find enumerates the LVGL tree fine.
#
# Pinning SHELL to msys2's sh.exe was tried and BROKE the build in a way that
# read like a toolchain fault: under msys2 sh spawned from here, TMP, TEMP and
# TMPDIR are all EMPTY, so gcc fell back to C:\WINDOWS and every single
# compile died with "Cannot create temporary file in C:\WINDOWS\: Permission
# denied". Exporting TMPDIR did not fix it either. Leave the shell alone.
#
# The compilers are pinned, because `gcc` on PATH depends on how the shell was
# launched and this build needs the mingw64 one specifically.
CC       := C:/msys64/mingw64/bin/gcc
CXX      := C:/msys64/mingw64/bin/g++
AR       := C:/msys64/mingw64/bin/ar
FIND     := find

# MANDATORY, and an absolute path to gcc.exe is NOT a substitute for it.
# mingw64/bin must be on PATH so the WINDOWS LOADER finds gcc's own DLLs
# (libisl-23, libgmp-10, libmpc-3, libmpfr-6, zlib1, libzstd, ...). Without it
# the loader picks up a foreign copy of one of those from some other program's
# directory on PATH, and cc1.exe dies with STATUS_ENTRYPOINT_NOT_FOUND
# (0xC0000139) BEFORE printing anything - so `gcc -c hello.c` fails with
# exit 1, zero bytes of output, on a one-line file. That reads exactly like a
# broken or quarantined toolchain and is neither; ldd even resolves every
# dependency correctly, because ldd does not use the real loader's search
# order. Measured 2026-09-03: identical command, exit 1 without this line and
# exit 0 with it.
export PATH := C:/msys64/mingw64/bin:$(PATH)

LVGL     := ../.pio/libdeps/twatch_ultra/lvgl
SRC      := ../src
SDL_INC  := C:/msys64/mingw64/include/SDL2
SDL_LIB  := C:/msys64/mingw64/lib

BUILD    := build
OBJ      := $(BUILD)/obj
LVOBJ    := $(BUILD)/lvobj

# -I$(LVGL)/.. is required and easy to miss: the generated font tables include
# "lvgl/lvgl.h" (WITH the directory prefix), so the LIBRARY'S PARENT has to be
# on the include path, not just the library itself.
INCS := -I. -Ishim -I$(LVGL)/.. -I$(LVGL) -I$(LVGL)/src -I$(SRC) -I$(SRC)/detect \
        -I$(SRC)/mesh -I$(SRC)/ble -I$(SRC)/notify -I$(SDL_INC)

# LV_CONF_INCLUDE_SIMPLE makes LVGL pick up sim/lv_conf.h off the -I. above
# rather than the device one beside the library.
DEFS := -DLV_CONF_INCLUDE_SIMPLE -DARGUS_SIM=1

CFLAGS   := -O2 -g $(DEFS) $(INCS)
CXXFLAGS := -O2 -g -std=gnu++17 -fpermissive $(DEFS) $(INCS)
LDFLAGS  := -L$(SDL_LIB) -lSDL2 -lmingw32 -mconsole

# ---- LVGL ------------------------------------------------------------------
# The 463-file source list is deliberately NOT held in a make variable. This
# make build fails to capture LARGE $(shell) output: `$(shell find ...)` over
# LVGL returns an EMPTY string (~25 KB of paths), while the same command piped
# through `wc -l` returns 463 correctly. That produced a silent "rebuilding 0
# objects". So the enumeration, the loop and the count check all happen inside
# the recipe shell, where the output never has to round-trip through make.
#
# Object names are derived from the full relative PATH, not the basename:
# LVGL basenames are NOT unique (vg_lite_matrix.c exists twice) and a flat
# object dir silently drops one of the pair.

# ---- ARGUS: the real screens, plus the shim and the harness ----------------
# Deliberately a SUBSET. Pulling all 116 src files would drag in WiFi/BLE/LoRa/
# mesh/detector code that has no host equivalent; these are the files the demo
# actually renders, and every one of them is the real firmware source.
ARGUS_CXX := $(SRC)/theme.cpp $(SRC)/argus_mode.cpp $(SRC)/time_screen.cpp \
             $(SRC)/world_clock_screen.cpp $(SRC)/dst_rules.cpp \
             $(SRC)/clock_time.cpp $(SRC)/calendar_screen.cpp \
             $(SRC)/stopwatch_screen.cpp $(SRC)/timer_screen.cpp \
             $(SRC)/tools_screen.cpp $(SRC)/threat_radar_screen.cpp
# alarm_screen.cpp is NOT here on purpose: it needs 18 alarm_* entry points
# from alarm.cpp, and alarm.cpp pulls in ESP-IDF driver/i2s.h plus
# SD.open/mkdir - and its screen would render nothing but defaults anyway.
# tools_screen.cpp is out for the same class of reason: 34 includes, i.e.
# the entire detector suite.
# Real generated art for the Flipper tile, compiled rather than stubbed.
ARGUS_C   := $(wildcard $(SRC)/font_argus_*.c) $(SRC)/flipper_logo_img.c
SIM_CXX   := shim/shim.cpp stubs.cpp stubs_tools.cpp main_sim.cpp

A_OBJS := $(patsubst %,$(OBJ)/%.o,$(notdir $(basename $(ARGUS_CXX)))) \
          $(patsubst %,$(OBJ)/%.o,$(notdir $(basename $(ARGUS_C)))) \
          $(patsubst %,$(OBJ)/%.o,$(notdir $(basename $(SIM_CXX))))

BIN := $(BUILD)/argus_sim.exe

.PHONY: all run frames clean distclean lvgl
all: $(BIN)

$(BIN): $(LVOBJ)/.stamp $(A_OBJS)
	@echo "  LINK  $@"
	@$(CXX) $(A_OBJS) $(BUILD)/liblvgl.a -o $@ $(LDFLAGS)

# One stamp for the whole LVGL library: it is 463 files that change only when
# lv_conf.h does, and a stale object compiled against an older conf is the
# failure this stamp exists to prevent (lv_init.c kept a reference to
# lv_fs_posix_init after that driver was switched off).
# Preflight. LVGL is a PlatformIO-managed dependency under .pio/libdeps,
# which is gitignored - so a FRESH CLONE does not have it and this build fails
# with a bare "find: ... No such file or directory" that does not say what to
# do. Say it.
.PHONY: check-lvgl
check-lvgl:
	@test -d $(LVGL)/src || { \
	  echo ""; \
	  echo "  LVGL not found at $(LVGL)"; \
	  echo ""; \
	  echo "  It is a PlatformIO dependency and .pio/ is gitignored, so a fresh"; \
	  echo "  clone has to fetch it once:"; \
	  echo ""; \
	  echo "      pio run -e twatch_ultra"; \
	  echo ""; \
	  echo "  (on this box pio is not on PATH:"; \
	  echo "   ~/.platformio/penv/Scripts/pio.exe)"; \
	  echo ""; \
	  exit 1; }

$(LVOBJ)/.stamp: lv_conf.h | check-lvgl
	@rm -rf $(LVOBJ) && mkdir -p $(LVOBJ)
	@$(FIND) $(LVGL)/src -name '*.c' > $(LVOBJ)/srcs.txt
	@src=`wc -l < $(LVOBJ)/srcs.txt`; \
	 echo "  LVGL  rebuilding $$src objects against sim/lv_conf.h (~2 min)"; \
	 set -e; \
	 while read -r f; do \
	   o=$(LVOBJ)/`echo "$$f" | sed 's|^$(LVGL)/src/||; s|/|__|g; s|\.c$$|.o|'`; \
	   $(CC) $(CFLAGS) -c "$$f" -o "$$o"; \
	 done < $(LVOBJ)/srcs.txt; \
	 n=`ls $(LVOBJ)/*.o | wc -l`; \
	 if [ "$$n" -ne "$$src" ]; then \
	   echo "object count $$n != source count $$src - object name collision"; exit 1; fi; \
	 echo "  LVGL  $$n/$$src objects ok"
	@$(AR) rcs $(BUILD)/liblvgl.a $(LVOBJ)/*.o
	@touch $@

$(OBJ)/%.o: $(SRC)/%.cpp | $(OBJ)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ)/%.o: $(SRC)/%.c | $(OBJ)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: %.cpp | $(OBJ)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ)/%.o: shim/%.cpp | $(OBJ)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ):
	@mkdir -p $(OBJ)

run: $(BIN)
	@$(BIN) --live

frames: $(BIN)
	@rm -rf $(BUILD)/frames && mkdir -p $(BUILD)/frames
	@$(BIN) --frames $(BUILD)/frames

clean:
	@rm -rf $(OBJ) $(BIN)

distclean:
	@rm -rf $(BUILD)
