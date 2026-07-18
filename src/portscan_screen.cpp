#include "portscan_screen.h"
#include "theme.h"
#include "portscan.h"
#include <LilyGoLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defined elsewhere.
void wifi_screen_show();

// ---- state -----------------------------------------------------------------

static lv_obj_t *portscan_screen;
static lv_obj_t *title_label;        // "10.0.0.42 — TCP Connect"
static lv_obj_t *tech_btns[3];
static lv_obj_t *preset_btns[3];
static lv_obj_t *range_row;          // hidden unless Range is the active preset
static lv_obj_t *range_lo_ta;
static lv_obj_t *range_hi_ta;
static lv_obj_t *keyboard;
static lv_obj_t *start_btn;
static lv_obj_t *start_btn_label;
static lv_obj_t *status_label;
static lv_obj_t *result_list;

static uint32_t       s_target_ip   = 0;
static char           s_target_name[64] = "";   // optional hostname for title
// Bitmask of selected techniques (PSTECH_*). Multi-select: any combination
// may be active; at least one stays selected at all times.
static PortScanTech   s_tech        = PSTECH_TCP_CONNECT;
static PortScanPreset s_preset      = PSPRE_TOP_20;
static uint16_t       s_custom_lo   = 1;
static uint16_t       s_custom_hi   = 1024;

// We rebuild the result rows incrementally — track the count we've already
// rendered so the live update doesn't tear down + redraw every tick.
static int            s_shown_results = 0;

// ---- helpers ---------------------------------------------------------------

static const char *TECH_LABELS[3]   = { "TCP Connect", "UDP", "Banner" };
static const char *PRESET_LABELS[3] = { "Top 20", "Top 100", "Range" };

static void format_target(char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u",
        (s_target_ip >> 24) & 0xFF, (s_target_ip >> 16) & 0xFF,
        (s_target_ip >>  8) & 0xFF,  s_target_ip        & 0xFF);
}

// Renders the technique bitmask as "TCP+UDP+Banner" / "TCP+UDP" / etc.
static void format_tech_string(char *out, size_t n)
{
    out[0] = '\0';
    if (s_tech & PSTECH_TCP_CONNECT) strncat(out, "TCP",     n - strlen(out) - 1);
    if (s_tech & PSTECH_UDP)         { if (out[0]) strncat(out, "+", n - strlen(out) - 1);
                                        strncat(out, "UDP",    n - strlen(out) - 1); }
    if (s_tech & PSTECH_BANNER)      { if (out[0]) strncat(out, "+", n - strlen(out) - 1);
                                        strncat(out, "Banner", n - strlen(out) - 1); }
    if (!out[0]) strncpy(out, "(none)", n);
}

static void update_title()
{
    char ip[20]; format_target(ip, sizeof(ip));
    char tech_s[40]; format_tech_string(tech_s, sizeof(tech_s));
    char buf[120];
    // Hostname-first when we got one — the IP slides into a smaller-looking
    // parenthetical so the user still sees both without losing the
    // human-readable identifier.
    if (s_target_name[0])
        snprintf(buf, sizeof(buf), "%s (%s)  -  %s", s_target_name, ip, tech_s);
    else
        snprintf(buf, sizeof(buf), "%s  -  %s", ip, tech_s);
    lv_label_set_text(title_label, buf);
}

// Multi-select painter for the technique row — each button is independently
// "on" or "off" against the bitmask. Used in lieu of the radio-style helper
// (which still drives the preset row, where exactly one is active).
static void paint_tech_btns()
{
    lv_color_t on  = lv_color_make(0x00, 0x55, 0xAA);
    lv_color_t off = lv_color_make(0x22, 0x22, 0x22);
    static const uint8_t bits[3] = {
        PSTECH_TCP_CONNECT, PSTECH_UDP, PSTECH_BANNER
    };
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(tech_btns[i],
            (s_tech & bits[i]) ? on : off, LV_PART_MAIN);
}

// Single-select painter retained for the preset row (Top 20 / Top 100 / Range
// remain mutually exclusive — only one preset can drive the port list).
static void paint_preset_btns(int active)
{
    lv_color_t on  = lv_color_make(0x00, 0x55, 0xAA);
    lv_color_t off = lv_color_make(0x22, 0x22, 0x22);
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(preset_btns[i],
            (i == active) ? on : off, LV_PART_MAIN);
}

static void apply_range_visibility()
{
    if (s_preset == PSPRE_CUSTOM) lv_obj_clear_flag(range_row, LV_OBJ_FLAG_HIDDEN);
    else                          lv_obj_add_flag(range_row, LV_OBJ_FLAG_HIDDEN);
}

// ---- start/stop ------------------------------------------------------------

static void update_start_btn()
{
    if (portscan_is_running()) {
        lv_obj_set_style_bg_color(start_btn,
            lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN);
        lv_label_set_text(start_btn_label, "STOP");
    } else {
        lv_obj_set_style_bg_color(start_btn,
            lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
        lv_label_set_text(start_btn_label, "START");
    }
}

static void update_status()
{
    char buf[64];
    if (portscan_is_running()) {
        snprintf(buf, sizeof(buf), "Scanning  %d / %d   open: %d",
            portscan_scanned(), portscan_total(), portscan_result_count());
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    } else if (portscan_total() > 0) {
        snprintf(buf, sizeof(buf), "Done   %d ports   %d open",
            portscan_total(), portscan_result_count());
        lv_obj_set_style_text_color(status_label,
            ARGUS_ACCENT, LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf), "Tap START");
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    }
    lv_label_set_text(status_label, buf);
}

// ---- result list -----------------------------------------------------------

static void result_row_add(int idx)
{
    const PortScanResult *r = portscan_result(idx);
    if (!r) return;

    lv_obj_t *row = lv_obj_create(result_list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_make(0x16, 0x16, 0x16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(row, 2, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const char *state_str =
        (r->state == PSTATE_OPEN_OR_FILT) ? "open|filtered" : "open";
    lv_color_t state_col =
        (r->state == PSTATE_OPEN_OR_FILT) ? lv_color_make(0xFF, 0xCC, 0x00)
                                          : ARGUS_ACCENT;

    char hdr[40];
    snprintf(hdr, sizeof(hdr), "%u/%s  %s",
             r->port, r->is_udp ? "udp" : "tcp", state_str);
    lv_obj_t *h = lv_label_create(row);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, state_col, LV_PART_MAIN);
    lv_label_set_text(h, hdr);

    if (r->banner[0]) {
        lv_obj_t *b = lv_label_create(row);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_style_text_font(b, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(b, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_label_set_text(b, r->banner);
    }
}

static void refresh_results()
{
    int n = portscan_result_count();
    // Full rebuild if the count dropped (e.g. new scan started) — otherwise
    // append-only for low-cost live updates.
    if (n < s_shown_results) {
        lv_obj_clean(result_list);
        s_shown_results = 0;
    }
    for (int i = s_shown_results; i < n; i++) result_row_add(i);
    s_shown_results = n;
}

// ---- events ----------------------------------------------------------------

static void on_tech_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static const uint8_t bits[3] = {
        PSTECH_TCP_CONNECT, PSTECH_UDP, PSTECH_BANNER
    };
    uint8_t bit = bits[idx];

    // Toggle. Block the deselect when it would empty the mask — a scan
    // needs at least one technique to run, and "all three off" leaves no
    // visual feedback either, so disallow it at the source.
    uint8_t next = s_tech ^ bit;
    if ((next & (PSTECH_TCP_CONNECT | PSTECH_UDP | PSTECH_BANNER)) == 0) return;
    s_tech = next;

    paint_tech_btns();
    update_title();
}

static void on_preset_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_preset = (PortScanPreset)idx;
    paint_preset_btns(idx);
    apply_range_visibility();
}

static void on_start_btn(lv_event_t *)
{
    if (portscan_is_running()) {
        portscan_stop();
        update_start_btn();
        update_status();
        return;
    }

    if (s_preset == PSPRE_CUSTOM) {
        const char *lo_s = lv_textarea_get_text(range_lo_ta);
        const char *hi_s = lv_textarea_get_text(range_hi_ta);
        int lo = lo_s && *lo_s ? atoi(lo_s) : 1;
        int hi = hi_s && *hi_s ? atoi(hi_s) : 1024;
        if (lo < 1)       lo = 1;
        if (lo > 65535)   lo = 65535;
        if (hi < 1)       hi = 1;
        if (hi > 65535)   hi = 65535;
        s_custom_lo = (uint16_t)lo;
        s_custom_hi = (uint16_t)hi;
    }

    portscan_clear_results();
    lv_obj_clean(result_list);
    s_shown_results = 0;

    bool ok = portscan_start(s_target_ip, s_tech, s_preset,
                             s_custom_lo, s_custom_hi);
    if (!ok) {
        lv_label_set_text(status_label, "Need WiFi");
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
        return;
    }
    update_start_btn();
    update_status();
}

// Range text-areas share the on-screen keyboard. Focus pulls it up, blur or
// READY/CANCEL puts it away — same pattern the pager TX modal uses.
static void on_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(keyboard, ta);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(keyboard);
        if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        if (portscan_is_running()) portscan_stop();
        wifi_screen_show();
    }
}

static void on_tick(lv_timer_t *)
{
    if (!portscan_screen_is_active()) return;
    refresh_results();
    update_status();
    static bool was_running = false;
    bool now_running = portscan_is_running();
    if (now_running != was_running) {
        update_start_btn();
        was_running = now_running;
    }
}

// ---- layout helpers --------------------------------------------------------

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int idx,
                          lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, 38);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(l);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return btn;
}

// ---- public API ------------------------------------------------------------

void portscan_screen_create()
{
    portscan_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(portscan_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(portscan_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(portscan_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(portscan_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    title_label = lv_label_create(portscan_screen);
    lv_obj_set_style_text_color(title_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(title_label, "");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);

    // Technique row (y=50). Three equal buttons spread across the panel.
    // 380 px wide / 3 buttons + 2 gaps of 8 px → 121 px each, x starts at 13.
    tech_btns[0]  = make_btn(portscan_screen, TECH_LABELS[0], 0,
                             13,            50, 121, on_tech_btn);
    tech_btns[1]  = make_btn(portscan_screen, TECH_LABELS[1], 1,
                             13 + 129,      50, 121, on_tech_btn);
    tech_btns[2]  = make_btn(portscan_screen, TECH_LABELS[2], 2,
                             13 + 129 * 2,  50, 121, on_tech_btn);

    // Preset row (y=98). Same column geometry as the technique row so the
    // two rows visually align.
    preset_btns[0] = make_btn(portscan_screen, PRESET_LABELS[0], 0,
                              13,            98, 121, on_preset_btn);
    preset_btns[1] = make_btn(portscan_screen, PRESET_LABELS[1], 1,
                              13 + 129,      98, 121, on_preset_btn);
    preset_btns[2] = make_btn(portscan_screen, PRESET_LABELS[2], 2,
                              13 + 129 * 2,  98, 121, on_preset_btn);

    // Custom range row (y=148) — text fields for lo/hi. Hidden by default.
    range_row = lv_obj_create(portscan_screen);
    lv_obj_set_size(range_row, 380, 44);
    lv_obj_set_pos(range_row, 15, 144);
    lv_obj_set_style_bg_opa(range_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(range_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(range_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(range_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(range_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lo_lbl = lv_label_create(range_row);
    lv_obj_set_style_text_color(lo_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(lo_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(lo_lbl, "Lo");
    lv_obj_align(lo_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    range_lo_ta = lv_textarea_create(range_row);
    lv_textarea_set_one_line(range_lo_ta, true);
    lv_textarea_set_max_length(range_lo_ta, 5);
    lv_textarea_set_accepted_chars(range_lo_ta, "0123456789");
    lv_textarea_set_text(range_lo_ta, "1");
    lv_obj_set_size(range_lo_ta, 100, 36);
    lv_obj_set_style_text_font(range_lo_ta, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(range_lo_ta, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(range_lo_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(range_lo_ta, LV_ALIGN_LEFT_MID, 30, 0);
    lv_obj_add_event_cb(range_lo_ta, on_ta_event, LV_EVENT_ALL, NULL);

    lv_obj_t *hi_lbl = lv_label_create(range_row);
    lv_obj_set_style_text_color(hi_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(hi_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(hi_lbl, "Hi");
    lv_obj_align(hi_lbl, LV_ALIGN_LEFT_MID, 144, 0);

    range_hi_ta = lv_textarea_create(range_row);
    lv_textarea_set_one_line(range_hi_ta, true);
    lv_textarea_set_max_length(range_hi_ta, 5);
    lv_textarea_set_accepted_chars(range_hi_ta, "0123456789");
    lv_textarea_set_text(range_hi_ta, "1024");
    lv_obj_set_size(range_hi_ta, 100, 36);
    lv_obj_set_style_text_font(range_hi_ta, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(range_hi_ta, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(range_hi_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(range_hi_ta, LV_ALIGN_LEFT_MID, 176, 0);
    lv_obj_add_event_cb(range_hi_ta, on_ta_event, LV_EVENT_ALL, NULL);

    // START / STOP — fixed position at y=196 so it doesn't move when the
    // range row shows/hides.
    start_btn = lv_obj_create(portscan_screen);
    lv_obj_set_size(start_btn, 200, 44);
    lv_obj_align(start_btn, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_set_style_radius(start_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(start_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(start_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(start_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(start_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(start_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(start_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(start_btn, on_start_btn, LV_EVENT_CLICKED, NULL);

    start_btn_label = lv_label_create(start_btn);
    lv_obj_set_style_text_color(start_btn_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(start_btn_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(start_btn_label, "START");
    lv_obj_center(start_btn_label);

    // Status line + scrollable result list fill the lower half.
    status_label = lv_label_create(portscan_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tap START");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 248);

    result_list = lv_obj_create(portscan_screen);
    lv_obj_set_size(result_list, 410, 232);
    lv_obj_align(result_list, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_bg_color(result_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(result_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(result_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(result_list, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(result_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(result_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(result_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(result_list, LV_FLEX_FLOW_COLUMN);

    // Shared on-screen keyboard for the lo/hi fields; hidden until either
    // text-area gets focus.
    keyboard = lv_keyboard_create(portscan_screen);
    lv_obj_set_size(keyboard, lv_pct(100), 180);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    paint_tech_btns();
    paint_preset_btns((int)s_preset);
    apply_range_visibility();

    lv_timer_create(on_tick, 400, NULL);
}

void portscan_screen_show(uint32_t ip_host_order, const char *name)
{
    s_target_ip = ip_host_order;
    if (name && *name) {
        strncpy(s_target_name, name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
    } else {
        s_target_name[0] = '\0';
    }
    // Show whatever results are already on file for this target — typically
    // empty because we cleared on START, but keeps the screen idempotent
    // when re-opened after a finished scan.
    s_shown_results = 0;
    lv_obj_clean(result_list);
    int n = portscan_result_count();
    for (int i = 0; i < n; i++) result_row_add(i);
    s_shown_results = n;

    update_title();
    update_start_btn();
    update_status();
    lv_scr_load(portscan_screen);
}

bool portscan_screen_is_active()
{
    return lv_screen_active() == portscan_screen;
}
