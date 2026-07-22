#include "ble_detect_pipeline.h"

#include <Arduino.h>        // FreeRTOS portMUX + millis (via the ESP32 core)
#include <LilyGoLib.h>      // instance (GPS)
#include <esp_gap_ble_api.h>

#include "ble_scan_manager.h"       // ble_scan_add / _remove / _consumer_count
#include "gps_screen.h"             // gps_screen_has_lock()
#include "geo_cell.h"               // geo::coarse_cell()
#include "detect_pipeline.h"        // detect_pipeline_feed_tracker() -> shared ThreatState

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

// Guards the TailDetector across the callback (BT task) and the tick (main task).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Whether our scan consumer is currently attached (piggyback state).
static bool s_registered = false;

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
    if (!detect::is_unwanted_tracker(res.ble_adv, (size_t)len)) return;   // GATE

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

    // Log only escalations (PossibleTail=3 / ConfirmedTail=4) to avoid spam.
    if (static_cast<uint8_t>(v.flag) >= 3) {
        BLD_LOG("[bledetect] tracker follow flag=%u cells=%u span=%us cell=%ld\n",
                (unsigned)v.flag, (unsigned)v.distinct_cells,
                (unsigned)v.span_sec_over_60 * 60u, (long)s.cell_id);
    }
}

// --- 1Hz pipeline tick (main/LVGL task context) -----------------------------
void ble_detect_pipeline_tick(uint32_t now_sec)
{
    s_now_sec = now_sec;

    // Publish the coarse GPS cell for the (locationless) callback. Read GPS here
    // on the main task; geo::coarse_cell uses <cmath>, unsafe in the BT callback.
    if (gps_screen_has_lock() && instance.gps.location.isValid())
        s_cell_id = geo::coarse_cell(instance.gps.location.lat(),
                                     instance.gps.location.lng());
    else
        s_cell_id = -1;

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
    } else if (others <= 0 && s_registered) {
        ble_scan_remove(ble_detect_cb);
        s_registered = false;
        BLD_LOG("[bledetect] piggyback detached\n");
    }

    // Age the follow tables. The shared ThreatState is ticked + UI/log-driven by
    // detect_pipeline_tick() (which iterates ALL domains including Airtag), so
    // nothing else to do here.
    portENTER_CRITICAL(&s_mux);
    s_tracker_tail.decay(now_sec);
    portEXIT_CRITICAL(&s_mux);
}
