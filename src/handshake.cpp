#include "handshake.h"
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
    s_running  = true;
    return true;
}

void handshake_stop()
{
    if (!s_running) return;
    s_running = false;
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
    (void)rssi; (void)ch;
    if (!s_running || len < 36) return;

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
