#pragma once
// Host shim for ESP32 Preferences (NVS). SIM ONLY.
// In-memory, non-persistent: argus_mode.cpp only uses it to remember the last
// mode, and a demo render wants a known starting mode every run anyway.
#include "Arduino.h"

class Preferences {
public:
    bool     begin(const char *, bool = false) { return true; }
    void     end(void) {}
    bool     clear(void) { return true; }
    size_t   putUChar(const char *k, uint8_t v);
    uint8_t  getUChar(const char *k, uint8_t def = 0);
    size_t   putBool(const char *k, bool v);
    bool     getBool(const char *k, bool def = false);
    size_t   putUInt(const char *k, uint32_t v);
    uint32_t getUInt(const char *k, uint32_t def = 0);
    bool     remove(const char *k);
};
