#include "handshake.h"
#include "handshake_channel.h"   // pure census + pick, covered by the host suite
#include "wifi_beacon_manager.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "freertos/queue.h"

void clock_screen_get_local_time(struct tm *out);   // defined in main.cpp

#define HS_MAXLEN 256        // EAPOL frames are small; cap the snap length
struct HsFrame { uint16_t len; uint8_t data[HS_MAXLEN]; };

static QueueHandle_t s_queue   = nullptr;
static volatile bool s_running = false;
static int           s_pwnd    = 0;

// PWND is counted once per AP; deduped by BSSID. Touched only from the WiFi
// task (handshake_rx_data), like the detector tables — no locking.
#define HS_SEEN 24
static uint8_t s_seen[HS_SEEN][6];
static int     s_seen_n = 0;

static char s_path[40] = "";
static bool s_hdr_done = false;

// A no-op beacon consumer purely to refcount the WiFi promiscuous radio on.
static void hs_wifi_noop(const WifiBeacon *b) { (void)b; }

// --- Channel selection -------------------------------------------------------
// The shared scan hops 1-13 on a 200 ms timer, so a full sweep is 2.6 s and the
// radio sits on any one channel ~7.7% of the time. An AP beacons every ~100 ms
// so surveying works fine through that, but a WPA 4-way handshake completes in
// tens of milliseconds: hop off and it is gone. Passive capture was therefore
// missing most handshakes even when one happened in front of it.
//
// Fix: SURVEY while hopping, counting DATA frames per channel, then PIN to the
// channel actually carrying client traffic. Re-survey periodically so a pin
// that has gone stale (the client left, the AP moved channel) self-corrects.
//
// Counting DATA frames rather than APs is deliberate: we want the channel where
// clients are talking, which is where a (re)association and its handshake will
// happen. The busiest-by-AP-count channel can easily have no client activity.
enum HsPhase { HS_SURVEY, HS_PINNED };
static HsPhase  s_phase      = HS_SURVEY;
static uint32_t s_phase_ms   = 0;
static uint8_t  s_pin_target = 0;    // channel we asked for; 0 = none

#define HS_SURVEY_MS  10000u   // ~4 full hop sweeps before deciding
#define HS_PIN_MS    180000u   // re-survey every 3 min so a stale pin corrects

// The census and the pick live in handshake_channel.h, hardware-free, so the
// decision is covered by test/test_handshake_channel.cpp. This file cannot be in
// the host suite's MODULES (Arduino/SD/FreeRTOS), so anything left inline here
// is untestable by construction - see argus-host-suite-blind-spot.
//
// Written by the WiFi task (observe) and read/reset from loop(). Same lock-free
// deal as s_seen[] above: a torn read, or a count lost to a concurrent clear,
// only perturbs a heuristic argmax and cannot corrupt anything.
static HsChannelCensus s_census;

static void hs_channel_tick()
{
    if (!s_running) return;
    uint32_t now = millis();

    if (s_phase == HS_SURVEY) {
        if ((uint32_t)(now - s_phase_ms) < HS_SURVEY_MS) return;

        // best() returns 0 for "do not pin" on a silent census. That is the
        // case that matters: a plain argmax over an all-zero array is channel 1
        // and means nothing, so pinning would park capture on an arbitrary dead
        // channel - strictly worse than hopping, which would at least cross a
        // live one eventually. Pinned by test_handshake_channel.cpp.
        uint8_t best = s_census.best();
        if (best == 0) {
            s_phase_ms = now;   // stay hopping, survey again
            return;
        }
        s_pin_target = best;
        wifi_beacon_pin_channel(s_pin_target);
        s_phase      = HS_PINNED;
        s_phase_ms   = now;
        return;
    }

    // PINNED. Re-assert: the manager drops the pin when the scan is torn down
    // (see stop_wifi), so without this a WiFi bounce would silently leave us
    // hopping while this module still believed it was parked.
    if (s_pin_target && wifi_beacon_pinned_channel() != s_pin_target)
        wifi_beacon_pin_channel(s_pin_target);

    if ((uint32_t)(now - s_phase_ms) >= HS_PIN_MS) {
        wifi_beacon_unpin();
        s_pin_target = 0;
        s_census.clear();
        s_phase    = HS_SURVEY;
        s_phase_ms = now;
    }
}

uint8_t handshake_capture_channel() { return s_phase == HS_PINNED ? s_pin_target : 0; }

bool handshake_is_running() { return s_running; }
int  handshake_pwnd_count() { return s_pwnd; }

bool handshake_start()
{
    if (s_running) return true;
    if (!s_queue) {
        s_queue = xQueueCreate(8, sizeof(HsFrame));
        if (!s_queue) return false;
    }
    if (!wifi_beacon_add(hs_wifi_noop)) return false;   // promiscuous on
    wifi_beacon_set_data_capture(true);                 // widen filter to DATA

    struct tm t;
    clock_screen_get_local_time(&t);
    snprintf(s_path, sizeof(s_path), "/pwn/%04d%02d%02d-%02d%02d%02d.pcap",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    s_hdr_done = false;
    s_seen_n   = 0;
    s_census.clear();
    s_phase      = HS_SURVEY;   // always re-survey on arm; the RF world moves
    s_phase_ms   = millis();
    s_pin_target = 0;
    s_running    = true;
    return true;
}

void handshake_stop()
{
    if (!s_running) return;
    s_running = false;
    // Release the pin BEFORE dropping the consumer. The manager clears the pin
    // on teardown anyway, but this module must never be the reason another
    // consumer inherits a parked channel it did not ask for.
    wifi_beacon_unpin();
    s_pin_target = 0;
    s_phase      = HS_SURVEY;
    wifi_beacon_set_data_capture(false);
    wifi_beacon_remove(hs_wifi_noop);
}

static bool seen_bssid(const uint8_t *b)
{
    for (int i = 0; i < s_seen_n; i++)
        if (memcmp(s_seen[i], b, 6) == 0) return true;
    if (s_seen_n < HS_SEEN) { memcpy(s_seen[s_seen_n++], b, 6); }
    return false;
}

void handshake_rx_data(const uint8_t *frame, int len, int8_t rssi, uint8_t ch)
{
    (void)rssi;
    if (!s_running || len < 36) return;

    // Per-channel DATA-frame census, drives the SURVEY -> PIN decision above.
    // Saturating, so a long busy session cannot wrap the counter and hand the
    // argmax to a quiet channel.
    s_census.observe(ch);

    uint8_t fc0 = frame[0];
    if ((fc0 & 0x0C) != 0x08) return;                 // not a data frame
    int hdr = ((fc0 & 0xF0) == 0x80) ? 26 : 24;       // QoS data carries +2 bytes
    if (len < hdr + 8 + 4) return;

    const uint8_t *llc = frame + hdr;                 // LLC/SNAP header
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03)) return;
    if (!(llc[6] == 0x88 && llc[7] == 0x8E)) return;  // EtherType 0x888E = EAPOL

    // A handshake / PMKID frame. addr3 (the BSSID) sits at offset 16.
    if (!seen_bssid(frame + 16)) s_pwnd++;

    if (!s_queue) return;
    HsFrame f;
    f.len = (len > HS_MAXLEN) ? HS_MAXLEN : (uint16_t)len;
    memcpy(f.data, frame, f.len);
    xQueueSend(s_queue, &f, 0);                        // SD write happens on main task
}

// pcap global header for DLT_IEEE802_11 (link type 105), little-endian.
static void write_global_header(File &fp)
{
    uint8_t h[24] = {
        0xD4, 0xC3, 0xB2, 0xA1,   // magic 0xa1b2c3d4
        0x02, 0x00, 0x04, 0x00,   // version 2.4
        0, 0, 0, 0,               // thiszone
        0, 0, 0, 0,               // sigfigs
        0xFF, 0xFF, 0x00, 0x00,   // snaplen 65535
        105,  0,    0,    0       // network = 105 (IEEE 802.11)
    };
    fp.write(h, sizeof(h));
}

void handshake_bg_tick()
{
    // FIRST: this must run even when nothing has been captured. The queue drain
    // below returns early when idle, which is precisely the state we are trying
    // to get out of, so putting the channel machine after it would mean a watch
    // that has caught nothing never pins and therefore keeps catching nothing.
    hs_channel_tick();

    if (!s_queue) return;
    HsFrame f;
    if (xQueueReceive(s_queue, &f, 0) != pdTRUE) return;

    if (!instance.isCardReady()) return;
    if (!SD.exists("/pwn")) SD.mkdir("/pwn");

    File fp = SD.open(s_path, FILE_APPEND);
    if (!fp) return;
    if (!s_hdr_done) {
        if (fp.size() == 0) write_global_header(fp);
        s_hdr_done = true;
    }

    uint32_t ms   = millis();
    uint32_t sec  = ms / 1000;
    uint32_t usec = (ms % 1000) * 1000;
    uint32_t l    = f.len;
    uint8_t  rec[16];
    memcpy(rec + 0, &sec,  4);    // ESP32 is little-endian — matches pcap
    memcpy(rec + 4, &usec, 4);
    memcpy(rec + 8, &l,    4);
    memcpy(rec + 12, &l,   4);
    fp.write(rec, 16);
    fp.write(f.data, f.len);
    fp.close();
}
