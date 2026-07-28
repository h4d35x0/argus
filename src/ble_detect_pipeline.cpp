#include "ble_detect_pipeline.h"

#include <Arduino.h>        // FreeRTOS portMUX + millis (via the ESP32 core)
#include <LilyGoLib.h>      // instance (GPS)
#include <esp_gap_ble_api.h>

#include "ble_scan_manager.h"       // ble_scan_add / _remove / _consumer_count
#include "gps_screen.h"             // gps_screen_has_lock()
#include "geo_cell.h"               // geo::coarse_cell()
#include "detect_pipeline.h"        // detect_pipeline_feed_tracker() -> shared ThreatState
#include "usb_sd.h"                 // usb_sd_is_running()

#include "detect/tracker_ident.h"   // is_unwanted_tracker()
#include "detect/tail_detect.h"     // TailDetector, DeviceSighting, TailVerdict

// Bench observability for the on-device bring-up test (piggyback attach + follow
// verdicts). Set to 0 once verified. No PII: only threat-state ints.
#define ARGUS_BLE_DETECT_DEBUG 1
#if ARGUS_BLE_DETECT_DEBUG
  #define BLD_LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define BLD_LOG(...) ((void)0)
#endif

// --- Owned pure state -------------------------------------------------------
// The BLE side owns only the TailDetector (the follow classifier); the shared
// ThreatState / log / UI live in detect_pipeline and are fed via
// detect_pipeline_feed_tracker(), so BLE + WiFi drive ONE posture.
static detect::TailDetector s_tracker_tail;

// The scan callback runs in the BT task and has no clock or GPS of its own; the
// 1Hz tick publishes both here for it to read (main task reads GPS - geo_cell
// pulls in <cmath>, which must not run in the callback).
static volatile uint32_t s_now_sec = 0;
static volatile int32_t  s_cell_id = -1;   // coarse GPS cell, -1 = no fix
static geo::StableCellTracker s_stable_cell;

// Guards the TailDetector across the callback (BT task) and the tick (main task).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Whether our scan consumer is currently attached (piggyback state).
static bool s_registered = false;

#if ARGUS_BLE_DETECT_DEBUG
#include <cstdarg>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Debug-only field-test counters. They make a silent serial log meaningful:
// total proves the callback is receiving adverts, recognized proves the tracker
// gate matched, and accepted proves the owner-state gate admitted a sighting.
static volatile uint32_t s_debug_total_adverts = 0;
static volatile uint32_t s_debug_recognized = 0;
static volatile uint32_t s_debug_accepted = 0;
static uint32_t s_debug_last_stats_sec = 0;
static bool s_debug_session_started = false;

#define BLE_DETECT_LOG_PATH "/Settings/bledetect.log"

struct DebugTrackState {
    bool used;
    uint32_t device_id;
    uint8_t flag;
    uint8_t cells;
    uint16_t minutes;
};
static DebugTrackState s_debug_tracks[8] = {};

struct DebugVerdictEvent {
    uint32_t t_sec;
    uint32_t device_id;
    uint8_t flag;
    uint8_t cells;
    uint16_t span_sec;
    int32_t cell_id;
    int8_t rssi;
};
static QueueHandle_t s_debug_event_queue = nullptr;

// SD writes stay on the main task. The BLE callback only enqueues fixed-size
// events, so field logging cannot block the controller task.
static void debug_sd_printf(const char *fmt, ...)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");

    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    File f = SD.open(BLE_DETECT_LOG_PATH, FILE_APPEND);
    if (!f) return;
    f.println(line);
    f.close();
}

static void debug_log_verdict(uint32_t device_id,
                              const detect::TailVerdict &v,
                              int32_t cell_id,
                              int8_t rssi,
                              uint32_t t_sec)
{
    DebugTrackState *slot = nullptr;
    for (size_t i = 0; i < sizeof(s_debug_tracks) / sizeof(s_debug_tracks[0]); ++i) {
        if (s_debug_tracks[i].used && s_debug_tracks[i].device_id == device_id) {
            slot = &s_debug_tracks[i];
            break;
        }
        if (!slot && !s_debug_tracks[i].used) slot = &s_debug_tracks[i];
    }
    if (!slot) slot = &s_debug_tracks[device_id % 8u];

    const uint8_t flag = static_cast<uint8_t>(v.flag);
    const bool changed = !slot->used || slot->device_id != device_id ||
                         slot->flag != flag || slot->cells != v.distinct_cells ||
                         slot->minutes != v.span_sec_over_60;
    if (!changed) return;

    slot->used = true;
    slot->device_id = device_id;
    slot->flag = flag;
    slot->cells = v.distinct_cells;
    slot->minutes = v.span_sec_over_60;
    BLD_LOG("[bledetect] verdict id=%08lx flag=%u cells=%u span=%us cell=%ld rssi=%d\n",
            (unsigned long)device_id, (unsigned)flag,
            (unsigned)v.distinct_cells,
            (unsigned)v.span_sec_over_60 * 60u,
            (long)cell_id, (int)rssi);

    if (s_debug_event_queue) {
        DebugVerdictEvent event = {
            t_sec,
            device_id,
            flag,
            v.distinct_cells,
            static_cast<uint16_t>(v.span_sec_over_60 * 60u),
            cell_id,
            rssi,
        };
        xQueueSend(s_debug_event_queue, &event, 0);
    }
}
#endif

// Fold a 6-byte BLE MAC into a 32-bit device_id (FNV-1a). BLE addresses rotate,
// but a rotating address that keeps reappearing across GPS cells is exactly the
// follow signal the TailDetector scores; the fold just gives ingest() a key.
static uint32_t fold_mac(const uint8_t bda[6])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; ++i) { h ^= bda[i]; h *= 16777619u; }
    return h;
}

// --- Scan callback (BT task context) ----------------------------------------
// Tiny and non-blocking: gate the advert, build one sighting, ingest, and report
// the follow verdict to the shared aggregator. No SD, no LVGL, no GPS, no alloc.
static void ble_detect_cb(esp_ble_gap_cb_param_t *param)
{
    if (!param) return;
    auto &res = param->scan_rst;
    int len = (int)res.adv_data_len + (int)res.scan_rsp_len;   // as airtag.cpp
    if (len <= 0) return;

#if ARGUS_BLE_DETECT_DEBUG
    s_debug_total_adverts++;
#endif

    // Identify once and apply the same conservative gate as
    // is_unwanted_tracker(): reject non-trackers and positively OwnerNearby
    // trackers, accept Separated or Unknown owner state.
    const detect::TrackerId id =
        detect::identify_tracker(res.ble_adv, (size_t)len);
    if (id.kind == detect::TrackerKind::None) return;
#if ARGUS_BLE_DETECT_DEBUG
    s_debug_recognized++;
#endif
    if (id.status == detect::TrackerStatus::OwnerNearby) return;
#if ARGUS_BLE_DETECT_DEBUG
    s_debug_accepted++;
#endif

    detect::DeviceSighting s;
    s.device_id = fold_mac(res.bda);
    s.t_sec     = s_now_sec;
    s.cell_id   = s_cell_id;
    s.rssi      = (int8_t)res.rssi;

    portENTER_CRITICAL(&s_mux);
    detect::TailVerdict v = s_tracker_tail.ingest(s);
    portEXIT_CRITICAL(&s_mux);

    // Report into the shared ThreatState (its own lock, taken outside ours).
    detect_pipeline_feed_tracker(v.flag, s_now_sec);

#if ARGUS_BLE_DETECT_DEBUG
    // Log the first accepted sighting and every flag/cell/minute transition.
    // This captures None, Familiar, Watching, Possible, and Confirmed without
    // printing every advertisement.
    debug_log_verdict(s.device_id, v, s.cell_id, s.rssi, s.t_sec);
#endif
}

// --- 1Hz pipeline tick (main/LVGL task context) -----------------------------
void ble_detect_pipeline_tick(uint32_t now_sec)
{
    s_now_sec = now_sec;

#if ARGUS_BLE_DETECT_DEBUG
    if (!s_debug_event_queue)
        s_debug_event_queue = xQueueCreate(16, sizeof(DebugVerdictEvent));
    if (!s_debug_session_started &&
        instance.isCardReady() && !usb_sd_is_running()) {
        debug_sd_printf("SESSION boot_sec=%lu debug=1",
                        (unsigned long)now_sec);
        s_debug_session_started = true;
    }
#endif

    // Publish the coarse GPS cell for the (locationless) callback. Read GPS here
    // on the main task; geo::coarse_cell uses <cmath>, unsafe in the BT callback.
    int32_t next_cell = -1;
    if (gps_screen_has_lock() && instance.gps.location.isValid())
        next_cell = s_stable_cell.update(instance.gps.location.lat(),
                                         instance.gps.location.lng());
    if (next_cell != s_cell_id) {
        s_cell_id = next_cell;
        BLD_LOG("[bledetect] gps lock=%d cell=%ld\n",
                (int)(next_cell >= 0), (long)next_cell);
#if ARGUS_BLE_DETECT_DEBUG
        debug_sd_printf("GPS t=%lu lock=%d cell=%ld",
                        (unsigned long)now_sec,
                        (int)(next_cell >= 0), (long)next_cell);
#endif
    }

    // PIGGYBACK activation: attach our consumer ONLY while some OTHER BLE consumer
    // (AirTag / Flipper / Skimmer / wardriver) already has the controller up, so
    // we are never the FIRST consumer and never trigger bring_up_controller() /
    // esp_bt_controller_enable() (which hangs/boot-loops with WiFi holding SRAM).
    // Detach as soon as no other consumer remains, so we never hold BLE up alone.
    int others = ble_scan_consumer_count() - (s_registered ? 1 : 0);
    if (others > 0 && !s_registered) {
        s_registered = ble_scan_add(ble_detect_cb);
        BLD_LOG("[bledetect] piggyback ATTACHED (others=%d) ok=%d\n",
                others, (int)s_registered);
#if ARGUS_BLE_DETECT_DEBUG
        debug_sd_printf("SCAN t=%lu attached=1 others=%d ok=%d",
                        (unsigned long)now_sec, others, (int)s_registered);
#endif
    } else if (others <= 0 && s_registered) {
        ble_scan_remove(ble_detect_cb);
        s_registered = false;
        BLD_LOG("[bledetect] piggyback detached\n");
#if ARGUS_BLE_DETECT_DEBUG
        debug_sd_printf("SCAN t=%lu attached=0",
                        (unsigned long)now_sec);
#endif
    }

#if ARGUS_BLE_DETECT_DEBUG
    if (s_registered &&
        (s_debug_last_stats_sec == 0 || now_sec - s_debug_last_stats_sec >= 30u)) {
        s_debug_last_stats_sec = now_sec;
        BLD_LOG("[bledetect] stats adverts=%lu recognized=%lu accepted=%lu cell=%ld\n",
                (unsigned long)s_debug_total_adverts,
                (unsigned long)s_debug_recognized,
                (unsigned long)s_debug_accepted,
                (long)s_cell_id);
        debug_sd_printf("STATS t=%lu adverts=%lu recognized=%lu accepted=%lu cell=%ld",
                        (unsigned long)now_sec,
                        (unsigned long)s_debug_total_adverts,
                        (unsigned long)s_debug_recognized,
                        (unsigned long)s_debug_accepted,
                        (long)s_cell_id);
    }

    if (s_debug_event_queue &&
        instance.isCardReady() && !usb_sd_is_running()) {
        DebugVerdictEvent event;
        while (xQueueReceive(s_debug_event_queue, &event, 0) == pdTRUE) {
            debug_sd_printf(
                "VERDICT t=%lu id=%08lx flag=%u cells=%u span=%us cell=%ld rssi=%d",
                (unsigned long)event.t_sec,
                (unsigned long)event.device_id,
                (unsigned)event.flag,
                (unsigned)event.cells,
                (unsigned)event.span_sec,
                (long)event.cell_id,
                (int)event.rssi);
        }
    }
#endif

    // Age the follow tables. The shared ThreatState is ticked + UI/log-driven by
    // detect_pipeline_tick() (which iterates ALL domains including Airtag), so
    // nothing else to do here.
    portENTER_CRITICAL(&s_mux);
    s_tracker_tail.decay(now_sec);
    portEXIT_CRITICAL(&s_mux);
}
