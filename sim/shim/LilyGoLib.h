#pragma once
// Host shim for LilyGoLib's `instance` singleton. SIM ONLY - never built into
// firmware, and deliberately NOT a copy of the real header.
//
// It covers exactly the members the screen code reaches for (enumerated by
// grepping `instance.` across src/*_screen.cpp and src/theme.cpp): rtc, pmu,
// gps and a few whole-device calls. Everything is backed by plain host state so
// a demo render is deterministic and repeatable.
//
// The values are SCENE STATE, not fiction dressed as measurement: the sim sets
// them explicitly per scene (sim_set_clock, sim_set_battery, ...) so what the
// captured frame shows is exactly what the harness asked for. Nothing here
// claims to be a reading from real hardware.
#include "Arduino.h"
#include <time.h>

struct SimRtc {
    void getDateTime(struct tm *out);
    void setDateTime(struct tm t);
    void hwClockRead(void) {}
};

struct SimPmu {
    int  getBatteryPercent(void);
    bool isCharging(void);
    bool isVbusIn(void);
    bool isEnableDLDO(int) { return true; }
};

// TinyGPSPlus-shaped accessors, only the ones the screens read. Each inner
// struct mirrors the real library's value/age pattern closely enough to compile.
struct SimGpsVal    { double deg = 0;  bool isValid() const { return false; } double lat() const { return 0; } double lng() const { return 0; } };
struct SimGpsNum    { uint32_t v = 0;  uint32_t value() const { return v; } bool isValid() const { return false; } };
struct SimGpsDouble { double v = 0;    double value() const { return v; }   bool isValid() const { return false; } double meters() const { return v; } double kmph() const { return v; } };
struct SimGpsDate   { uint16_t year() const { return 2026; } uint8_t month() const { return 9; } uint8_t day() const { return 3; } bool isValid() const { return false; } };
struct SimGpsTime   { uint8_t hour() const { return 0; } uint8_t minute() const { return 0; } uint8_t second() const { return 0; } bool isValid() const { return false; } };

struct SimGps {
    SimGpsVal    location;
    SimGpsNum    satellites;
    SimGpsDouble hdop, altitude, speed;
    SimGpsDate   date;
    SimGpsTime   time;
    uint32_t charsProcessed(void) { return 0; }
    uint32_t failedChecksum(void) { return 0; }
    uint32_t passedChecksum(void) { return 0; }
    uint32_t sentencesWithFix(void) { return 0; }
    bool     encode(char) { return false; }
    void     loop(void) {}
};

struct SimWatch {
    SimRtc rtc;
    SimPmu pmu;
    SimGps gps;
    void begin(void) {}
    void setBrightness(uint8_t) {}
    void vibrator(void) {}
    bool isCardReady(void) { return false; }
    bool initLoRa(void) { return false; }
    bool initNFC(void) { return false; }
    void powerControl(int, bool) {}
};

extern SimWatch instance;

// ---- scene controls, driven by the capture harness -------------------------
void sim_set_clock(int year, int mon, int day, int hour, int min, int sec);
void sim_set_battery(int pct, bool charging, bool vbus);
