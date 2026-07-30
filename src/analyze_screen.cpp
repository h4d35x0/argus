#include "analyze_screen.h"
#include "theme.h"
#include "bt_analyze_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

// Defined in tools_screen.cpp
void tools_screen_show();

#define N_CHANNELS  13
// BAR_W trimmed 24→22, BAR_GAP 4→3 to keep the chart width inside the
// 410-diameter visible circle once CHART_BOTTOM_Y dropped to 375. With
// 13·22 + 12·3 = 322 px total, the outer corners at (44, 375) and
// (366, 375) sit ~203 px from the screen centre — just inside r=205.
#define HOP_MS      150       // dwell time per channel before hopping
#define BAR_W       22
#define BAR_GAP     3
// Chart geometry tuned to fit inside the watch's ~410-diameter visible
// circle. A 360-px-wide chart (13*24 + 12*4) fits the circle for any Y in
// [153, 349] — pulling the top/bottom in keeps the corners off the bezel.
#define CHART_BOTTOM_Y  375   // y of the bar baseline — pushed down from
                              // 348 so the chart claims more vertical
                              // real estate (gain ~27 px of bar height).
                              // Stays inside the visible circle thanks
                              // to the matching BAR_W/BAR_GAP shrink.
#define CHART_TOP_Y     154   // y of the chart's top (max bar reach)

static lv_obj_t *screen;
static lv_obj_t *title_label;
static lv_obj_t *status_label;
static lv_obj_t *legend_label;
static lv_obj_t *bars[N_CHANNELS];
static lv_obj_t *ch_labels[N_CHANNELS];
static int       bar_x[N_CHANNELS];   // x position of each bar (constant)

// Per-packet counters touched by the promiscuous callback (WiFi task) and
// snapshot/reset by the main task at each hop. Reads/writes of these uint32_t
// are atomic on the ESP32-S3; the occasional dropped packet during reset is
// fine for a rate estimate.
static volatile uint32_t s_cur_pkts     = 0;
static volatile uint32_t s_cur_bytes    = 0;
static volatile int8_t   s_cur_max_rssi = -127;

struct ChanStat {
    float    pps;              // packets per second, EMA-smoothed
    int8_t   peak_rssi;        // peak RSSI ever seen on this channel
    uint32_t pkt_total;        // cumulative packet count
};
static ChanStat s_chan[N_CHANNELS];

static int      s_cur_ch     = 1;          // 1-based channel currently tuned
static uint32_t s_hop_start  = 0;
static uint32_t s_total_pkts = 0;
static bool     s_running    = false;

// WiFi mode at the moment we entered the analyzer — stop_analysis() restores
// it so leaving the screen returns the radio to its prior state (typically
// WIFI_OFF if nothing was using it, WIFI_STA if the WiFi tile had it active).
static wifi_mode_t s_prev_wifi_mode = WIFI_MODE_NULL;

// ---- promiscuous capture ---------------------------------------------------

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    s_cur_pkts++;
    s_cur_bytes += p->rx_ctrl.sig_len;
    int8_t r = p->rx_ctrl.rssi;
    if (r > s_cur_max_rssi) s_cur_max_rssi = r;
}

static void start_analysis()
{
    if (s_running) return;
    // Snapshot the WiFi mode *before* we touch the radio so we can restore it
    // on the way out.
    s_prev_wifi_mode = WiFi.getMode();

    memset(s_chan, 0, sizeof(s_chan));
    for (int i = 0; i < N_CHANNELS; i++) s_chan[i].peak_rssi = -127;
    s_cur_ch       = 1;
    s_cur_pkts     = 0;
    s_cur_bytes    = 0;
    s_cur_max_rssi = -127;
    s_total_pkts   = 0;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_channel(s_cur_ch, WIFI_SECOND_CHAN_NONE);
    s_hop_start = millis();
    s_running   = true;
}

static void stop_analysis()
{
    if (!s_running) return;
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous(false);

    if (s_prev_wifi_mode == WIFI_MODE_NULL) {
        // WiFi was off before — make sure the IDF state is fully torn down so
        // esp_wifi_get_mode() returns ESP_ERR_WIFI_NOT_INIT and the clock-face
        // WiFi indicator goes back to gray. WiFi.mode(WIFI_OFF) alone is
        // documented to do this, but in practice after promiscuous-mode use
        // it leaves the IDF still initialised on this Arduino-ESP32 version,
        // so call the IDF teardown directly as a belt and braces.
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        esp_wifi_deinit();
    } else {
        // Restore the prior on-mode (e.g. WIFI_STA if the WiFi tile had been
        // associated). The association itself is gone because promiscuous mode
        // required a disconnect, but the mode flag is preserved.
        WiFi.mode(s_prev_wifi_mode);
    }
    s_running = false;
}

// ---- per-tick state machine + redraw ---------------------------------------

static lv_color_t bar_colour(float pps)
{
    if (pps > 80.0f) return lv_color_make(0xFF, 0x44, 0x44);   // saturated
    if (pps > 20.0f) return lv_color_make(0xFF, 0xCC, 0x00);   // busy
    if (pps >  3.0f) return lv_color_make(0x44, 0xBB, 0xFF);   // light
    return                lv_color_make(0x00, 0xAA, 0x55);     // quiet
}

static void refresh()
{
    if (!s_running) return;

    uint32_t now = millis();
    uint32_t elapsed = now - s_hop_start;
    if (elapsed >= HOP_MS) {
        // Snapshot + reset the per-channel counters, then advance.
        uint32_t pkts  = s_cur_pkts;
        int8_t   rssi  = s_cur_max_rssi;
        s_cur_pkts     = 0;
        s_cur_bytes    = 0;
        s_cur_max_rssi = -127;

        int idx = s_cur_ch - 1;
        float secs = elapsed / 1000.0f;
        float pps_new = (secs > 0.001f) ? (pkts / secs) : 0;
        s_chan[idx].pps        = s_chan[idx].pps * 0.6f + pps_new * 0.4f;
        s_chan[idx].pkt_total += pkts;
        if (rssi > s_chan[idx].peak_rssi) s_chan[idx].peak_rssi = rssi;
        s_total_pkts          += pkts;

        s_cur_ch = (s_cur_ch % N_CHANNELS) + 1;
        esp_wifi_set_channel(s_cur_ch, WIFI_SECOND_CHAN_NONE);
        s_hop_start = now;
    }

    // Auto-scale the chart to the busiest channel so quieter channels stay
    // visible. Floor at 10 pps so a dead spectrum doesn't blow up tiny noise.
    float max_pps = 10.0f;
    for (int i = 0; i < N_CHANNELS; i++)
        if (s_chan[i].pps > max_pps) max_pps = s_chan[i].pps;

    int chart_h = CHART_BOTTOM_Y - CHART_TOP_Y;
    int best_ch = 1;
    float best_pps = -1.0f;
    for (int i = 0; i < N_CHANNELS; i++) {
        int h = (int)(s_chan[i].pps / max_pps * (float)chart_h);
        if (h < 2) h = 2;
        if (h > chart_h) h = chart_h;
        // Reposition the bar's top so it appears to grow upward from
        // CHART_BOTTOM_Y as its height changes.
        lv_obj_set_pos(bars[i], bar_x[i], CHART_BOTTOM_Y - h);
        lv_obj_set_height(bars[i], h);
        lv_obj_set_style_bg_color(bars[i], bar_colour(s_chan[i].pps), LV_PART_MAIN);

        bool active = (i == s_cur_ch - 1);
        lv_obj_set_style_border_color(bars[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(bars[i], active ? 2 : 0, LV_PART_MAIN);

        if (s_chan[i].pps > best_pps) { best_pps = s_chan[i].pps; best_ch = i + 1; }
    }

    // Status: which channel is currently tuned and the total packets captured.
    int idx_active = s_cur_ch - 1;
    if (s_chan[idx_active].peak_rssi > -127) {
        lv_label_set_text_fmt(status_label,
            "ch %d  %.0f pkt/s  peak %d dBm   total %lu",
            s_cur_ch, s_chan[idx_active].pps,
            (int)s_chan[idx_active].peak_rssi,
            (unsigned long)s_total_pkts);
    } else {
        lv_label_set_text_fmt(status_label,
            "ch %d   total %lu pkts",
            s_cur_ch, (unsigned long)s_total_pkts);
    }

    // Hint at the bottom: the least-busy channel candidate.
    int quietest = 0;
    float qp = s_chan[0].pps;
    for (int i = 1; i < N_CHANNELS; i++) if (s_chan[i].pps < qp) { qp = s_chan[i].pps; quietest = i; }
    lv_label_set_text_fmt(legend_label,
        "quiet: ch %d (%.0f pkt/s)   busiest: ch %d (%.0f pkt/s)",
        quietest + 1, qp, best_ch, best_pps < 0 ? 0 : best_pps);
}

static void on_timer(lv_timer_t *) { refresh(); }

// ---- events ----------------------------------------------------------------

// Swipe LEFT hands off to the BT analyzer (the right-side neighbour); WiFi
// promiscuous mode is released first so the BT scan can run without contention.
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        stop_analysis();
        bt_analyze_screen_show();
    }
}

// ---- layout ----------------------------------------------------------------

void analyze_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    title_label = lv_label_create(screen);
    lv_obj_set_style_text_color(title_label, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title_label, "WiFi");
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

    // 13 channel bars + numbers.
    int total_w = N_CHANNELS * BAR_W + (N_CHANNELS - 1) * BAR_GAP;
    int start_x = (410 - total_w) / 2;
    for (int i = 0; i < N_CHANNELS; i++) {
        int x = start_x + i * (BAR_W + BAR_GAP);
        bar_x[i] = x;

        bars[i] = lv_obj_create(screen);
        lv_obj_set_size(bars[i], BAR_W, 2);
        lv_obj_set_style_bg_color(bars[i], lv_color_make(0x00, 0xAA, 0x55), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_SCROLLABLE);
        // refresh() repositions each bar's y as height changes so it appears
        // to grow upward from the chart baseline.
        lv_obj_set_pos(bars[i], x, CHART_BOTTOM_Y - 2);

        ch_labels[i] = lv_label_create(screen);
        lv_obj_set_style_text_color(ch_labels[i], ARGUS_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(ch_labels[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text_fmt(ch_labels[i], "%d", i + 1);
        // Centre each channel number under its bar.
        lv_obj_set_pos(ch_labels[i], x + (BAR_W - 12) / 2, CHART_BOTTOM_Y + 4);
    }

    // Baseline under the bars.
    lv_obj_t *base = lv_obj_create(screen);
    lv_obj_set_size(base, total_w, 1);
    lv_obj_set_style_bg_color(base, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(base, start_x, CHART_BOTTOM_Y);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_timer, 100, NULL);
}

void analyze_screen_show()
{
    start_analysis();
    refresh();
    lv_scr_load(screen);
}

bool analyze_screen_is_active()
{
    return lv_screen_active() == screen;
}

void analyze_screen_stop()
{
    // Internal stop_analysis() already early-exits when !s_running, so this
    // is safe to call unconditionally — main.cpp uses that to clean up on
    // back-button exit regardless of which analyzer was open.
    stop_analysis();
}
