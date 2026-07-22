// notify_log.h - debug logging for the phone-notification subsystem.
//
// OFF by default. The notification pipeline handles private content (message
// titles, bodies, sender numbers, app identifiers), so in normal builds NOTHING
// from it is written to the USB serial port. Define ARGUS_NOTIFY_DEBUG at build
// time (the `ancs_spike` env does) to turn the tracing back on for bench work.
#pragma once

#ifdef ARGUS_NOTIFY_DEBUG
  #include <Arduino.h>
  #define NLOG(...)   Serial.printf(__VA_ARGS__)
  #define NLOGLN(x)   Serial.println(x)
#else
  #define NLOG(...)   ((void)0)
  #define NLOGLN(x)   ((void)0)
#endif
