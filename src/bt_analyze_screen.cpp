#include "bt_analyze_screen.h"
#include "theme.h"
#include "analyze_screen.h"
#include "lora_analyze_screen.h"
#include "ble_scan_manager.h"
#include <LilyGoLib.h>
#include <string.h>

// Defined in tools_screen.cpp
void tools_screen_show();

#define N_BUCKETS       8
// BAR_W trimmed 38→34, BAR_GAP 8→6 to keep the chart width inside the
// 410-diameter visible circle once CHART_BOTTOM_Y dropped to 375. With
// 8·34 + 7·6 = 314 px total, the outer corners at (48, 375) and
// (362, 375) sit ~200 px from the screen centre — just inside r=205.
#define BAR_W           34
#define BAR_GAP         6
// Chart geometry tuned to fit inside the watch's ~410-diameter visible
// circle. Pulling top/bottom in keeps the chart corners off the bezel.
#define CHART_BOTTOM_Y  375   // pushed down from 348 for ~27 px more
                              // bar height; the BAR_W/BAR_GAP shrink
                              // keeps the outer corners on the bezel.
#define CHART_TOP_Y     154

// Devices are aged out of the table this long after the last advertisement
// — a device that has gone away simply stops contributing to the bars.
#define BT_DEV_MAX      64
#define BT_DEV_TTL_MS   10000

static lv_obj_t *screen;
static lv_obj_t *title_label;
static lv_obj_t *status_label;
static lv_obj_t *legend_label;
static lv_obj_t *bars[N_BUCKETS];
static lv_obj_t *bucket_labels[N_BUCKETS];
static int       bar_x[N_BUCKETS];

struct BtDev {
    esp_bd_addr_t bda;
    int8_t        rssi;
    uint32_t      last_ms;
    bool          valid;
};

// Touched by the BT scan callback (Bluedroid task) on insert/update and by the
// main task in refresh() on read/age-out. No lock — the writes are small and
// the visualisation tolerates the occasional miscount of one. Done the same
// way the AirTag dedup table is.
static BtDev s_devs[BT_DEV_MAX];

static volatile uint32_t s_pkts          = 0;   // total advertisements seen
static uint32_t          s_pps_last_pkts = 0;
static uint32_t          s_last_tick_ms  = 0;
static float             s_pps_smoothed  = 0.0f;
static int               s_strongest_rssi = -127;
static esp_bd_addr_t     s_strongest_bda  = {};
static bool              s_running        = false;

// ---- bucket helpers --------------------------------------------------------

static int bucket_of(int rssi)
{
    if (rssi >= -30) return 0;
    if (rssi >= -40) return 1;
    if (rssi >= -50) return 2;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 4;
    if (rssi >= -80) return 5;
    if (rssi >= -90) return 6;
    return 7;
}

static lv_color_t bucket_colour(int b)
{
    // Strong-to-weak RSSI gradient — closer (left) is louder/redder.
    static const uint32_t cols[N_BUCKETS] = {
        0xFF4444, 0xFF8844, 0xFFCC00, 0xCCCC00,
        0x44CC44, 0x00CCCC, 0x4488FF, 0x6666AA,
    };
    uint32_t c = cols[b];
    return lv_color_make((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// ---- BLE callback (runs on Bluedroid task) ---------------------------------

static void scan_cb(esp_ble_gap_cb_param_t *param)
{
    if (!s_running) return;
    auto &r = param->scan_rst;
    int8_t rssi = (int8_t)r.rssi;
    s_pkts++;

    if (rssi > s_strongest_rssi) {
        s_strongest_rssi = rssi;
        memcpy(s_strongest_bda, r.bda, 6);
    }

    uint32_t now = millis();
    int      empty_slot = -1;
    int      oldest_slot = 0;
    uint32_t oldest_ms   = 0xFFFFFFFFu;

    for (int i = 0; i < BT_DEV_MAX; i++) {
        if (!s_devs[i].valid) {
            if (empty_slot < 0) empty_slot = i;
            continue;
        }
        if (memcmp(s_devs[i].bda, r.bda, 6) == 0) {
            s_devs[i].rssi    = rssi;
            s_devs[i].last_ms = now;
            return;
        }
        if (s_devs[i].last_ms < oldest_ms) {
            oldest_ms   = s_devs[i].last_ms;
            oldest_slot = i;
        }
    }
    int slot = (empty_slot >= 0) ? empty_slot : oldest_slot;
    memcpy(s_devs[slot].bda, r.bda, 6);
    s_devs[slot].rssi    = rssi;
    s_devs[slot].last_ms = now;
    s_devs[slot].valid   = true;
}

// ---- lifecycle -------------------------------------------------------------

static void start_analysis()
{
    if (s_running) return;
    memset(s_devs, 0, sizeof(s_devs));
    s_pkts           = 0;
    s_pps_last_pkts  = 0;
    s_pps_smoothed   = 0.0f;
    s_strongest_rssi = -127;
    s_last_tick_ms   = millis();
    s_running        = true;
    ble_scan_add(scan_cb);
}

static void stop_analysis()
{
    if (!s_running) return;
    s_running = false;
    ble_scan_remove(scan_cb);
}

// ---- redraw ---------------------------------------------------------------

static void refresh()
{
    if (!s_running) return;
    uint32_t now = millis();

    // Packet-rate EMA over actual elapsed wall time.
    uint32_t cur_pkts = s_pkts;
    uint32_t delta    = cur_pkts - s_pps_last_pkts;
    s_pps_last_pkts   = cur_pkts;
    uint32_t elapsed  = now - s_last_tick_ms;
    s_last_tick_ms    = now;
    float pps_new     = elapsed ? ((float)delta * 1000.0f / (float)elapsed) : 0;
    s_pps_smoothed    = s_pps_smoothed * 0.6f + pps_new * 0.4f;

    // Count surviving devices per bucket; reap stale entries on the way.
    int counts[N_BUCKETS] = {0};
    int total_active      = 0;
    for (int i = 0; i < BT_DEV_MAX; i++) {
        if (!s_devs[i].valid) continue;
        if (now - s_devs[i].last_ms > BT_DEV_TTL_MS) {
            s_devs[i].valid = false;
            continue;
        }
        counts[bucket_of(s_devs[i].rssi)]++;
        total_active++;
    }

    // Auto-scale the chart, floored so a sparse environment still shows shape.
    int maxc = 4;
    for (int i = 0; i < N_BUCKETS; i++) if (counts[i] > maxc) maxc = counts[i];
    int chart_h = CHART_BOTTOM_Y - CHART_TOP_Y;

    for (int i = 0; i < N_BUCKETS; i++) {
        int h = (counts[i] > 0)
              ? (int)((float)counts[i] / (float)maxc * (float)chart_h)
              : 2;
        if (h < 2)       h = 2;
        if (h > chart_h) h = chart_h;
        lv_obj_set_pos(bars[i], bar_x[i], CHART_BOTTOM_Y - h);
        lv_obj_set_height(bars[i], h);
        if (counts[i] > 0) {
            lv_obj_set_style_bg_color(bars[i], bucket_colour(i), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(bars[i], lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_70, LV_PART_MAIN);
        }
    }

    lv_label_set_text_fmt(status_label,
        "%d devices  %.0f adv/s  peak %d dBm",
        total_active, s_pps_smoothed,
        (s_strongest_rssi == -127) ? 0 : (int)s_strongest_rssi);

    if (s_strongest_rssi != -127) {
        lv_label_set_text_fmt(legend_label,
            "strongest: %02X:%02X:%02X:%02X:%02X:%02X",
            s_strongest_bda[0], s_strongest_bda[1], s_strongest_bda[2],
            s_strongest_bda[3], s_strongest_bda[4], s_strongest_bda[5]);
    } else {
        lv_label_set_text(legend_label, "");
    }
}

static void on_timer(lv_timer_t *) { refresh(); }

// ---- events ----------------------------------------------------------------

// Swipe RIGHT returns to the WiFi analyzer (the left-side neighbour); LEFT
// hands off to the LoRa analyzer (the right-side neighbour). The BLE scan is
// dropped first either way so the next analyzer can claim its radio cleanly.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        stop_analysis();
        analyze_screen_show();
    } else if (dir == LV_DIR_LEFT) {
        stop_analysis();
        lora_analyze_screen_show();
    }
}

// ---- layout ----------------------------------------------------------------

void bt_analyze_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    title_label = lv_label_create(screen);
    lv_obj_set_style_text_color(title_label, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title_label, "Bluetooth");
    // Anchored at the top centre to match the PAGER / TPMS / SETTINGS
    // screens. font_48 extends to ~y=56 from this origin; status/legend
    // below stay where they are with breathing room between them.
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(status_label, "starting...");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 88);

    legend_label = lv_label_create(screen);
    lv_obj_set_style_text_color(legend_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(legend_label, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(legend_label, "");
    lv_obj_align(legend_label, LV_ALIGN_TOP_MID, 0, 108);

    int total_w = N_BUCKETS * BAR_W + (N_BUCKETS - 1) * BAR_GAP;
    int start_x = (410 - total_w) / 2;

    static const char *bucket_text[N_BUCKETS] = {
        "-30", "-40", "-50", "-60", "-70", "-80", "-90", "far",
    };

    for (int i = 0; i < N_BUCKETS; i++) {
        int x = start_x + i * (BAR_W + BAR_GAP);
        bar_x[i] = x;

        bars[i] = lv_obj_create(screen);
        lv_obj_set_size(bars[i], BAR_W, 2);
        lv_obj_set_style_bg_color(bars[i], lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_radius(bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(bars[i], x, CHART_BOTTOM_Y - 2);

        bucket_labels[i] = lv_label_create(screen);
        lv_obj_set_style_text_color(bucket_labels[i], ARGUS_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(bucket_labels[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(bucket_labels[i], bucket_text[i]);
        lv_obj_set_pos(bucket_labels[i], x + (BAR_W - 22) / 2, CHART_BOTTOM_Y + 4);
    }

    lv_obj_t *base = lv_obj_create(screen);
    lv_obj_set_size(base, total_w, 1);
    lv_obj_set_style_bg_color(base, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(base, start_x, CHART_BOTTOM_Y);

    lv_obj_t *axis_hint = lv_label_create(screen);
    lv_obj_set_style_text_color(axis_hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(axis_hint, &font_argus_label_14, LV_PART_MAIN);
    lv_label_set_text(axis_hint, "RSSI (dBm) - closer is stronger");
    // Bottom of visible circle is at y≈456; pull the hint up so the
    // label clears the bezel.
    lv_obj_align(axis_hint, LV_ALIGN_BOTTOM_MID, 0, -50);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_timer, 500, NULL);
}

void bt_analyze_screen_show()
{
    start_analysis();
    refresh();
    lv_scr_load(screen);
}

bool bt_analyze_screen_is_active()
{
    return lv_screen_active() == screen;
}

void bt_analyze_screen_stop()
{
    // No-op when !s_running, so callers don't need to gate this themselves.
    stop_analysis();
}
