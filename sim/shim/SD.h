#pragma once
// Host shim for the SD / Arduino-String surface the screen code touches.
// SIM ONLY.
//
// exists() answers from a real host directory when sim_sd_set_root() is called,
// so the /Icons PNG path in time_screen.cpp can be exercised for real against a
// copy of a card. With no root set it answers false - the same branch a watch
// with no card takes. That is a safe default rather than a pretend-success.
//
// File and String exist because tools_screen.cpp persists its tile order to
// /Settings. Both of those paths are guarded by instance.isCardReady(), which
// the shim reports false, so they return before touching the filesystem; these
// types are here to COMPILE that code, not to emulate a card. Any File opened
// here is invalid by construction, which is why operator! returns true.
#include "Arduino.h"
#include <string>

void sim_sd_set_root(const char *host_dir);

// Arduino String, only the methods the screen code calls.
class String {
public:
    String() {}
    String(const char *s) : v(s ? s : "") {}
    void   trim();
    size_t length() const { return v.size(); }
    bool   equals(const char *o) const { return o && v == o; }
    const char *c_str() const { return v.c_str(); }
private:
    std::string v;
};

#define FILE_READ  "r"
#define FILE_WRITE "w"

class File {
public:
    File() {}
    // Always invalid: the sim never has a card mounted. Callers all test this
    // and bail, which is exactly the no-card path on the device.
    explicit operator bool() const { return false; }
    bool   operator!() const { return true; }
    bool   available() { return false; }
    String readStringUntil(char) { return String(); }
    int    printf(const char *, ...) { return 0; }
    void   close() {}
};

class SimSD {
public:
    bool exists(const char *path);
    bool mkdir(const char *) { return false; }
    File open(const char *, const char * = FILE_READ) { return File(); }
    bool begin(void) { return false; }
};

extern SimSD SD;
