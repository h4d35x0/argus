#include "configuration_screen.h"
#include "theme.h"
#include "channels_screen.h"
#include "meshtastic.h"
#include "gps_screen.h"
#include "map_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdlib.h>

// Defined in send_message_screen.cpp
void send_message_screen_show();
// Defined in main.cpp
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *cfg_screen;
static lv_obj_t *long_ta;
static lv_obj_t *short_ta;
static lv_obj_t *keyboard;

static lv_obj_t *gps_status_row;
static lv_obj_t *gps_status_icon;
static lv_obj_t *broadcast_row;
static lv_obj_t *broadcast_switch;
static bool      s_broadcast_enabled = false;
static lv_obj_t *interval_row;
static lv_obj_t *interval_dropdown;
static lv_obj_t *rebroadcast_row;
static lv_obj_t *rebroadcast_switch;
static bool      s_rebroadcast_enabled = false;
// Haptic notifications on incoming TEXT messages. Defaults on so the
// user feels new traffic out of the box; can be flipped off here if
// the broadcast vibrate ends up too noisy.
static lv_obj_t *vib_dm_row;
static lv_obj_t *vib_dm_switch;
static bool      s_vibrate_dm = true;
static lv_obj_t *vib_bc_row;
static lv_obj_t *vib_bc_switch;
static bool      s_vibrate_broadcast = true;
// Announce Node to the Mesh — toggle + interval dropdown. Default ON
// at 10 minutes so a fresh boot shows up in nearby node lists.
static lv_obj_t *announce_row;
static lv_obj_t *announce_switch;
static lv_obj_t *announce_int_row;
static lv_obj_t *announce_int_dropdown;
static bool      s_announce_enabled = true;

// Indexed by dropdown selection. 0 ("Once") means broadcast a single position
// then stop; non-zero values are the ms between repeated broadcasts.
static const uint32_t BROADCAST_INTERVAL_MS[] = {
    0,         // Once
    30000,     // Every 30 seconds
    120000,    // Every 2 minutes  (default)
    300000,    // Every 5 minutes
    600000,    // Every 10 minutes
    1200000,   // Every 20 minutes
    3600000,   // Every Hour
};

// NodeInfo announce intervals. Matches the P4's set: 1m / 5m / 10m / 15m.
// Default is index 2 (10 minutes) - same as meshtastic.cpp's static
// s_announce_interval initialiser.
static const uint32_t ANNOUNCE_INTERVAL_MS[] = {
    60000,     // 1 Minute
    300000,    // 5 Minutes
    600000,    // 10 Minutes (default)
    900000,    // 15 Minutes
};
static const int ANNOUNCE_DEFAULT_IDX = 2;

// Re-entry guard for the SD load path. Setting widget values from
// load_config_from_sd() fires VALUE_CHANGED on the switches and the
// dropdown; those handlers normally call save_config_to_sd(). With
// s_loading true the save is suppressed.
static bool s_loading = false;

// Forward decl - defined alongside the announce handlers below but
// referenced by apply_config_kv() above them.
static void commit_announce();

void configuration_screen_commit()
{
    meshtastic_set_long_name(lv_textarea_get_text(long_ta));
    meshtastic_set_short_name(lv_textarea_get_text(short_ta));
}

// Persist every Meshtastic-config screen value (plus the long/short
// name from the meshtastic backend) to /Meshtastic/config.txt as
// human-readable key=value pairs. Called from every "value changed"
// handler on this screen so a reboot loses nothing.
static void save_config_to_sd()
{
    if (s_loading || !instance.isCardReady()) return;
    if (!SD.exists("/Meshtastic")) SD.mkdir("/Meshtastic");
    File f = SD.open("/Meshtastic/config.txt", FILE_WRITE);
    if (!f) return;
    f.printf("long_name=%s\n",  meshtastic_get_long_name());
    f.printf("short_name=%s\n", meshtastic_get_short_name());
    f.printf("broadcast=%d\n",  s_broadcast_enabled ? 1 : 0);
    f.printf("interval_idx=%u\n",
        (unsigned)lv_dropdown_get_selected(interval_dropdown));
    f.printf("rebroadcast=%d\n", s_rebroadcast_enabled ? 1 : 0);
    f.printf("vibrate_dm=%d\n",        s_vibrate_dm        ? 1 : 0);
    f.printf("vibrate_broadcast=%d\n", s_vibrate_broadcast ? 1 : 0);
    f.printf("announce=%d\n", s_announce_enabled ? 1 : 0);
    f.printf("announce_interval_idx=%u\n",
        (unsigned)lv_dropdown_get_selected(announce_int_dropdown));
    f.close();
}

// Apply a single key=value pair from the config file. Called once per
// non-blank line by load_config_from_sd. Unknown keys are ignored so
// old config files keep working after schema additions.
static void apply_config_kv(const char *key, const char *val)
{
    if (strcmp(key, "long_name") == 0) {
        meshtastic_set_long_name(val);
        if (long_ta) lv_textarea_set_text(long_ta, val);
    } else if (strcmp(key, "short_name") == 0) {
        meshtastic_set_short_name(val);
        if (short_ta) lv_textarea_set_text(short_ta, val);
    } else if (strcmp(key, "broadcast") == 0) {
        s_broadcast_enabled = (strtol(val, nullptr, 10) != 0);
        if (broadcast_switch) {
            if (s_broadcast_enabled) lv_obj_add_state   (broadcast_switch, LV_STATE_CHECKED);
            else                     lv_obj_clear_state (broadcast_switch, LV_STATE_CHECKED);
        }
    } else if (strcmp(key, "interval_idx") == 0) {
        uint32_t idx = (uint32_t)strtoul(val, nullptr, 10);
        if (idx < sizeof(BROADCAST_INTERVAL_MS) / sizeof(BROADCAST_INTERVAL_MS[0])
            && interval_dropdown) {
            lv_dropdown_set_selected(interval_dropdown, idx);
        }
    } else if (strcmp(key, "rebroadcast") == 0) {
        s_rebroadcast_enabled = (strtol(val, nullptr, 10) != 0);
        if (rebroadcast_switch) {
            if (s_rebroadcast_enabled) lv_obj_add_state   (rebroadcast_switch, LV_STATE_CHECKED);
            else                       lv_obj_clear_state (rebroadcast_switch, LV_STATE_CHECKED);
        }
    } else if (strcmp(key, "vibrate_dm") == 0) {
        s_vibrate_dm = (strtol(val, nullptr, 10) != 0);
        if (vib_dm_switch) {
            if (s_vibrate_dm) lv_obj_add_state   (vib_dm_switch, LV_STATE_CHECKED);
            else              lv_obj_clear_state (vib_dm_switch, LV_STATE_CHECKED);
        }
    } else if (strcmp(key, "vibrate_broadcast") == 0) {
        s_vibrate_broadcast = (strtol(val, nullptr, 10) != 0);
        if (vib_bc_switch) {
            if (s_vibrate_broadcast) lv_obj_add_state   (vib_bc_switch, LV_STATE_CHECKED);
            else                     lv_obj_clear_state (vib_bc_switch, LV_STATE_CHECKED);
        }
    } else if (strcmp(key, "announce") == 0) {
        s_announce_enabled = (strtol(val, nullptr, 10) != 0);
        if (announce_switch) {
            if (s_announce_enabled) lv_obj_add_state   (announce_switch, LV_STATE_CHECKED);
            else                    lv_obj_clear_state (announce_switch, LV_STATE_CHECKED);
        }
        // Mirror the new state into meshtastic:: on the next pass; the
        // interval may still be the old default until apply_config_kv
        // sees announce_interval_idx, but commit_announce uses whatever
        // is currently selected in the dropdown.
        if (announce_int_dropdown) commit_announce();
    } else if (strcmp(key, "announce_interval_idx") == 0) {
        uint32_t idx = (uint32_t)strtoul(val, nullptr, 10);
        if (idx < sizeof(ANNOUNCE_INTERVAL_MS) / sizeof(ANNOUNCE_INTERVAL_MS[0])
            && announce_int_dropdown) {
            lv_dropdown_set_selected(announce_int_dropdown, idx);
            commit_announce();
        }
    }
}

static void load_config_from_sd()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/Meshtastic/config.txt")) return;
    File f = SD.open("/Meshtastic/config.txt", FILE_READ);
    if (!f) return;
    s_loading = true;
    char line[96];
    while (f.available()) {
        // Read one line (newline-terminated). Tolerates CRLF / CR / LF.
        size_t n = 0;
        while (f.available() && n < sizeof(line) - 1) {
            int c = f.read();
            if (c < 0) break;
            if (c == '\n') break;
            if (c == '\r') continue;
            line[n++] = (char)c;
        }
        line[n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        apply_config_kv(line, eq + 1);
    }
    f.close();
    s_loading = false;
}

static void show_keyboard(lv_obj_t *ta)
{
    lv_keyboard_set_textarea(keyboard, ta);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    // The keyboard occupies the bottom ~240px; hide the GPS rows underneath
    // so the user isn't presented with half-clipped widgets while typing.
    lv_obj_add_flag(gps_status_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(broadcast_row,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(interval_row,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rebroadcast_row, LV_OBJ_FLAG_HIDDEN);
    if (vib_dm_row) lv_obj_add_flag(vib_dm_row, LV_OBJ_FLAG_HIDDEN);
    if (vib_bc_row) lv_obj_add_flag(vib_bc_row, LV_OBJ_FLAG_HIDDEN);
    if (announce_row)     lv_obj_add_flag(announce_row,     LV_OBJ_FLAG_HIDDEN);
    if (announce_int_row) lv_obj_add_flag(announce_int_row, LV_OBJ_FLAG_HIDDEN);
}

static void hide_keyboard()
{
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gps_status_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(broadcast_row,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(interval_row,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(rebroadcast_row, LV_OBJ_FLAG_HIDDEN);
    if (vib_dm_row) lv_obj_clear_flag(vib_dm_row, LV_OBJ_FLAG_HIDDEN);
    if (vib_bc_row) lv_obj_clear_flag(vib_bc_row, LV_OBJ_FLAG_HIDDEN);
    if (announce_row)     lv_obj_clear_flag(announce_row,     LV_OBJ_FLAG_HIDDEN);
    if (announce_int_row) lv_obj_clear_flag(announce_int_row, LV_OBJ_FLAG_HIDDEN);
}

static void on_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        show_keyboard(ta);
    } else if (code == LV_EVENT_DEFOCUSED) {
        configuration_screen_commit();
        save_config_to_sd();
        hide_keyboard();
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        configuration_screen_commit();
        save_config_to_sd();
        lv_obj_t *ta = lv_keyboard_get_textarea(keyboard);
        if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        hide_keyboard();
    }
}

static void on_broadcast_changed(lv_event_t *e)
{
    s_broadcast_enabled = lv_obj_has_state(broadcast_switch, LV_STATE_CHECKED);
    save_config_to_sd();
}

static void on_rebroadcast_changed(lv_event_t *e)
{
    s_rebroadcast_enabled = lv_obj_has_state(rebroadcast_switch, LV_STATE_CHECKED);
    save_config_to_sd();
}

static void on_interval_changed(lv_event_t *)
{
    save_config_to_sd();
}

static void on_vibrate_dm_changed(lv_event_t *)
{
    s_vibrate_dm = lv_obj_has_state(vib_dm_switch, LV_STATE_CHECKED);
    save_config_to_sd();
}

static void on_vibrate_broadcast_changed(lv_event_t *)
{
    s_vibrate_broadcast = lv_obj_has_state(vib_bc_switch, LV_STATE_CHECKED);
    save_config_to_sd();
}

// Push the announce switch + interval dropdown state into meshtastic::.
// Disables the dropdown while the toggle is off so it's visually obvious
// the interval is moot.
static void commit_announce()
{
    bool on = lv_obj_has_state(announce_switch, LV_STATE_CHECKED);
    uint32_t idx = lv_dropdown_get_selected(announce_int_dropdown);
    if (idx >= sizeof(ANNOUNCE_INTERVAL_MS) / sizeof(ANNOUNCE_INTERVAL_MS[0])) {
        idx = ANNOUNCE_DEFAULT_IDX;
    }
    meshtastic_set_announce(on, ANNOUNCE_INTERVAL_MS[idx]);
    if (on)  lv_obj_clear_state (announce_int_dropdown, LV_STATE_DISABLED);
    else     lv_obj_add_state   (announce_int_dropdown, LV_STATE_DISABLED);
}

static void on_announce_changed(lv_event_t *)
{
    s_announce_enabled = lv_obj_has_state(announce_switch, LV_STATE_CHECKED);
    commit_announce();
    save_config_to_sd();
}

static void on_announce_interval_changed(lv_event_t *)
{
    commit_announce();
    save_config_to_sd();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        configuration_screen_commit();
        save_config_to_sd();
        // Reverse chain: Configuration -> Map (when /map tiles exist on
        // the SD card) -> Send Message. Skip straight back to send when
        // there are no tiles.
        if (map_screen_available()) map_screen_show();
        else                        send_message_screen_show();
    }
}

static lv_obj_t *make_field(const char *caption, int y, int max_len)
{
    lv_obj_t *lbl = lv_label_create(cfg_screen);
    lv_obj_set_style_text_color(lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, caption);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, -185, y);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(cfg_screen);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_size(ta, 380, 48);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, y + 28);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(ta, on_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_ta_event, LV_EVENT_DEFOCUSED, NULL);
    return ta;
}

void configuration_screen_create()
{
    cfg_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cfg_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(cfg_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(cfg_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "CONFIGURATION");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    long_ta  = make_field("Long Name",  60,  MESH_MAX_LONG_NAME  - 1);
    short_ta = make_field("Short Name", 150, MESH_MAX_SHORT_NAME - 1);

    // On-screen keyboard — hidden until a field is focused
    keyboard = lv_keyboard_create(cfg_screen);
    lv_obj_set_size(keyboard, 410, 240);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, on_kb_event, LV_EVENT_CANCEL, NULL);

    // GPS status row — mirror the WARDRIVER screen's big-icon style
    gps_status_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(gps_status_row, 380, 60);
    lv_obj_set_style_bg_opa(gps_status_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(gps_status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gps_status_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gps_status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gps_status_row, LV_ALIGN_TOP_MID, 0, 240);

    lv_obj_t *gps_lbl = lv_label_create(gps_status_row);
    lv_obj_set_style_text_color(gps_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(gps_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(gps_lbl, "GPS");
    lv_obj_align(gps_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    gps_status_icon = lv_label_create(gps_status_row);
    lv_obj_set_style_text_font(gps_status_icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(gps_status_icon, lv_color_make(0xFF, 0x33, 0x33), LV_PART_MAIN);
    lv_label_set_text(gps_status_icon, LV_SYMBOL_CLOSE);
    lv_obj_align(gps_status_icon, LV_ALIGN_RIGHT_MID, 0, 0);

    // Broadcast Location row — locked until GPS reports a fix
    broadcast_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(broadcast_row, 380, 55);
    lv_obj_set_style_bg_opa(broadcast_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(broadcast_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(broadcast_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(broadcast_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(broadcast_row, LV_ALIGN_TOP_MID, 0, 310);

    lv_obj_t *bc_lbl = lv_label_create(broadcast_row);
    lv_obj_set_style_text_color(bc_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(bc_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(bc_lbl, "Broadcast Location");
    lv_obj_align(bc_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    broadcast_switch = lv_switch_create(broadcast_row);
    lv_obj_set_size(broadcast_switch, 80, 40);
    lv_obj_set_style_bg_color(broadcast_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(broadcast_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(broadcast_switch, LV_STATE_DISABLED);  // locked until GPS fix
    lv_obj_add_event_cb(broadcast_switch, on_broadcast_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(broadcast_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Broadcast Interval dropdown — independent of GPS lock (user can preset)
    interval_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(interval_row, 380, 55);
    lv_obj_set_style_bg_opa(interval_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(interval_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(interval_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(interval_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(interval_row, LV_ALIGN_TOP_MID, 0, 375);

    lv_obj_t *iv_lbl = lv_label_create(interval_row);
    lv_obj_set_style_text_color(iv_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(iv_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(iv_lbl, "Broadcast Interval");
    lv_obj_align(iv_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    interval_dropdown = lv_dropdown_create(interval_row);
    lv_dropdown_set_options(interval_dropdown,
        "Once\n"
        "Every 30 seconds\n"
        "Every 2 minutes\n"
        "Every 5 minutes\n"
        "Every 10 minutes\n"
        "Every 20 minutes\n"
        "Every Hour");
    lv_dropdown_set_selected(interval_dropdown, 2);   // default: Every 2 minutes
    lv_obj_add_event_cb(interval_dropdown, on_interval_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_size(interval_dropdown, 195, 38);
    lv_obj_set_style_text_font(interval_dropdown, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(interval_dropdown, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(interval_dropdown, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(interval_dropdown, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(interval_dropdown, 1, LV_PART_MAIN);
    // Style the drop-down list popup too
    lv_obj_t *iv_list = lv_dropdown_get_list(interval_dropdown);
    lv_obj_set_style_bg_color(iv_list, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(iv_list, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(iv_list, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_border_color(iv_list, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_align(interval_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

    // Rebroadcast Packets toggle — relay other nodes' packets onto the mesh
    rebroadcast_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(rebroadcast_row, 380, 55);
    lv_obj_set_style_bg_opa(rebroadcast_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(rebroadcast_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rebroadcast_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rebroadcast_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rebroadcast_row, LV_ALIGN_TOP_MID, 0, 435);

    lv_obj_t *rb_lbl = lv_label_create(rebroadcast_row);
    lv_obj_set_style_text_color(rb_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(rb_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(rb_lbl, "Rebroadcast Packets");
    lv_obj_align(rb_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    rebroadcast_switch = lv_switch_create(rebroadcast_row);
    lv_obj_set_size(rebroadcast_switch, 80, 40);
    lv_obj_set_style_bg_color(rebroadcast_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(rebroadcast_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    // default off
    lv_obj_add_event_cb(rebroadcast_switch, on_rebroadcast_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(rebroadcast_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Vibrate on incoming DM - sits below the rebroadcast row. The
    // watch is round and these last rows live below the visible disc;
    // the user scrolls the content to reach them. Default ON.
    vib_dm_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(vib_dm_row, 380, 55);
    lv_obj_set_style_bg_opa(vib_dm_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(vib_dm_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(vib_dm_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(vib_dm_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(vib_dm_row, LV_ALIGN_TOP_MID, 0, 495);

    lv_obj_t *vd_lbl = lv_label_create(vib_dm_row);
    lv_obj_set_style_text_color(vd_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(vd_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(vd_lbl, "Vibrate on DM");
    lv_obj_align(vd_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    vib_dm_switch = lv_switch_create(vib_dm_row);
    lv_obj_set_size(vib_dm_switch, 80, 40);
    lv_obj_set_style_bg_color(vib_dm_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(vib_dm_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(vib_dm_switch, LV_STATE_CHECKED);     // default ON
    lv_obj_add_event_cb(vib_dm_switch, on_vibrate_dm_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(vib_dm_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Vibrate on incoming broadcast - sits below the DM toggle.
    vib_bc_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(vib_bc_row, 380, 55);
    lv_obj_set_style_bg_opa(vib_bc_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(vib_bc_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(vib_bc_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(vib_bc_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(vib_bc_row, LV_ALIGN_TOP_MID, 0, 555);

    lv_obj_t *vb_lbl = lv_label_create(vib_bc_row);
    lv_obj_set_style_text_color(vb_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(vb_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(vb_lbl, "Vibrate on Broadcast");
    lv_obj_align(vb_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    vib_bc_switch = lv_switch_create(vib_bc_row);
    lv_obj_set_size(vib_bc_switch, 80, 40);
    lv_obj_set_style_bg_color(vib_bc_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(vib_bc_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(vib_bc_switch, LV_STATE_CHECKED);     // default ON
    lv_obj_add_event_cb(vib_bc_switch, on_vibrate_broadcast_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(vib_bc_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Announce Node to the Mesh toggle. When enabled, bg_tick blasts
    // a NodeInfo (and Telemetry) packet at the cadence selected below
    // so other Meshtastic nodes can populate their node list with our
    // friendly name instead of falling back to "!hexnodeid".
    announce_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(announce_row, 380, 55);
    lv_obj_set_style_bg_opa(announce_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(announce_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(announce_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(announce_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(announce_row, LV_ALIGN_TOP_MID, 0, 615);

    lv_obj_t *an_lbl = lv_label_create(announce_row);
    lv_obj_set_style_text_color(an_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(an_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(an_lbl, "Announce Node");
    lv_obj_align(an_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    announce_switch = lv_switch_create(announce_row);
    lv_obj_set_size(announce_switch, 80, 40);
    lv_obj_set_style_bg_color(announce_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(announce_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(announce_switch, LV_STATE_CHECKED);     // default ON
    lv_obj_add_event_cb(announce_switch, on_announce_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(announce_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    // Announce Interval dropdown — 1m / 5m / 10m / 15m, default 10m.
    announce_int_row = lv_obj_create(cfg_screen);
    lv_obj_set_size(announce_int_row, 380, 55);
    lv_obj_set_style_bg_opa(announce_int_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(announce_int_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(announce_int_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(announce_int_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(announce_int_row, LV_ALIGN_TOP_MID, 0, 675);

    lv_obj_t *ai_lbl = lv_label_create(announce_int_row);
    lv_obj_set_style_text_color(ai_lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(ai_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(ai_lbl, "Announce Interval");
    lv_obj_align(ai_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    announce_int_dropdown = lv_dropdown_create(announce_int_row);
    lv_dropdown_set_options(announce_int_dropdown,
        "1 Minute\n"
        "5 Minutes\n"
        "10 Minutes\n"
        "15 Minutes");
    lv_dropdown_set_selected(announce_int_dropdown, ANNOUNCE_DEFAULT_IDX);
    lv_obj_add_event_cb(announce_int_dropdown, on_announce_interval_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_size(announce_int_dropdown, 175, 38);
    lv_obj_set_style_text_font(announce_int_dropdown, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(announce_int_dropdown, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color(announce_int_dropdown, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(announce_int_dropdown, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(announce_int_dropdown, 1, LV_PART_MAIN);
    lv_obj_t *ai_list = lv_dropdown_get_list(announce_int_dropdown);
    lv_obj_set_style_bg_color   (ai_list, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_text_color (ai_list, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font  (ai_list, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_border_color(ai_list, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_align(announce_int_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);

    // "Channels >" link card - opens the channels sub-screen where
    // the user can enable / set-active the 4 channel slots. Sits
    // below the vibrate toggles in the scroll region; user flicks up
    // to reach it on the round 410-disc.
    lv_obj_t *ch_link = lv_obj_create(cfg_screen);
    lv_obj_set_size(ch_link, 380, 55);
    lv_obj_align(ch_link, LV_ALIGN_TOP_MID, 0, 735);
    lv_obj_set_style_bg_color(ch_link, lv_color_make(0x10, 0x20, 0x30), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ch_link, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(ch_link, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ch_link, 16, LV_PART_MAIN);
    lv_obj_set_style_border_color(ch_link, lv_color_make(0x0A, 0x84, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ch_link, 1, LV_PART_MAIN);
    lv_obj_clear_flag(ch_link, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ch_link, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ch_link, [](lv_event_t *) { channels_screen_show(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *ch_lbl = lv_label_create(ch_link);
    lv_label_set_text(ch_lbl, "Channels");
    lv_obj_set_style_text_color(ch_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(ch_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *ch_chev = lv_label_create(ch_link);
    lv_label_set_text(ch_chev, ">");
    lv_obj_set_style_text_color(ch_chev, lv_color_make(0x0A, 0x84, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ch_chev, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(ch_chev, LV_ALIGN_RIGHT_MID, 0, 0);

    // Invisible spacer below the Channels link to extend the scroll
    // range so the card stays reachable above the bottom bezel.
    // Channels now lives at y=735 (after the two new Announce rows),
    // so the pad moves down by the same 120 px (was y=830).
    lv_obj_t *scroll_pad = lv_obj_create(cfg_screen);
    lv_obj_set_size(scroll_pad, 1, 1);
    lv_obj_align(scroll_pad, LV_ALIGN_TOP_MID, 0, 950);
    lv_obj_set_style_bg_opa(scroll_pad, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_pad, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scroll_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scroll_pad, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(cfg_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Pull persisted values off the SD card and seed every widget +
    // backend state. s_loading suppresses the VALUE_CHANGED save-storm
    // that would otherwise re-write the same values back as we apply
    // them. Safe to call when no card is mounted - the load just bails.
    load_config_from_sd();
}

void configuration_screen_show()
{
    main_loop_request_lvgl_priority(12);
    // Sync fields with current meshtastic values
    lv_textarea_set_text(long_ta,  meshtastic_get_long_name());
    lv_textarea_set_text(short_ta, meshtastic_get_short_name());
    hide_keyboard();
    configuration_screen_update();   // reflect current GPS state on entry
    lv_scr_load(cfg_screen);
}

bool configuration_screen_is_active()
{
    return lv_screen_active() == cfg_screen;
}

void configuration_screen_update()
{
    bool lock = gps_screen_has_lock();
    lv_label_set_text(gps_status_icon, lock ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(gps_status_icon,
        lock ? lv_color_make(0x00, 0xCC, 0x44) : lv_color_make(0xFF, 0x33, 0x33),
        LV_PART_MAIN);

    // Without a lock the switch is locked out — keep its checked state intact
    // so the user's intent is preserved when GPS reacquires.
    if (lock) lv_obj_clear_state(broadcast_switch, LV_STATE_DISABLED);
    else      lv_obj_add_state(broadcast_switch,   LV_STATE_DISABLED);
}

bool configuration_screen_get_broadcast_location()
{
    return s_broadcast_enabled;
}

uint32_t configuration_screen_get_broadcast_interval_ms()
{
    uint32_t idx = lv_dropdown_get_selected(interval_dropdown);
    const uint32_t n = sizeof(BROADCAST_INTERVAL_MS) / sizeof(BROADCAST_INTERVAL_MS[0]);
    if (idx >= n) idx = 0;
    return BROADCAST_INTERVAL_MS[idx];
}

bool configuration_screen_get_rebroadcast_enabled()
{
    return s_rebroadcast_enabled;
}

bool configuration_screen_get_vibrate_dm()         { return s_vibrate_dm; }
bool configuration_screen_get_vibrate_broadcast()  { return s_vibrate_broadcast; }
