#include "gps_screen.h"
#include "gps_gsv.h"       // satellites IN VIEW + C/N0 (see gps_health.h)
#include "dst_rules.h"    // us_dst_active(): one definition, host-tested
#include "tools_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <SD.h>
#include "timezone.h"
#include "usb_sd.h"
#include "hexhound.h"     // new GPS cell = new territory = pet exploration XP
#include "argus_mode.h"          // health log: which mode was live when GPS dropped
#include "ble_scan_manager.h"    // health log: BLE consumer count
#include "wifi_radio_screen.h"   // health log: WiFi rail state
#include "lora_screen.h"         // health log: LoRa rail state

// Defined in main.cpp
void clock_screen_set_gps_active(bool active);
void clock_screen_set_sat_count(uint32_t count);
void clock_screen_set_utc_offset(int offset_hours);
bool clock_screen_manual_time_active();
void clock_screen_get_local_time(struct tm *out);

// Lower-bound longitude (×10, integer degrees) for each UTC hour offset -12 … +12.
// A longitude belongs to offset (i - 12) when lon×10 >= k_tz_lower[i].
// Zone boundaries sit at every 7.5° — the midpoint between standard meridians.
static const int16_t k_tz_lower[25] = {
    -1800, -1725, -1575, -1425, -1275, -1125,  -975,  -825,
     -675,  -525,  -375,  -225,   -75,    75,   225,   375,
      525,   675,   825,   975,  1125,  1275,  1425,  1575,
     1725
};

static int utc_offset_from_longitude(double lon)
{
    int16_t lon10 = (int16_t)(lon * 10.0);
    int offset = -12;
    for (int i = 0; i < 25; i++) {
        if (lon10 < k_tz_lower[i]) break;
        offset = i - 12;
    }
    return offset;
}

// Zeller's congruence for the Gregorian calendar. Returns 0 = Sunday,

static lv_obj_t *gps_screen;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;

static lv_obj_t *val_satellites;
static lv_obj_t *val_latitude;
static lv_obj_t *val_longitude;
static lv_obj_t *val_date;
static lv_obj_t *val_gps_time;
static lv_obj_t *val_altitude;
static lv_obj_t *val_speed;
static lv_obj_t *val_fix_age;
static lv_obj_t *val_status;    // WHY there is no fix - see gps_health.h
static lv_obj_t *val_nmea;      // NMEA byte rate - see the GPS HEALTH block
static lv_obj_t *val_csum;      // passed/failed checksums this power cycle

static bool gps_powered = false;
static bool rtc_synced  = false; // reset each GPS power cycle

// Set when the radio is switched off while it HELD a fix, cleared on the next
// power-on. Surfaced in the Status row because that switch is not the harmless
// thing it looks like: a receiver tracks a satellite far below the signal it
// needs to ACQUIRE one, so a fix carried indoors survives while the same spot
// yields nothing from a standing start. Measured 2026-09-02: a fix held indoors
// on 5-8 satellites, switched off, then 16m45s of exactly nothing after being
// switched back on 3m36s later in the same room.
static bool s_fix_discarded = false;

// ---- UART RX RING ----------------------------------------------------------
// The GPS talks 38400 8N1 (LilyGoWatchUltra.cpp, POWER_GPS case) = 3840 bytes/s.
// The ESP32 core defaults HardwareSerial to a 256-byte RX ring, which is 67 ms
// of NMEA. instance.gps.loop() is pumped exactly once per main-loop iteration
// (main.cpp), and this firmware's own comments record iterations of "hundreds of
// ms" under wardriver load and a ~250 ms block inside start_wardriving(). Any
// iteration longer than the ring drops bytes mid-sentence; TinyGPSPlus then
// rejects the sentence on checksum, and with enough loss a module that HAS a fix
// never lands a parseable one inside gps_fresh()'s 5 s window. That reads on
// screen as "GPS never locked".
//
// 2048 bytes = 533 ms of tolerance, which covers the stalls documented above at
// 2 KB of internal DRAM. Deliberately not larger: the ring is an internal-DRAM
// allocation and this build is sensitive to the largest contiguous internal
// block (see tasks/COEXIST-NOTES.md). If the health log below still shows
// checksum failures climbing, raise this before adding pump calls to hot paths.
//
// setRxBufferSize() is a no-op (and logs an error) while the driver is
// installed, so it MUST sit between Serial1.end() and the begin() that
// powerControl() runs. gps_uart_up() is the only sanctioned ordering.
static const size_t GPS_RX_RING_BYTES = 2048;

// Bring the GPS rail and its UART up together.
//
// powerControl(POWER_GPS,false) gpio_reset_pin()s the UART pins but never calls
// Serial1.end(), so the Serial1.begin() it runs on the next enable re-inits a
// stale driver and RX stays detached: no NMEA, never locks. Tearing the UART
// down explicitly makes each enable's begin() start clean, and gives us the one
// window in which the RX ring can be resized.
static void gps_uart_up()
{
    Serial1.end();
    Serial1.setRxBufferSize(GPS_RX_RING_BYTES);   // must precede begin()
    instance.powerControl(POWER_GPS, true);       // enableBLDO1 + Serial1.begin
}

// ---- SATELLITES IN VIEW ----------------------------------------------------
// GSV accumulator. TinyGPSPlus does not parse GSV at all, so before this the
// only satellite figure available was GGA's used-in-fix count - see the
// CORRECTED note in gps_health.h for what that cost.
static GpsGsv s_gsv;

// Drain the GPS UART into BOTH parsers.
//
// This replaces instance.gps.loop(), which is inline in the LilyGoLib GPS class
// and reads the port itself, giving no way to tee the stream. Reading the port
// here instead keeps the library untouched and preserves the property the RX
// ring note above depends on: the port is drained exactly once per main-loop
// iteration, from one place.
//
// encode() still drives every counter the health log reports (charsProcessed,
// passedChecksum, failedChecksum, sentencesWithFix), so nothing downstream
// changes.
void gps_screen_pump()
{
    if (!gps_powered) return;
    const uint32_t now = millis();
    while (Serial1.available()) {
        const int c = Serial1.read();
        if (c < 0) break;
        instance.gps.encode((char)c);
        s_gsv.feed((char)c, now);
    }
}

// Persist the radio on/off state so GPS survives a reboot. Mirrors the
// small-file pattern used by timezone.cpp / settings_screen.cpp.
#define GPS_PATH "/Settings/gps.txt"

static void gps_save_power(bool on)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return; // host owns SD when mounted
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(GPS_PATH, FILE_WRITE);   // FILE_WRITE = truncate
    if (!f) return;
    f.printf("%d\n", on ? 1 : 0);
    f.close();
}

// TinyGPSPlus has no reset; after a power cycle it keeps the previous session's
// values with isValid()==true until fresh NMEA overwrites them (a cold restart
// can take 30s+). Gate on commit age so stale data reads as "no fix" — this also
// stops the RTC being re-synced to the last session's timestamp. GPS streams
// ~1 Hz, so a live element is updated well within this window.
static const uint32_t GPS_FRESH_MS = 5000;

template <typename T>
static bool gps_fresh(const T &elem)
{
    return elem.isValid() && elem.age() < GPS_FRESH_MS;
}

// ---- STABLE LOCK (debounced) ------------------------------------------------
// gps_screen_has_lock() below is an INSTANTANEOUS predicate: one stale NMEA
// sample or a routine satellite dip reads as "no fix" for that tick. That is
// correct for the UI (show the truth right now), but wrong for the detection
// consumers, which need to know whether we HAVE a position, not whether this
// particular millisecond was clean.
//
// The 2026-07-30 field run logged 26 lock drops / 25 re-acquires, several only
// 1-2 s apart. Each one made ble_detect_pipeline publish cell = -1, the tail
// detector's "unknown location" sentinel, punching holes in the geo-cell trail
// the follow classifier is built on.
//
// So: rising edge is INSTANT (a fix is a fix), falling edge must persist for
// GPS_STABLE_DROP_MS before we believe it, and the satellite threshold has
// hysteresis (acquire at 4, hold down to 3) so a routine one-satellite dip does
// not drop the fix. Evaluated once per second from on_gps_update(); the accessor
// just reads the cached result.
//
// Consequence callers must know: while the drop is being debounced, the position
// this vouches for can be up to GPS_STABLE_DROP_MS old. That is a bounded,
// survey-grade staleness - unlike TinyGPSPlus's isValid(), which can hand back a
// fix from a PREVIOUS POWER CYCLE (see the GPS_FRESH_MS note above).
static const uint32_t GPS_STABLE_DROP_MS  = 10000;  // ride out sub-10s dropouts
static const uint32_t GPS_STABLE_SATS_ACQ = 4;      // sats needed to acquire
static const uint32_t GPS_STABLE_SATS_HLD = 3;      // sats needed to hold

static bool     s_stable_lock       = false;
static uint32_t s_unlocked_since_ms = 0;    // 0 = not currently timing a drop

// The raw condition behind the stable lock. Same fields as gps_screen_has_lock(),
// but the satellite floor depends on which way we are crossing it.
static bool gps_stable_raw()
{
    if (!gps_powered) return false;
    if (!gps_fresh(instance.gps.location))   return false;
    if (!gps_fresh(instance.gps.satellites)) return false;
    const uint32_t need = s_stable_lock ? GPS_STABLE_SATS_HLD : GPS_STABLE_SATS_ACQ;
    return instance.gps.satellites.value() >= need;
}

static void gps_stable_lock_update()
{
    // GPS powered off is a DELIBERATE loss, not a dropout - drop immediately so
    // nothing keeps vouching for a position after the user kills the radio.
    if (!gps_powered) {
        s_stable_lock       = false;
        s_unlocked_since_ms = 0;
        return;
    }

    if (gps_stable_raw()) {
        s_stable_lock       = true;   // rising edge: instant
        s_unlocked_since_ms = 0;
        return;
    }

    if (!s_stable_lock) return;       // already lost; nothing to debounce

    const uint32_t now = millis();
    if (s_unlocked_since_ms == 0) {
        s_unlocked_since_ms = now;    // start timing this drop
        return;
    }
    if (now - s_unlocked_since_ms >= GPS_STABLE_DROP_MS) {
        s_stable_lock       = false;  // sustained loss: believe it
        s_unlocked_since_ms = 0;
    }
}

// ---- GPS HEALTH (the discriminator) ----------------------------------------
// When the watch reports "no fix", exactly one of three things is true, and
// until now nothing on the device could tell them apart after the fact:
//
//   bytes/s ~= 0                    -> no NMEA at all. Rail, UART or wiring.
//   bytes/s healthy, bad climbing   -> byte loss. The RX ring overflowed (see
//                                      GPS_RX_RING_BYTES) or the main-loop pump
//                                      is starved. A FIRMWARE problem.
//   bytes/s healthy, bad flat,
//   sats > 0, fixes flat            -> the module is talking and sees
//                                      satellites but cannot resolve a fix.
//                                      RF: sky view, not firmware.
//
// TinyGPSPlus already counts all of it, so this only samples the deltas and puts
// them somewhere that survives a reboot. It is written to SD because that is the
// gap that made the 2026-08-03 failures undiagnosable: the run ended, nobody
// could recall what the screen had shown, and the device kept no record. The
// two rows added to the screen are the live view of the same numbers.
//
// Deliberately carries NO position data, so it is safe to read and quote whole.
#define GPS_HEALTH_PATH "/Settings/gpshealth.log"

// Heartbeat cadence while the radio is on. 30 s is frequent enough to catch a
// short outing (session 26 of the 2026-08-03 log was ~15 min and never locked)
// without the file becoming its own problem: ~85 bytes/line is ~250 KB/day of
// continuous GPS, hence the rotation cap below.
static const uint32_t GPS_HEALTH_PERIOD_MS = 30000;
static const uint32_t GPS_HEALTH_MAX_BYTES = 64u * 1024u;

static uint32_t s_health_last_ms     = 0;   // last heartbeat
static uint32_t s_health_last_chars  = 0;   // charsProcessed() at last sample
static uint32_t s_health_last_sample = 0;   // millis() at last sample
static uint32_t s_health_bytes_per_s = 0;   // latest computed rate
static uint32_t s_health_power_ms    = 0;   // millis() when the radio came up
static bool     s_health_last_stable = false;

// Baselines taken at power-on. TinyGPSPlus counters are cumulative for the life
// of the process, so every figure we report is scoped to THIS power cycle;
// otherwise a previous session's clean run would mask a bad one.
static uint32_t s_health_base_chars  = 0;
static uint32_t s_health_base_ok     = 0;
static uint32_t s_health_base_bad    = 0;
static uint32_t s_health_base_fix    = 0;

// Classified condition (see gps_health.h), so the UI and the WarDrive gate can
// say "no satellites for 3m12s" rather than "X".
static GpsHealth s_health_state    = GpsHealth::Off;
static uint32_t  s_health_quiet_ms = 0;   // when bps first fell below the floor

// The GSV inputs that produced s_health_state, cached alongside it.
//
// A log record must not contradict itself. These were read fresh inside
// gps_health_capture() at first, which is correct for a TICK (state is
// recomputed in the same call chain) but wrong for MODE and OFF, which are
// emitted the instant the event happens - up to a second after the last
// classification. The 2026-09-03 bring-up caught exactly that: a MODE line
// reading `view=1 cno=34 ... state=No sats`, which is three fields describing
// two different moments. A future investigation reading that line would draw
// the wrong conclusion about what the mode switch did.
//
// Deliberately NOT fixed by recomputing the state at capture time. That would
// regress the OFF record, which is emitted after gps_powered has already gone
// false and would therefore always classify as "Off" - throwing away the one
// thing that record exists to preserve, namely that the radio was LOCKED when
// the user switched it off.
static uint8_t   s_health_view     = 0;
static uint8_t   s_health_cno      = 0;

// TIME WITHOUT A FIX, which is deliberately NOT "time in the current state".
//
// The first version of this measured the latter and reset on every state
// change. Field-tested 2026-08-04 and it was useless: with satellites
// flickering 0 -> 1 -> 0 the classification flaps NoSatellites <-> Acquiring,
// so the counter kept zeroing and the user watched a timer that never climbed
// through a two-minute failure. The question being asked is always "how long
// has this been broken", so measure exactly that: reset only on an actual fix.
//
// Stable lock rather than the instantaneous one, so a 1-2 s blip does not
// silently restart a number the user is reading as elapsed failure time.
static uint32_t  s_nofix_since_ms  = 0;

// Called on every GPS power transition, so a fresh power cycle never inherits
// the previous one's counters or rate.
static void gps_health_reset()
{
    const uint32_t now = millis();
    s_health_power_ms    = now;
    s_health_last_ms     = 0;         // 0 = no heartbeat yet this cycle
    s_health_last_sample = now;
    s_health_last_chars  = instance.gps.charsProcessed();
    s_health_bytes_per_s = 0;
    s_health_last_stable = false;
    s_health_base_chars  = instance.gps.charsProcessed();
    s_health_base_ok     = instance.gps.passedChecksum();
    s_health_base_bad    = instance.gps.failedChecksum();
    s_health_base_fix    = instance.gps.sentencesWithFix();

    // Classify from the current inputs rather than assuming a starting state:
    // a hot start is already Locked on the first tick, and a power-off must
    // read Off immediately. quiet_sec is 0 here, so this can never open a power
    // cycle by declaring the receiver dead.
    s_health_quiet_ms = 0;
    s_gsv.reset();                 // a new cycle must not inherit the old sky
    s_health_view     = 0;
    s_health_cno      = 0;
    s_health_state    = gps_health_classify(
        gps_powered, gps_screen_has_lock(), 0, 0, 0, 0);
    s_nofix_since_ms  = now;   // a power cycle starts the clock on "no fix yet"
}

// ---- BACKLOG: THE INSTRUMENT USED TO REQUIRE WHAT IT MEASURES ---------------
// This log lives ON the SD card, so until now it could only ever observe the
// card-IN condition. Every session analysed on 2026-08-03 had the card in,
// because otherwise there was no log to read. That is a blind spot in exactly
// the wrong place: the reported symptom is "GPS takes longer to lock once the
// SD card is in", and wardriving is the one feature that needs the card and a
// fix at the SAME time.
//
// So records are captured whether or not a card is present, and buffered in RAM
// when it is not. main.cpp already polls card-detect and hot-mounts on
// insertion, so inserting the card mid-run flushes the card-OUT history to disk
// with its original timestamps intact. Run with no card, let GPS acquire, then
// insert the card and the whole acquisition is written retroactively.
//
// 48 records at 32 bytes is 1.5 KB of static DRAM (.bss, so it never fragments
// the heap the coexist work is sensitive to) and covers ~24 min at the 30 s
// heartbeat - comfortably longer than any acquisition worth measuring.
enum GpsHealthEvent : uint8_t {
    GHE_START = 0, GHE_TICK, GHE_LOCK, GHE_LOST, GHE_MODE, GHE_OFF
};
static const char *const kGpsEventName[] = { "START","TICK","LOCK","LOST","MODE","OFF" };

// Packed so the ring stays small. Counters are per power cycle, already
// baselined, so 32 bits each is ample.
struct GpsHealthRec {
    uint8_t  yy, mon, day, hh, mi, ss;   // local time, yy = year - 2000
    uint8_t  ev;
    uint8_t  state;
    uint8_t  mode;
    uint8_t  sats;       // GGA field 7: satellites USED in the fix
    uint8_t  view;       // GSV: satellites IN VIEW
    uint8_t  cno;        // GSV: strongest C/N0 in view, dB-Hz
    int8_t   ble;
    uint8_t  flags;                      // see GHF_* below
    uint16_t on_sec;
    uint16_t bps;
    uint32_t chars, ok, bad, fix;
};

static const uint8_t GHF_LOCK   = 1 << 0;
static const uint8_t GHF_STABLE = 1 << 1;
static const uint8_t GHF_WIFI   = 1 << 2;
static const uint8_t GHF_LORA   = 1 << 3;
static const uint8_t GHF_USB    = 1 << 4;
static const uint8_t GHF_CHG    = 1 << 5;
static const uint8_t GHF_CARD   = 1 << 6;   // was the card present when captured

static const uint8_t GPS_HEALTH_BACKLOG = 48;
static GpsHealthRec s_ring[GPS_HEALTH_BACKLOG];
// Aggregate-initialised at definition rather than in gps_screen_create(), so the
// capacity can never be 0 at the first push regardless of call order (slot()
// takes a modulo by it).
static GpsBacklog   s_backlog = { GPS_HEALTH_BACKLOG, 0, 0, 0 };

static bool gps_health_sd_usable()
{
    return instance.isCardReady() && !usb_sd_is_running();   // host owns SD
}

// Snapshot the live state. Saturating casts, so a long run or a wild byte rate
// truncates a FIELD rather than corrupting the record.
static void gps_health_capture(GpsHealthEvent ev, GpsHealthRec *r)
{
    struct tm t;
    clock_screen_get_local_time(&t);
    r->yy  = (uint8_t)((t.tm_year + 1900) % 100);
    r->mon = (uint8_t)(t.tm_mon + 1);
    r->day = (uint8_t)t.tm_mday;
    r->hh  = (uint8_t)t.tm_hour;
    r->mi  = (uint8_t)t.tm_min;
    r->ss  = (uint8_t)t.tm_sec;

    const uint32_t on  = (millis() - s_health_power_ms) / 1000u;
    const uint32_t sat = gps_fresh(instance.gps.satellites)
                       ? instance.gps.satellites.value() : 0;

    r->ev     = (uint8_t)ev;
    r->state  = (uint8_t)s_health_state;
    r->mode   = (uint8_t)argus_mode_current();
    r->sats   = (uint8_t)(sat > 255 ? 255 : sat);
    // From the cache, NOT read fresh - see s_health_view above.
    r->view   = s_health_view;
    r->cno    = s_health_cno;
    r->ble    = (int8_t)ble_scan_consumer_count();
    r->on_sec = (uint16_t)(on > 65535u ? 65535u : on);
    r->bps    = (uint16_t)(s_health_bytes_per_s > 65535u ? 65535u : s_health_bytes_per_s);

    r->flags = 0;
    if (gps_screen_has_lock())              r->flags |= GHF_LOCK;
    if (s_stable_lock)                      r->flags |= GHF_STABLE;
    if (wifi_radio_screen_is_powered())     r->flags |= GHF_WIFI;
    if (lora_screen_is_powered())           r->flags |= GHF_LORA;
    if (instance.pmu.isVbusIn())            r->flags |= GHF_USB;
    if (instance.pmu.isCharging())          r->flags |= GHF_CHG;
    if (gps_health_sd_usable())             r->flags |= GHF_CARD;

    r->chars = instance.gps.charsProcessed()   - s_health_base_chars;
    r->ok    = instance.gps.passedChecksum()   - s_health_base_ok;
    r->bad   = instance.gps.failedChecksum()   - s_health_base_bad;
    r->fix   = instance.gps.sentencesWithFix() - s_health_base_fix;
}

// Radio, power and CARD context on every line. The 2026-08-03 runs showed
// satellites collapsing to 0 while NMEA kept streaming, and the only account of
// what else was running at that moment was a recollection - which could not even
// establish when the watch had been on USB. Carrying it inline makes the
// correlation readable straight off the artifact: if satellites fall as
// ble/wifi/lora/usb/card come up, that is desense.
static void gps_health_print(File &f, const GpsHealthRec &r)
{
    static const char *const kModeName[] = { "daily", "defense", "offense" };
    f.printf("20%02u-%02u-%02u %02u:%02u:%02u on=%us %s bps=%u chars=%lu ok=%lu "
             "bad=%lu fix=%lu sats=%u view=%u cno=%u lock=%d stable=%d state=%s "
             "mode=%s ble=%d wifi=%d lora=%d usb=%d chg=%d card=%d\n",
             r.yy, r.mon, r.day, r.hh, r.mi, r.ss,
             r.on_sec,
             r.ev < 6 ? kGpsEventName[r.ev] : "?",
             r.bps,
             (unsigned long)r.chars, (unsigned long)r.ok,
             (unsigned long)r.bad,   (unsigned long)r.fix,
             r.sats, r.view, r.cno,
             (r.flags & GHF_LOCK)   ? 1 : 0,
             (r.flags & GHF_STABLE) ? 1 : 0,
             gps_health_label((GpsHealth)r.state),
             r.mode < 3 ? kModeName[r.mode] : "?",
             (int)r.ble,
             (r.flags & GHF_WIFI) ? 1 : 0,
             (r.flags & GHF_LORA) ? 1 : 0,
             (r.flags & GHF_USB)  ? 1 : 0,
             (r.flags & GHF_CHG)  ? 1 : 0,
             (r.flags & GHF_CARD) ? 1 : 0);
}

static void gps_health_buffer(const GpsHealthRec &r)
{
    s_ring[s_backlog.push()] = r;
}

static void gps_health_emit(GpsHealthEvent ev)
{
    GpsHealthRec rec;
    gps_health_capture(ev, &rec);

    if (!gps_health_sd_usable()) { gps_health_buffer(rec); return; }
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");

    // Rotate rather than grow without bound. Truncate-and-mark, not a ring: this
    // is a diagnostic, and the RECENT window is the one that matters.
    File probe = SD.open(GPS_HEALTH_PATH, FILE_READ);
    bool rotate = probe && probe.size() > GPS_HEALTH_MAX_BYTES;
    if (probe) probe.close();
    if (rotate) SD.remove(GPS_HEALTH_PATH);

    File f = SD.open(GPS_HEALTH_PATH, FILE_APPEND);
    // Card present but the open failed: buffer instead of discarding, so a
    // transient SD error costs nothing.
    if (!f) { gps_health_buffer(rec); return; }
    if (rotate) f.print("-- rotated (size cap) --\n");

    // Drain the card-out history first, oldest first, so the file stays in
    // chronological order. Say how many were dropped rather than truncating
    // silently - a backlog that quietly lost its head would misdate an
    // acquisition.
    if (s_backlog.count) {
        f.printf("-- backlog: %u record(s) buffered with no card, %u dropped --\n",
                 (unsigned)s_backlog.count, (unsigned)s_backlog.dropped);
        for (uint8_t i = 0; i < s_backlog.count; i++)
            gps_health_print(f, s_ring[s_backlog.slot(i)]);
        s_backlog.clear();
    }

    gps_health_print(f, rec);
    f.close();
}

// Sampled once per second from on_gps_update(), before the !gps_powered return.
static void gps_health_update()
{
    if (!gps_powered) return;

    const uint32_t now   = millis();
    const uint32_t chars = instance.gps.charsProcessed();

    // Byte rate over the interval actually elapsed, not an assumed 1 s: the
    // timer is LVGL-driven and slips under exactly the load we are hunting.
    const uint32_t dt = now - s_health_last_sample;
    if (dt >= 1000) {
        s_health_bytes_per_s = (uint32_t)(((uint64_t)(chars - s_health_last_chars) * 1000u) / dt);
        s_health_last_chars  = chars;
        s_health_last_sample = now;

        // Track how long the link has been silent, so NoData needs to persist
        // rather than firing on the one legitimately empty sample at power-on.
        if (s_health_bytes_per_s < kGpsNoDataFloorBps) {
            if (s_health_quiet_ms == 0) s_health_quiet_ms = now;
        } else {
            s_health_quiet_ms = 0;
        }
    }

    const uint32_t quiet_sec = s_health_quiet_ms ? (now - s_health_quiet_ms) / 1000u : 0;
    // One snapshot drives the classification AND the record, so a logged line
    // can never show inputs that disagree with the state they produced.
    s_health_view  = s_gsv.in_view(now);
    s_health_cno   = s_gsv.cno_max(now);
    const GpsHealth h = gps_health_classify(
        gps_powered, gps_screen_has_lock(), s_health_bytes_per_s,
        s_health_view, s_health_cno, quiet_sec);
    s_health_state = h;

    // Hold the no-fix clock at zero for as long as we HAVE a fix, so the moment
    // one is lost it starts counting from that loss, not from the power cycle.
    if (s_stable_lock) s_nofix_since_ms = now;

    // Log the first sample of a power cycle, every stable-lock edge, and a
    // heartbeat. The edges are what turn the file into a timeline of the run.
    bool           have  = true;
    GpsHealthEvent event = GHE_TICK;
    if (s_health_last_ms == 0)                      event = GHE_START;
    else if (s_stable_lock != s_health_last_stable) event = s_stable_lock ? GHE_LOCK : GHE_LOST;
    else if (now - s_health_last_ms >= GPS_HEALTH_PERIOD_MS) event = GHE_TICK;
    else                                            have  = false;
    if (!have) return;

    s_health_last_stable = s_stable_lock;
    s_health_last_ms     = now;
    gps_health_emit(event);
}

// Stamp the exact instant of a Daily/Defense/Offense switch. The 30 s heartbeat
// can straddle a switch, which is the whole difference between "GPS died when
// Defense came up" and "GPS died sometime in that half-minute".
static void gps_health_on_mode(ArgusMode)
{
    if (gps_powered) gps_health_emit(GHE_MODE);   // the line itself carries mode=
}

static void update_status()
{
    lv_label_set_text(status_label, gps_powered ? "Radio: ON" : "Radio: OFF");
}

static void on_toggle(lv_event_t *e)
{
    gps_powered = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    if (gps_powered) {
        s_fix_discarded = false;
        gps_uart_up();                           // rail + clean UART, sized ring
    } else {
        // Read the lock BEFORE tearing anything down: gps_stable_lock_update()
        // clears it on the next tick, so by then the fact that there was a fix
        // to discard is gone.
        s_fix_discarded = s_stable_lock;
        // Close the health timeline BEFORE tearing down, while the counters
        // still describe the cycle that just ended. Without this a run the user
        // switched off just stops mid-file, indistinguishable from a crash.
        gps_health_emit(GHE_OFF);
        instance.powerControl(POWER_GPS, false); // disableBLDO1 + reset pins
        Serial1.end();
    }
    gps_health_reset();
    clock_screen_set_gps_active(gps_powered);
    if (!gps_powered)
        rtc_synced = false; // allow re-sync on next power-on
    update_status();
    gps_save_power(gps_powered); // survive reboot
}

// Creates one key/value row in the scrollable data panel.
// The value label pointer is written to *val_out for later updates.
static void make_data_row(lv_obj_t *parent, const char *field, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_set_style_bg_color(row, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lbl, field);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(val, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    *val_out = val;
}

// Fires every second via lv_timer; reads TinyGPSPlus fields and updates labels.
static void on_gps_update(lv_timer_t *timer)
{
    char buf[32];
    bool screen_active = gps_screen_is_active();

    // Debounced lock state, refreshed every tick. Must run BEFORE the
    // !gps_powered early-return below so powering GPS off clears it.
    gps_stable_lock_update();

    // Health sampling runs off-screen too: the whole point is to have a record
    // of a run that failed while the user was looking at something else.
    // Must follow gps_stable_lock_update() so LOCK/LOST edges log this tick.
    gps_health_update();

    // GPS off path. Push 0 sats to the home indicator so the badge clears
    // even if the user toggled GPS off from this screen and immediately
    // went back home; only refresh the (hidden) labels when the screen
    // is actually loaded, since those are LVGL-cost-only.
    if (!gps_powered) {
        clock_screen_set_sat_count(0);
        if (screen_active) {
            lv_label_set_text(val_satellites, "--");
            lv_label_set_text(val_latitude,   "--");
            lv_label_set_text(val_longitude,  "--");
            lv_label_set_text(val_date,       "--");
            lv_label_set_text(val_gps_time,   "--");
            lv_label_set_text(val_altitude,   "--");
            lv_label_set_text(val_speed,      "--");
            lv_label_set_text(val_fix_age,    "--");
            lv_label_set_text(val_nmea,       "--");
            lv_label_set_text(val_csum,       "--");
            lv_label_set_text(val_status,
                              s_fix_discarded ? "Off - fix discarded"
                                              : gps_health_label(GpsHealth::Off));
        }
        return;
    }

    // Sync RTC + compute local UTC offset once per GPS power cycle.
    // Require a quality lock: valid position, date, time, year sanity, and ≥4 satellites.
    // Skipped entirely when the user has set the time by hand (manual override).
    // Run this even when off-screen - the user may enable GPS, leave for
    // home, and we still want the RTC to sync as soon as the fix lands.
    if (!rtc_synced
        && !clock_screen_manual_time_active()
        && gps_fresh(instance.gps.location)
        && gps_fresh(instance.gps.date)
        && gps_fresh(instance.gps.time)
        && instance.gps.date.year() > 2000
        && gps_fresh(instance.gps.satellites)
        && instance.gps.satellites.value() >= 4) {
        instance.rtc.setDateTime(
            instance.gps.date.year(),
            instance.gps.date.month(),
            instance.gps.date.day(),
            instance.gps.time.hour(),
            instance.gps.time.minute(),
            instance.gps.time.second()
        );
        instance.rtc.hwClockRead();
        // Base offset comes from longitude. North-American time zones
        // (UTC-5 Eastern through UTC-8 Pacific) get a +1 added when the
        // current date falls inside the US DST window. The check
        // ignores Arizona/Hawaii (no DST) and non-US countries that
        // happen to share those longitudes; users in those zones can
        // flip Manual Time to override.
        double gps_lat = instance.gps.location.lat();
        double gps_lon = instance.gps.location.lng();
        int base_off = utc_offset_from_longitude(gps_lon);
        int dst_bump = 0;
        if (base_off >= -8 && base_off <= -5
            && us_dst_active(instance.gps.date.year(),
                             instance.gps.date.month(),
                             instance.gps.date.day())) {
            // Arizona (UTC-7) does not observe DST. Suppress the bump when
            // the fix falls inside Arizona's bounding box. Navajo Nation
            // (NE corner of AZ) does observe DST but is a small area;
            // those users can use Manual Time if precision matters.
            bool in_arizona = (base_off == -7)
                           && (gps_lat >= 31.3 && gps_lat <= 37.0)
                           && (gps_lon >= -114.83 && gps_lon <= -109.05);
            if (!in_arizona) dst_bump = 1;
        }
        clock_screen_set_utc_offset(base_off + dst_bump);
        // A GPS fix is a real clock sync (UTC straight off the constellation),
        // so stamp it - that is what lets the watch later admit it has not been
        // synced in weeks instead of presenting a drifted RTC with confidence.
        timezone_note_synced(base_off + dst_bump, clocksync::Source::Gps);
        rtc_synced = true;
    }

    // Satellite count - push to the home-screen status indicator every
    // tick regardless of which screen is loaded. Without this, the home
    // badge only updated while the GPS screen itself was open.
    uint32_t sat_count = 0;
    if (gps_fresh(instance.gps.satellites)) {
        sat_count = instance.gps.satellites.value();
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)sat_count);
    } else {
        strcpy(buf, "--");
    }
    clock_screen_set_sat_count(sat_count);

    // Feed HexHound "new territory" XP when we enter a fresh coarse GPS cell.
    // Runs even off-screen (GPS may be on while the user is elsewhere); the pet
    // engine coarse-rounds and dedups, so a stationary fix won't farm XP.
    if (gps_fresh(instance.gps.location))
        hexhound_note_cell(instance.gps.location.lat(), instance.gps.location.lng());

    // From here down: pure GPS-screen UI labels. ~7 lv_label_set_text
    // calls per second + several gps.* field reads - skip when the user
    // isn't looking at this screen.
    if (!screen_active) return;

    // Used-in-fix AND in-view, because they are different quantities and the
    // difference is the diagnosis: on 2026-09-02 used fell 12 -> 5 while in-view
    // never moved, which is a signal-quality collapse rather than a lost sky.
    {
        const uint32_t now_ms = millis();
        const uint8_t  view   = s_gsv.in_view(now_ms);
        const uint8_t  cno    = s_gsv.cno_max(now_ms);
        char sats_line[40];
        if (view)
            snprintf(sats_line, sizeof(sats_line), "%s / %u in view %udB",
                     buf, (unsigned)view, (unsigned)cno);
        else
            snprintf(sats_line, sizeof(sats_line), "%s / none in view", buf);
        lv_label_set_text(val_satellites, sats_line);
    }

    // Location-derived fields (lat, lng, fix age share the same validity flag)
    if (gps_fresh(instance.gps.location)) {
        snprintf(buf, sizeof(buf), "%.5f", instance.gps.location.lat());
        lv_label_set_text(val_latitude, buf);
        snprintf(buf, sizeof(buf), "%.5f", instance.gps.location.lng());
        lv_label_set_text(val_longitude, buf);
        snprintf(buf, sizeof(buf), "%lu ms", instance.gps.location.age());
        lv_label_set_text(val_fix_age, buf);
    } else {
        lv_label_set_text(val_latitude,  "--");
        lv_label_set_text(val_longitude, "--");
        lv_label_set_text(val_fix_age,   "--");
    }

    // Date
    if (gps_fresh(instance.gps.date))
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
            instance.gps.date.year(), instance.gps.date.month(), instance.gps.date.day());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_date, buf);

    // Time (UTC)
    if (gps_fresh(instance.gps.time))
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
            instance.gps.time.hour(), instance.gps.time.minute(), instance.gps.time.second());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_gps_time, buf);

    // Altitude
    if (gps_fresh(instance.gps.altitude))
        snprintf(buf, sizeof(buf), "%.1f m", instance.gps.altitude.meters());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_altitude, buf);

    // Speed
    if (gps_fresh(instance.gps.speed))
        snprintf(buf, sizeof(buf), "%.1f km/h", instance.gps.speed.kmph());
    else
        strcpy(buf, "--");
    lv_label_set_text(val_speed, buf);

    // Status: the label alone once locked, label + dwell otherwise. The dwell is
    // what separates "just started looking" from "this is never going to work".
    if (s_health_state == GpsHealth::Locked) {
        lv_label_set_text(val_status, gps_health_label(s_health_state));
    } else {
        char dur[16];
        gps_health_duration(gps_screen_health_secs(), dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "%s %s", gps_health_label(s_health_state), dur);
        lv_label_set_text(val_status, buf);
    }

    // Health rows. Read these together when the fix will not come:
    // 0 B/s means no NMEA at all; a healthy rate with "bad" climbing means we
    // are losing bytes; a healthy rate with bad at 0 means the module is fine
    // and the sky is the problem. Figures are for this power cycle only.
    snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)s_health_bytes_per_s);
    lv_label_set_text(val_nmea, buf);

    snprintf(buf, sizeof(buf), "%lu ok / %lu bad",
             (unsigned long)(instance.gps.passedChecksum() - s_health_base_ok),
             (unsigned long)(instance.gps.failedChecksum() - s_health_base_bad));
    lv_label_set_text(val_csum, buf);
}

void gps_screen_create()
{
    gps_screen = lv_obj_create(NULL);
    tools_attach_jump_gesture(gps_screen);   // swipe-down -> Tools grid
    lv_obj_set_style_bg_color(gps_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(gps_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(gps_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "GPS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle (left of center)
    toggle_sw = lv_switch_create(gps_screen);
    lv_obj_set_ext_click_area(toggle_sw, 22);  // easier to hit
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle, vertically centred with it)
    status_label = lv_label_create(gps_screen);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);
    update_status();

    // Scrollable data panel — occupies the bottom portion of the screen
    lv_obj_t *data_panel = lv_obj_create(gps_screen);
    lv_obj_set_size(data_panel, 380, 291);
    lv_obj_align(data_panel, LV_ALIGN_BOTTOM_MID, 0, -75);
    lv_obj_set_style_bg_color(data_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(data_panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(data_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(data_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_panel, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(data_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(data_panel, LV_SCROLLBAR_MODE_AUTO);
    // Flex column layout stacks rows and handles overflow scrolling
    lv_obj_set_layout(data_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(data_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(data_panel, 0, LV_PART_MAIN);

    // Status first: it is the row that answers "why is there no fix", so it
    // must be visible without scrolling.
    make_data_row(data_panel, "Status",     &val_status);
    make_data_row(data_panel, "Satellites", &val_satellites);
    make_data_row(data_panel, "Latitude",   &val_latitude);
    make_data_row(data_panel, "Longitude",  &val_longitude);
    make_data_row(data_panel, "Date",       &val_date);
    make_data_row(data_panel, "Time",       &val_gps_time);
    make_data_row(data_panel, "Altitude",   &val_altitude);
    make_data_row(data_panel, "Speed",      &val_speed);
    make_data_row(data_panel, "Fix Age",    &val_fix_age);
    make_data_row(data_panel, "NMEA",       &val_nmea);
    make_data_row(data_panel, "Checksum",   &val_csum);

    lv_timer_create(on_gps_update, 1000, NULL);
    argus_mode_on_change(gps_health_on_mode);
}

void gps_screen_show()
{
    lv_scr_load(gps_screen);
}

bool gps_screen_is_active()
{
    return lv_screen_active() == gps_screen;
}

bool gps_screen_is_powered()
{
    return gps_powered;
}

bool gps_screen_has_lock()
{
    return gps_powered
        && gps_fresh(instance.gps.location)
        && gps_fresh(instance.gps.satellites)
        && instance.gps.satellites.value() >= 4;
}

// Debounced sibling of gps_screen_has_lock(). See the STABLE LOCK block near
// gps_fresh() for why the detection/survey consumers use this instead.
bool gps_screen_has_stable_lock()
{
    return s_stable_lock;
}

// WHY there is no fix, and for how long. See the GPS HEALTH block above.
GpsHealth gps_screen_health()
{
    return s_health_state;
}

uint32_t gps_screen_health_secs()
{
    return (millis() - s_nofix_since_ms) / 1000u;
}

// Boot-time power-on: bring GPS up the same way on_toggle does and reflect it on
// the switch. Called from setup() ONLY when the user opted GPS into "Enable at
// boot" (boot_prefs), so the enable decision lives in the caller and there is no
// per-radio file check here. Must run after the SD card is mounted
// (instance.begin) and after gps_screen_create().
void gps_screen_restore_power()
{
    // Power GPS on the same way on_toggle does. See gps_uart_up() for why the
    // UART is torn down and resized before powerControl re-inits it.
    gps_uart_up();
    gps_powered = true;
    rtc_synced  = false;
    gps_health_reset();
    clock_screen_set_gps_active(true);
    update_status();

    // Reflect on the toggle switch if the screen was already created.
    if (toggle_sw)
        lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
}
