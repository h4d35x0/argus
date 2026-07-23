#include "probe_sniffer.h"
#include "wifi_beacon_manager.h"
#include "offense_wifi.h"
#include "argus_mode.h"
#include "theme.h"
#include <Arduino.h>          // portMUX
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp / main.cpp
void tools_screen_show();
void low_mem_show_dialog(const char *msg);

// ---- device store (WiFi task writes, LVGL task reads) ----------------------
struct ProbeDev {
    bool     used;
    uint8_t  mac[6];
    char     ssid[33];   // latest DIRECTED probe SSID seen from this device ("" = only broadcast)
    int8_t   rssi;
    uint16_t count;
};
#define PS_MAX 24
static ProbeDev       s_devs[PS_MAX];
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool           s_running = false;

static void noop_beacon(const WifiBeacon *) {}   // holds WiFi up in survey mode

static void probe_cb(const WifiMgmtFrame *m)
{
    if (!m || m->subtype != 0x04) return;        // probe REQUEST only

    // Parse the SSID element (id 0), which for a directed probe names the
    // network the device is looking for. The mgmt header is 24 bytes.
    char ssid[33] = {0};
    if (m->frame && m->len >= 26) {
        const uint8_t *tags = m->frame + 24;
        int tl = m->len - 24;
        if (tl >= 2 && tags[0] == 0) {           // first element is SSID
            int slen = tags[1];
            if (slen > 32) slen = 32;
            if (slen > 0 && 2 + slen <= tl) { memcpy(ssid, tags + 2, slen); ssid[slen] = '\0'; }
        }
    }

    portENTER_CRITICAL(&s_mux);
    int slot = -1, weakest = 0;
    for (int i = 0; i < PS_MAX; i++) {
        if (s_devs[i].used && memcmp(s_devs[i].mac, m->src, 6) == 0) { slot = i; break; }
        if (!s_devs[i].used && slot < 0) slot = i;
        if (s_devs[i].used && s_devs[i].count < s_devs[weakest].count) weakest = i;
    }
    if (slot < 0) slot = weakest;                // full: evict least-active
    ProbeDev &d = s_devs[slot];
    if (!d.used || memcmp(d.mac, m->src, 6) != 0) {
        memset(&d, 0, sizeof(d));
        memcpy(d.mac, m->src, 6);
        d.used = true;
    }
    d.rssi = m->rssi;
    if (d.count < 0xFFFF) d.count++;
    if (ssid[0]) { strncpy(d.ssid, ssid, sizeof(d.ssid) - 1); d.ssid[sizeof(d.ssid) - 1] = '\0'; }
    portEXIT_CRITICAL(&s_mux);
}

static int snapshot(ProbeDev *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < PS_MAX && n < max; i++)
        if (s_devs[i].used) out[n++] = s_devs[i];
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool probe_sniffer_start()
{
    if (s_running) return true;
    if (offense_wifi_held()) return false;       // don't fight the injection tools
    portENTER_CRITICAL(&s_mux); memset(s_devs, 0, sizeof(s_devs)); portEXIT_CRITICAL(&s_mux);
    if (!wifi_beacon_add(noop_beacon)) return false;      // brings WiFi up (BLE guard inside)
    if (!wifi_mgmt_add(probe_cb)) { wifi_beacon_remove(noop_beacon); return false; }
    s_running = true;
    return true;
}

void probe_sniffer_stop()
{
    wifi_mgmt_remove(probe_cb);
    wifi_beacon_remove(noop_beacon);
    s_running = false;
}

bool probe_sniffer_is_running() { return s_running; }

// ---- screen ----------------------------------------------------------------
static lv_obj_t *ps_screen;
static lv_obj_t *ps_count;
static lv_obj_t *ps_list;
static lv_obj_t *ps_toggle_lbl;
static lv_obj_t *ps_toggle_btn;
static lv_timer_t *ps_ui;

static void ps_refresh(lv_timer_t *)
{
    if (!probe_sniffer_screen_is_active()) return;
    bool run = probe_sniffer_is_running();
    lv_label_set_text(ps_toggle_lbl, run ? "STOP" : "START");
    lv_obj_set_style_bg_color(ps_toggle_btn, run ? HADES_RED : lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);

    ProbeDev devs[PS_MAX];
    int n = snapshot(devs, PS_MAX);
    char c[40];
    snprintf(c, sizeof(c), run ? "%d devices" : "idle - press START", n);
    lv_label_set_text(ps_count, c);

    lv_obj_clean(ps_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(ps_list);
        lv_obj_set_size(row, 388, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_make(0x12, 0x16, 0x1C), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        char t[96];
        snprintf(t, sizeof(t), "%02X:%02X:%02X:%02X:%02X:%02X   %d dBm\nlooking for: %s",
                 devs[i].mac[0], devs[i].mac[1], devs[i].mac[2],
                 devs[i].mac[3], devs[i].mac[4], devs[i].mac[5], (int)devs[i].rssi,
                 devs[i].ssid[0] ? devs[i].ssid : "(any / broadcast)");
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, &font_argus_label_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, devs[i].ssid[0] ? ARGUS_OFFENSE_ACCENT : ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(l, t);
    }
}

static void ps_on_toggle(lv_event_t *)
{
    if (probe_sniffer_is_running()) probe_sniffer_stop();
    else if (!probe_sniffer_start()) {
        const char *why = offense_wifi_busy_reason();
        low_mem_show_dialog(why ? why : "Can't start. Turn Bluetooth off\nand stop other WiFi tools, then retry.");
        return;
    }
    ps_refresh(nullptr);
}

static void ps_on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

void probe_sniffer_screen_create()
{
    ps_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ps_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ps_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ps_screen);
    lv_obj_set_style_text_color(title, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(title, "PROBES");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    ps_count = lv_label_create(ps_screen);
    lv_obj_set_style_text_color(ps_count, lv_color_make(0x3C, 0xDC, 0x78), LV_PART_MAIN);
    lv_obj_set_style_text_font(ps_count, &font_argus_label_16, LV_PART_MAIN);
    lv_label_set_text(ps_count, "idle - press START");
    lv_obj_align(ps_count, LV_ALIGN_TOP_MID, 0, 54);

    ps_list = lv_obj_create(ps_screen);
    lv_obj_set_size(ps_list, 404, 300);
    lv_obj_align(ps_list, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_color(ps_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(ps_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ps_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ps_list, 5, LV_PART_MAIN);
    lv_obj_set_layout(ps_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ps_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ps_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ps_list, LV_SCROLLBAR_MODE_AUTO);

    ps_toggle_btn = lv_button_create(ps_screen);
    lv_obj_set_size(ps_toggle_btn, 200, 60);
    lv_obj_align(ps_toggle_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(ps_toggle_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ps_toggle_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(ps_toggle_btn, ps_on_toggle, LV_EVENT_CLICKED, NULL);
    ps_toggle_lbl = lv_label_create(ps_toggle_btn);
    lv_obj_set_style_text_color(ps_toggle_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(ps_toggle_lbl, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(ps_toggle_lbl, "START");
    lv_obj_center(ps_toggle_lbl);

    lv_obj_add_event_cb(ps_screen, ps_on_gesture, LV_EVENT_GESTURE, NULL);

    ps_ui = lv_timer_create(ps_refresh, 1000, NULL);
    lv_timer_pause(ps_ui);
}

void probe_sniffer_screen_show()
{
    if (argus_mode_current() != ArgusMode::Offense) { tools_screen_show(); return; }
    ps_refresh(nullptr);
    lv_timer_resume(ps_ui);
    lv_scr_load(ps_screen);
}

bool probe_sniffer_screen_is_active() { return lv_screen_active() == ps_screen; }
