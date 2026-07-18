#include "pager_screen.h"
#include "theme.h"
#include "pager.h"
#include "lora_screen.h"
#include <LilyGoLib.h>
#include <stdio.h>

// Defined in main.cpp
void tools_screen_show();

// ---- preset frequencies ----------------------------------------------------

// "SCAN ALL" is dropdown index 0 — picking it hands control to the
// multi-frequency hop-scanner in pager.cpp. The other entries are single
// frequencies and behave like the original RX behaviour.
#define FREQ_SCAN_ALL_IDX  0

static const float FREQ_PRESETS[] = {
    152.240f, 157.450f, 158.100f,
    454.025f, 462.550f,
    929.612f, 931.862f,
    466.075f,
};
static const char * const FREQ_LABELS =
    "SCAN ALL\n"
    "152.240 MHz\n"
    "157.450 MHz\n"
    "158.100 MHz\n"
    "454.025 MHz\n"
    "462.550 MHz\n"
    "929.612 MHz\n"
    "931.862 MHz\n"
    "466.075 MHz\n"
    "Custom...";

static const int FREQ_COUNT = (int)(sizeof(FREQ_PRESETS) / sizeof(FREQ_PRESETS[0]));
// "Custom..." sits at the end, after all presets
#define FREQ_CUSTOM_IDX  (FREQ_COUNT + 1)

// Convert dropdown selection index → frequency, treating index 0 as the
// SCAN-ALL sentinel (no concrete single freq) and FREQ_CUSTOM_IDX as the
// custom text-area path (caller must read rx_freq_ta directly).
static float freq_at_dropdown_idx(int sel)
{
    if (sel <= FREQ_SCAN_ALL_IDX) return 0.0f;
    int preset_idx = sel - 1;          // shift past SCAN ALL
    if (preset_idx >= FREQ_COUNT) return 0.0f;
    return FREQ_PRESETS[preset_idx];
}

// ---- widgets ---------------------------------------------------------------

static lv_obj_t *pager_screen;
static lv_obj_t *status_label;
static lv_obj_t *freq_dropdown;
static lv_obj_t *rx_freq_ta;    // custom RX frequency text area
static lv_obj_t *rx_kb;         // keyboard for rx_freq_ta
static lv_obj_t *scan_switch;
static lv_obj_t *msg_list;
static lv_obj_t *mode_btns[4];

// TX modal — sits on top of the main pager screen as a child obj with a
// dim semi-transparent backdrop. Hidden until the TX button is tapped.
static lv_obj_t *tx_modal;          // backdrop + container
static lv_obj_t *tx_panel;          // foreground panel
static lv_obj_t *tx_freq_dropdown;  // preset frequency selector
static lv_obj_t *tx_freq_ta;        // numeric MHz field (custom freq)
static lv_obj_t *tx_capcode_ta;     // capcode text area (numeric)
static lv_obj_t *tx_msg_ta;         // message text area
static lv_obj_t *tx_keyboard;       // on-screen keyboard
static lv_obj_t *tx_status_label;   // "Sent" / error reporting

static const char *MODE_LABELS[4] = { "512", "1200", "2400", "FLEX" };

// ---- TX freq presets -------------------------------------------------------
//
// Dropdown index 0 is "Custom..." — picking it leaves the freq text area
// editable for the user to type their own MHz value. Any other index
// auto-fills the text area with the matching preset, and the text area
// remains the canonical source of truth when on_tx_send actually reads
// the freq, so editing the field after a preset selection just shifts
// the dropdown back to "Custom..." semantically.
#define TX_FREQ_CUSTOM_IDX  0

static const float TX_FREQ_PRESETS[] = {
    152.240f, 157.450f, 158.100f,
    454.025f, 462.550f, 466.075f,
    929.612f, 931.862f,
};
static const char *const TX_FREQ_LABELS =
    "Custom...\n"
    "152.240 MHz\n"
    "157.450 MHz\n"
    "158.100 MHz\n"
    "454.025 MHz\n"
    "462.550 MHz\n"
    "466.075 MHz\n"
    "929.612 MHz\n"
    "931.862 MHz";

// ---- helpers ---------------------------------------------------------------

static lv_color_t func_color(uint8_t func)
{
    switch (func) {
    case PFUNC_TONE:    return lv_color_make(0xFF, 0xCC, 0x00);
    case PFUNC_NUMERIC: return lv_color_make(0x44, 0xAA, 0xFF);
    case PFUNC_ALPHA:   return ARGUS_ACCENT;
    case PFUNC_VOICE:   return lv_color_make(0xFF, 0x88, 0x00);
    default:            return lv_color_make(0xAA, 0x44, 0xFF); // FLEX — purple
    }
}

static const char *func_name(uint8_t func)
{
    switch (func) {
    case PFUNC_TONE:    return "TONE";
    case PFUNC_NUMERIC: return "NUM";
    case PFUNC_ALPHA:   return "ALPHA";
    case PFUNC_VOICE:   return "VOICE";
    default:            return "FLEX";
    }
}

static void update_mode_buttons()
{
    PagerMode cur = pager_get_mode();
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(mode_btns[i],
            (i == (int)cur) ? lv_color_make(0x00, 0x55, 0xAA)
                            : lv_color_make(0x22, 0x22, 0x22),
            LV_PART_MAIN);
    }
}

// Set when pager_start() fails for a reason other than the LoRa radio being
// busy (e.g. a RadioLib init error), so update_status() can report it.
static bool s_start_error = false;

static void update_status()
{
    if (pager_is_running()) {
        char buf[48];
        if (pager_is_scanning_all()) {
            snprintf(buf, sizeof(buf), "Scan %.3f  [%d/%d]",
                     pager_get_freq(),
                     pager_scan_current_idx() + 1,
                     pager_scan_freq_count());
        } else {
            snprintf(buf, sizeof(buf), "Scan %.3f", pager_get_freq());
        }
        lv_label_set_text(status_label, buf);
        lv_obj_set_style_text_color(status_label, ARGUS_ACCENT, LV_PART_MAIN);
        return;
    }
    if (lora_screen_is_powered()) {
        lv_label_set_text(status_label, "Radio in use by LoRa");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xAA, 0x00), LV_PART_MAIN);
        return;
    }
    if (s_start_error) {
        char buf[28];
        snprintf(buf, sizeof(buf), "Radio init failed (%d)", (int)pager_last_error());
        lv_label_set_text(status_label, buf);
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
        return;
    }
    lv_label_set_text(status_label, "Off");
    lv_obj_set_style_text_color(status_label, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
}

static void rebuild_msg_list()
{
    lv_obj_clean(msg_list);
    int n = pager_get_msg_count();
    if (n == 0) {
        lv_obj_t *lbl = lv_label_create(msg_list);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_style_text_color(lbl, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_label_set_text(lbl, "No messages yet.");
        lv_obj_center(lbl);
        return;
    }
    for (int i = 0; i < n; i++) {
        const PagerMsg *m = pager_get_msg(i);
        if (!m) continue;

        lv_obj_t *entry = lv_obj_create(msg_list);
        lv_obj_set_width(entry, lv_pct(100));
        lv_obj_set_height(entry, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(entry, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(entry, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_side(entry, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(entry, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_set_style_border_width(entry, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(entry, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(entry, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(entry, 3, LV_PART_MAIN);
        lv_obj_set_layout(entry, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(entry, LV_OBJ_FLAG_SCROLLABLE);

        // Header: capcode  [TYPE]  HH:MM:SS  freq
        // freq is included so the scanner UI can show which preset the
        // message arrived on without the user having to guess.
        char hdr[64];
        if (m->freq_mhz > 0.0f) {
            snprintf(hdr, sizeof(hdr), "#%07lu  [%s]  %s  %.3f",
                     (unsigned long)m->capcode, func_name(m->func),
                     m->time_str, m->freq_mhz);
        } else {
            snprintf(hdr, sizeof(hdr), "#%07lu  [%s]  %s",
                     (unsigned long)m->capcode, func_name(m->func),
                     m->time_str);
        }

        lv_obj_t *h = lv_label_create(entry);
        lv_obj_set_width(h, lv_pct(100));
        lv_obj_set_style_text_color(h, func_color(m->func), LV_PART_MAIN);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(h, hdr);
        lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);

        // Message body (only if there is one)
        if (m->text[0] != '\0') {
            lv_obj_t *t = lv_label_create(entry);
            lv_obj_set_width(t, lv_pct(100));
            lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_text_font(t, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_label_set_text(t, m->text);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        }
    }
}

// ---- event handlers --------------------------------------------------------

// Common path for (re)starting RX from the current dropdown + mode. Routes
// to either the single-freq pager_start() or the multi-freq scanner based
// on whether SCAN ALL is selected.
static bool start_from_ui()
{
    int sel = (int)lv_dropdown_get_selected(freq_dropdown);
    if (sel == FREQ_SCAN_ALL_IDX) {
        return pager_start_scanner(pager_get_mode());
    }
    if (sel == FREQ_CUSTOM_IDX) {
        const char *s = lv_textarea_get_text(rx_freq_ta);
        float f = (s && *s) ? strtof(s, NULL) : 0.0f;
        return pager_start(f, pager_get_mode());  // pager_start() validates range
    }
    float f = freq_at_dropdown_idx(sel);
    if (f <= 0.0f) f = FREQ_PRESETS[0];
    return pager_start(f, pager_get_mode());
}

static void on_freq_changed(lv_event_t *)
{
    int sel = (int)lv_dropdown_get_selected(freq_dropdown);

    // Keep text area in sync: auto-fill on preset selection so the user can
    // see the exact MHz value and edit from it if they switch to Custom...
    if (sel != FREQ_SCAN_ALL_IDX && sel != FREQ_CUSTOM_IDX) {
        int preset_idx = sel - 1;
        if (preset_idx >= 0 && preset_idx < FREQ_COUNT) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%.3f", FREQ_PRESETS[preset_idx]);
            lv_textarea_set_text(rx_freq_ta, buf);
        }
    }

    if (!pager_is_running()) {
        update_status();
        return;
    }
    start_from_ui();
    update_status();
}

static void on_rx_freq_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        // Tapping the text area implicitly selects Custom...
        lv_dropdown_set_selected(freq_dropdown, FREQ_CUSTOM_IDX);
        lv_keyboard_set_mode(rx_kb, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(rx_kb, rx_freq_ta);
        lv_obj_clear_flag(rx_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(rx_kb, LV_OBJ_FLAG_HIDDEN);
        // Apply the typed frequency immediately if RX is already running
        if (pager_is_running() &&
            (int)lv_dropdown_get_selected(freq_dropdown) == FREQ_CUSTOM_IDX) {
            start_from_ui();
            update_status();
        }
    }
}

static void on_rx_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(rx_kb);
        if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_obj_add_flag(rx_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_scan_toggled(lv_event_t *)
{
    bool want = lv_obj_has_state(scan_switch, LV_STATE_CHECKED);
    if (want) {
        bool ok = start_from_ui();
        if (!ok) {
            // Couldn't start — revert the switch. Flag a genuine radio error
            // (as opposed to LoRa holding the radio) for update_status().
            lv_obj_clear_state(scan_switch, LV_STATE_CHECKED);
            s_start_error = !lora_screen_is_powered();
        } else {
            s_start_error = false;
        }
    } else {
        pager_stop();
        s_start_error = false;
    }
    update_status();
    update_mode_buttons();
}

static void on_mode_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    PagerMode m = (PagerMode)idx;
    if (pager_is_running()) {
        // Preserve scanner mode across a mode-button change — otherwise
        // tapping FLEX while scanning would silently drop into single-freq.
        if (pager_is_scanning_all())
            pager_start_scanner(m);
        else
            pager_start(pager_get_freq(), m);
    } else {
        pager_set_mode(m);   // stored for the next time scan is toggled on
    }
    update_mode_buttons();
    update_status();
}

static void on_clear_btn(lv_event_t *)
{
    pager_clear_msgs();
    rebuild_msg_list();
}

// ---- TX modal --------------------------------------------------------------
//
// The modal is built once in pager_screen_create() and kept hidden until the
// user taps TX. It holds two text areas (capcode + message) sharing a single
// on-screen keyboard; the SEND button reads both fields, calls into the
// pager TX encoder, and reports the outcome in tx_status_label.

static void tx_modal_show()
{
    lv_obj_clear_flag(tx_modal, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(tx_status_label, "");
}

static void tx_modal_hide()
{
    lv_obj_add_flag(tx_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tx_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void on_tx_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        // Switch keyboard mode based on which TA is focused — numeric for
        // the capcode and freq fields, text for the message field. The
        // freq field uses NUMBER mode too so a stray letter can't sneak
        // in and break strtof's parse.
        bool numeric = (ta == tx_capcode_ta) || (ta == tx_freq_ta);
        lv_keyboard_set_mode(tx_keyboard,
            numeric ? LV_KEYBOARD_MODE_NUMBER
                    : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(tx_keyboard, ta);
        lv_obj_clear_flag(tx_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(tx_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

// Fired when the user picks an entry from the freq dropdown. "Custom..."
// (index 0) leaves the text area alone — they can type a value into it
// — and any preset auto-fills the text area with its MHz value. The
// text area stays the source of truth for the actual TX frequency.
static void on_tx_freq_dropdown_changed(lv_event_t *)
{
    int sel = (int)lv_dropdown_get_selected(tx_freq_dropdown);
    if (sel == TX_FREQ_CUSTOM_IDX) return;
    int preset_idx = sel - 1;
    const int count = (int)(sizeof(TX_FREQ_PRESETS) / sizeof(TX_FREQ_PRESETS[0]));
    if (preset_idx < 0 || preset_idx >= count) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%.3f", TX_FREQ_PRESETS[preset_idx]);
    lv_textarea_set_text(tx_freq_ta, buf);
}

static void on_tx_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(tx_keyboard);
        if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_obj_add_flag(tx_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_tx_send(lv_event_t *)
{
    const char *cap_str = lv_textarea_get_text(tx_capcode_ta);
    const char *msg_str = lv_textarea_get_text(tx_msg_ta);

    if (!cap_str || !*cap_str) {
        lv_label_set_text(tx_status_label, "Need capcode");
        lv_obj_set_style_text_color(tx_status_label,
            lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
        return;
    }
    uint32_t capcode = (uint32_t)strtoul(cap_str, NULL, 10);
    if (capcode == 0) {
        lv_label_set_text(tx_status_label, "Bad capcode");
        lv_obj_set_style_text_color(tx_status_label,
            lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
        return;
    }

    // Parse the TX frequency from the always-visible text area. strtof
    // tolerates trailing whitespace/garbage; we then range-check against
    // the SX1262's tuning window so the user gets a clear UI error
    // instead of a generic "Failed (err -12)".
    const char *freq_str = lv_textarea_get_text(tx_freq_ta);
    float freq_mhz = (freq_str && *freq_str) ? strtof(freq_str, NULL) : 0.0f;
    if (freq_mhz < 150.0f || freq_mhz > 960.0f) {
        lv_label_set_text(tx_status_label, "Freq must be 150-960 MHz");
        lv_obj_set_style_text_color(tx_status_label,
            lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
        return;
    }

    char tx_msg[40];
    snprintf(tx_msg, sizeof(tx_msg), "Sending %.3f MHz...", freq_mhz);
    lv_label_set_text(tx_status_label, tx_msg);
    lv_obj_set_style_text_color(tx_status_label,
        lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    // Force a paint so the label updates before the synchronous transmit
    // takes over the radio for ~half a second.
    lv_refr_now(NULL);

    PagerMode mode = pager_get_mode();
    bool ok;
    if (mode == PAGER_FLEX_1600) {
        ok = pager_transmit_flex(capcode, msg_str ? msg_str : "", freq_mhz);
    } else {
        // Default to alpha if there's text, tone-only if not — matches what
        // the user would naturally expect from leaving the message blank.
        PagerFunc fn = (msg_str && *msg_str) ? PFUNC_ALPHA : PFUNC_TONE;
        ok = pager_transmit_pocsag(capcode, fn, msg_str ? msg_str : "",
                                   mode, freq_mhz);
    }

    if (ok) {
        lv_label_set_text(tx_status_label, "Sent");
        lv_obj_set_style_text_color(tx_status_label,
            ARGUS_ACCENT, LV_PART_MAIN);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "Failed (err %d)", (int)pager_last_error());
        lv_label_set_text(tx_status_label, buf);
        lv_obj_set_style_text_color(tx_status_label,
            lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
    }
    // Reflect any post-TX state change (e.g. scan switch flipping if RX
    // failed to resume).
    update_status();
}

static void on_tx_close(lv_event_t *) { tx_modal_hide(); }
static void on_tx_btn(lv_event_t *)   { tx_modal_show(); }

static void on_update(lv_timer_t *)
{
    update_status();
    // Cache the last-seen message count so the (somewhat expensive) flex
    // rebuild only runs when there's something new, not every 500 ms tick.
    static int s_last_msg_count = -1;
    if (pager_screen_is_active()) {
        int n = pager_get_msg_count();
        if (n != s_last_msg_count) {
            rebuild_msg_list();
            s_last_msg_count = n;
        }
    } else {
        s_last_msg_count = -1;   // force a rebuild next time the screen opens
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        pager_stop();
        tools_screen_show();
    }
}

// ---- layout helpers --------------------------------------------------------

static lv_obj_t *make_mode_btn(lv_obj_t *parent, const char *text, int idx,
                                lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *btn = lv_obj_create(parent);
    // 76 wide × 44 tall — bumped from 64×36 to give a noticeably larger
    // finger target. Four side-by-side with 8 px gaps (4·76 + 3·8 = 328)
    // still fit inside the 410-diameter visible circle at the y row used
    // by the caller; corners verified at (41, 164) → ~186 px from centre.
    lv_obj_set_size(btn, 76, 44);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, on_mode_btn, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return btn;
}

// ---- public API ------------------------------------------------------------

void pager_screen_create()
{
    pager_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pager_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(pager_screen, 0, LV_PART_MAIN);

    // Title — font_48 to match the TPMS / SETTINGS headers. The dropdown
    // below sits at y=70 which leaves a 14 px gap clear of the bigger
    // title (font_48 extends to ~y=56 from the y=8 origin).
    lv_obj_t *title = lv_label_create(pager_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "PAGER");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // The visible window is a 410-diameter circle centred at (205, 251); at
    // the top and bottom the visible x range collapses fast, so widgets are
    // centred and stacked vertically rather than pushed to the bezel.

    // Frequency row — dropdown on the left (150 px), custom-freq text area
    // on the right (130 px), 8 px gap, total 288 px centered.  At y=70 the
    // circular bezel gives ~308 px of usable width so both fit comfortably.
    freq_dropdown = lv_dropdown_create(pager_screen);
    lv_dropdown_set_options(freq_dropdown, FREQ_LABELS);
    lv_obj_set_size(freq_dropdown, 150, 44);
    lv_obj_align(freq_dropdown, LV_ALIGN_TOP_MID, -69, 70);
    lv_obj_set_style_text_font(freq_dropdown, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(freq_dropdown, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(freq_dropdown, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(freq_dropdown, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(freq_dropdown, on_freq_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Custom frequency text area — always visible so the user can see the
    // active MHz value; becomes editable when "Custom..." is selected (or
    // tapped directly, which auto-selects "Custom...").
    rx_freq_ta = lv_textarea_create(pager_screen);
    lv_textarea_set_one_line(rx_freq_ta, true);
    lv_textarea_set_max_length(rx_freq_ta, 7);
    lv_textarea_set_accepted_chars(rx_freq_ta, "0123456789.");
    lv_textarea_set_placeholder_text(rx_freq_ta, "152.240");
    lv_textarea_set_text(rx_freq_ta, "152.240");
    lv_obj_set_size(rx_freq_ta, 130, 44);
    lv_obj_align(rx_freq_ta, LV_ALIGN_TOP_MID, 79, 70);
    lv_obj_set_style_text_font(rx_freq_ta, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rx_freq_ta, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(rx_freq_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_scroll_dir(rx_freq_ta, LV_DIR_HOR);
    lv_obj_add_event_cb(rx_freq_ta, on_rx_freq_ta_event, LV_EVENT_ALL, NULL);

    // Scan toggle + status on a single row at y=120 where ~316 px of
    // visible width opens up. Switch on the left, status text to the right.
    scan_switch = lv_switch_create(pager_screen);
    lv_obj_set_size(scan_switch, 60, 32);
    lv_obj_align(scan_switch, LV_ALIGN_TOP_MID, -90, 120);
    lv_obj_set_style_bg_color(scan_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scan_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(scan_switch, on_scan_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    status_label = lv_label_create(pager_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_label_set_text(status_label, "Off");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 50, 128);

    // Mode buttons — 4 × 76 px wide with 8 px gaps (4·76 + 3·8 = 328),
    // centred at y=164. Leftmost starts at x=41, rightmost ends at x=369;
    // the outermost corners sit ~186 px from the screen centre, well
    // inside the r=205 bezel.
    for (int i = 0; i < 4; i++) {
        mode_btns[i] = make_mode_btn(pager_screen, MODE_LABELS[i], i,
                                     41 + i * 84, 164);
    }

    // TX + CLEAR — bumped 60×28 → 80×40 for a much easier finger target.
    // Centred at y=220 with 16 px between them; offsets ±48 from centre
    // put their outer corners at (117, 220) and (293, 260), both well
    // inside the bezel.
    lv_obj_t *tx_btn = lv_obj_create(pager_screen);
    lv_obj_set_size(tx_btn, 80, 40);
    lv_obj_align(tx_btn, LV_ALIGN_TOP_MID, -48, 220);
    lv_obj_set_style_bg_color(tx_btn, lv_color_make(0x11, 0x44, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(tx_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tx_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tx_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tx_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tx_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tx_btn, on_tx_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tx_lbl = lv_label_create(tx_btn);
    lv_label_set_text(tx_lbl, "TX");
    lv_obj_set_style_text_color(tx_lbl, lv_color_make(0x66, 0xFF, 0xAA), LV_PART_MAIN);
    // Bumped font_14 → font_20 so the label scales with the bigger button.
    lv_obj_set_style_text_font(tx_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(tx_lbl);

    lv_obj_t *clear_btn = lv_obj_create(pager_screen);
    lv_obj_set_size(clear_btn, 80, 40);
    lv_obj_align(clear_btn, LV_ALIGN_TOP_MID, 48, 220);
    lv_obj_set_style_bg_color(clear_btn, lv_color_make(0x44, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_width(clear_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(clear_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(clear_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(clear_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clear_btn, on_clear_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clbl = lv_label_create(clear_btn);
    lv_label_set_text(clbl, "CLEAR");
    lv_obj_set_style_text_color(clbl, lv_color_make(0xFF, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_text_font(clbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(clbl);

    // Message list — 320 wide × 160 tall (height trimmed from 180 to make
    // room for the taller button rows above); ends at y=430, comfortably
    // above the visible bottom at y≈456.
    msg_list = lv_obj_create(pager_screen);
    lv_obj_set_size(msg_list, 320, 160);
    lv_obj_align(msg_list, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_set_style_bg_color(msg_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(msg_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(msg_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(msg_list, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(msg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(msg_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(msg_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(msg_list, LV_FLEX_FLOW_COLUMN);

    // On-screen keyboard for the custom RX frequency field.
    rx_kb = lv_keyboard_create(pager_screen);
    lv_obj_set_size(rx_kb, lv_pct(100), 200);
    lv_obj_align(rx_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(rx_kb, on_rx_kb_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(rx_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(pager_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // ---- TX modal -----------------------------------------------------
    //
    // Built once, hidden until invoked. Full-screen translucent backdrop
    // catches taps outside the panel (does nothing — user must use CLOSE)
    // and dims the underlying screen so the panel pops.

    tx_modal = lv_obj_create(pager_screen);
    lv_obj_set_size(tx_modal, lv_pct(100), lv_pct(100));
    lv_obj_align(tx_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(tx_modal, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tx_modal, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(tx_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tx_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tx_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tx_modal, LV_OBJ_FLAG_HIDDEN);

    tx_panel = lv_obj_create(tx_modal);
    // 350 tall (was 280) — extra 70 px holds the new "Frequency" row
    // above the capcode field without crowding the existing controls.
    lv_obj_set_size(tx_panel, 380, 350);
    // Centre the panel vertically on the screen rather than pinning it to
    // the top. The keyboard slides up from BOTTOM_MID on focus, so even
    // centred the panel still has the bottom half of the screen free for
    // the keyboard to occupy without overlapping the input fields.
    lv_obj_align(tx_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(tx_panel, lv_color_make(0x18, 0x18, 0x18), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tx_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tx_panel, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(tx_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(tx_panel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tx_panel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(tx_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Panel title
    lv_obj_t *tx_title = lv_label_create(tx_panel);
    lv_obj_set_style_text_color(tx_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(tx_title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(tx_title, "TRANSMIT");
    lv_obj_align(tx_title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Mode hint — reflects which encoder will be used (POCSAG bps / FLEX).
    // Updated in pager_screen_show() so it stays in sync with the mode
    // buttons on the underlying screen.
    lv_obj_t *tx_mode_hint = lv_label_create(tx_panel);
    lv_obj_set_style_text_color(tx_mode_hint, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(tx_mode_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(tx_mode_hint, "");
    lv_obj_align(tx_mode_hint, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_add_event_cb(pager_screen, [](lv_event_t *) {
        // Periodic refresh via the existing 2 s on_update timer would also
        // catch this, but the immediate refresh on every mode-button click
        // keeps the hint snappy.
    }, LV_EVENT_REFRESH, NULL);

    // Frequency row — dropdown on the left for the common preset list,
    // numeric text area on the right that's the source of truth for the
    // value actually passed to pager_transmit_*. Picking a preset auto-
    // fills the text area; picking "Custom..." (index 0) leaves it for
    // the user to edit; typing into the text area effectively means
    // "Custom..." regardless of what the dropdown last showed.
    lv_obj_t *freq_lbl = lv_label_create(tx_panel);
    lv_obj_set_style_text_color(freq_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(freq_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(freq_lbl, "Frequency (MHz)");
    lv_obj_align(freq_lbl, LV_ALIGN_TOP_LEFT, 0, 36);

    tx_freq_dropdown = lv_dropdown_create(tx_panel);
    lv_dropdown_set_options(tx_freq_dropdown, TX_FREQ_LABELS);
    lv_obj_set_size(tx_freq_dropdown, 200, 44);
    lv_obj_align(tx_freq_dropdown, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_text_font(tx_freq_dropdown, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tx_freq_dropdown, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(tx_freq_dropdown, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(tx_freq_dropdown, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(tx_freq_dropdown, on_tx_freq_dropdown_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    tx_freq_ta = lv_textarea_create(tx_panel);
    lv_textarea_set_one_line(tx_freq_ta, true);
    // 7 chars max → "999.999" — enough for the SX1262's 150–960 MHz range
    // at 3-decimal precision (the same precision the preset labels use).
    lv_textarea_set_max_length(tx_freq_ta, 7);
    lv_textarea_set_accepted_chars(tx_freq_ta, "0123456789.");
    lv_textarea_set_placeholder_text(tx_freq_ta, "152.240");
    lv_textarea_set_text(tx_freq_ta, "152.240");
    lv_obj_set_size(tx_freq_ta, 130, 44);
    lv_obj_align(tx_freq_ta, LV_ALIGN_TOP_RIGHT, 0, 58);
    lv_obj_set_style_text_font(tx_freq_ta, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tx_freq_ta, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(tx_freq_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_scroll_dir(tx_freq_ta, LV_DIR_HOR);
    lv_obj_add_event_cb(tx_freq_ta, on_tx_ta_event, LV_EVENT_ALL, NULL);

    // Capcode field — shifted down 70 px to clear the new frequency row.
    lv_obj_t *cap_lbl = lv_label_create(tx_panel);
    lv_obj_set_style_text_color(cap_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap_lbl, "Capcode");
    lv_obj_align(cap_lbl, LV_ALIGN_TOP_LEFT, 0, 106);

    tx_capcode_ta = lv_textarea_create(tx_panel);
    lv_textarea_set_one_line(tx_capcode_ta, true);
    lv_textarea_set_max_length(tx_capcode_ta, 7);
    lv_textarea_set_accepted_chars(tx_capcode_ta, "0123456789");
    lv_textarea_set_placeholder_text(tx_capcode_ta, "1234567");
    // Height bumped 34 → 44 — at 34 the font_16 glyphs + textarea padding
    // + the blinking cursor exceed the container height by a couple of
    // pixels, which makes LVGL animate the scroll up/down every cursor
    // blink. 44 leaves enough headroom for the cursor not to cause that.
    lv_obj_set_size(tx_capcode_ta, 200, 44);
    lv_obj_set_style_text_font(tx_capcode_ta, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tx_capcode_ta, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(tx_capcode_ta, lv_color_white(), LV_PART_MAIN);
    // Horizontal-only scroll — for a one-line numeric field there's never
    // anything to scroll vertically, and disabling that axis kills the
    // residual jitter even if the height ever ends up borderline.
    lv_obj_set_scroll_dir(tx_capcode_ta, LV_DIR_HOR);
    lv_obj_align(tx_capcode_ta, LV_ALIGN_TOP_LEFT, 0, 128);
    lv_obj_add_event_cb(tx_capcode_ta, on_tx_ta_event, LV_EVENT_ALL, NULL);

    // Message field — shifted down with the rest.
    lv_obj_t *msg_lbl = lv_label_create(tx_panel);
    lv_obj_set_style_text_color(msg_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(msg_lbl, "Message");
    lv_obj_align(msg_lbl, LV_ALIGN_TOP_LEFT, 0, 172);

    tx_msg_ta = lv_textarea_create(tx_panel);
    lv_textarea_set_one_line(tx_msg_ta, true);
    // A single POCSAG batch can carry ~30 alpha chars in 15 message codewords
    // (15 × 20 bits ÷ 7 bits/char ≈ 42, minus EOT padding). Cap at 40 to
    // give a little headroom.
    lv_textarea_set_max_length(tx_msg_ta, 40);
    lv_textarea_set_placeholder_text(tx_msg_ta, "Page text (leave empty for tone-only)");
    // Same height bump (34 → 44) + horizontal-only scroll as the capcode
    // field above so cursor-blink jitter doesn't show up here either.
    lv_obj_set_size(tx_msg_ta, 356, 44);
    lv_obj_set_scroll_dir(tx_msg_ta, LV_DIR_HOR);
    lv_obj_set_style_text_font(tx_msg_ta, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tx_msg_ta, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(tx_msg_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(tx_msg_ta, LV_ALIGN_TOP_LEFT, 0, 194);
    lv_obj_add_event_cb(tx_msg_ta, on_tx_ta_event, LV_EVENT_ALL, NULL);

    // SEND + CLOSE buttons — shifted down with the rest of the fields.
    lv_obj_t *send_btn = lv_obj_create(tx_panel);
    lv_obj_set_size(send_btn, 120, 44);
    lv_obj_align(send_btn, LV_ALIGN_TOP_LEFT, 0, 248);
    lv_obj_set_style_bg_color(send_btn, lv_color_make(0x00, 0x88, 0x44), LV_PART_MAIN);
    lv_obj_set_style_radius(send_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(send_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(send_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(send_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(send_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(send_btn, on_tx_send, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, "SEND");
    lv_obj_set_style_text_color(send_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(send_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(send_lbl);

    lv_obj_t *close_btn = lv_obj_create(tx_panel);
    lv_obj_set_size(close_btn, 120, 44);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 248);
    lv_obj_set_style_bg_color(close_btn, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_radius(close_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(close_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(close_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, on_tx_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "CLOSE");
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(close_lbl);

    // Inline status line for "Sending..." / "Sent" / "Failed (err N)"
    tx_status_label = lv_label_create(tx_panel);
    lv_obj_set_style_text_font(tx_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(tx_status_label, "");
    lv_obj_align(tx_status_label, LV_ALIGN_TOP_LEFT, 0, 306);

    // Shared on-screen keyboard for both text areas. Lives on the modal so
    // it sits above the panel; the textarea's focus events show/hide it.
    tx_keyboard = lv_keyboard_create(tx_modal);
    lv_obj_set_size(tx_keyboard, lv_pct(100), 180);
    lv_obj_align(tx_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(tx_keyboard, on_tx_kb_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(tx_keyboard, LV_OBJ_FLAG_HIDDEN);

    update_mode_buttons();
    update_status();
    rebuild_msg_list();

    // 500 ms instead of 2 s — during multi-freq scanning the channel hops
    // every ~300 ms and a 2 s status refresh feels noticeably stale.
    lv_timer_create(on_update, 500, NULL);
}

void pager_screen_show()
{
    s_start_error = false;
    // The scanner may have been stopped elsewhere (e.g. LoRa claimed the
    // shared radio) — sync the switch to the real state rather than trusting
    // its last visual position.
    if (pager_is_running())
        lv_obj_add_state(scan_switch, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(scan_switch, LV_STATE_CHECKED);
    update_mode_buttons();
    update_status();
    lv_scr_load(pager_screen);
}

bool pager_screen_is_active()
{
    return lv_screen_active() == pager_screen;
}
