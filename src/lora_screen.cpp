#include "lora_screen.h"
#include "tools_screen.h"
#include "theme.h"
#include "meshtastic.h"
#include "pager.h"
#include "tpms.h"
#include "aprs.h"
#include <LilyGoLib.h>
#include <SD.h>
#include "usb_sd.h"

// Defined in main.cpp
void clock_screen_set_lora_active(bool active);

static lv_obj_t *lora_screen;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;

static lv_obj_t *val_node_id;
static lv_obj_t *val_frequency;
static lv_obj_t *val_bandwidth;
static lv_obj_t *val_rssi;
static lv_obj_t *val_cad;
static lv_obj_t *val_tx_power;
static lv_obj_t *val_packet_count;

static lv_obj_t *boosted_switch;
static bool      s_boosted_gain_enabled = true;

static bool lora_powered = false;

// Persist the radio on/off state so LoRa survives a reboot. Mirrors the
// small-file pattern used by gps_screen.cpp.
#define LORA_PATH "/Settings/lora.txt"

static void lora_save_power(bool on)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return; // host owns SD when mounted
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(LORA_PATH, FILE_WRITE);   // FILE_WRITE = truncate
    if (!f) return;
    f.printf("%d\n", on ? 1 : 0);
    f.close();
}

static void update_status()
{
    lv_label_set_text(status_label, lora_powered ? "Radio: ON" : "Radio: OFF");
}

static void make_data_row(lv_obj_t *parent, const char *field, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_set_style_bg_color(row, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(lbl, field);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(val, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    *val_out = val;
}

static void on_lora_update(lv_timer_t *timer)
{
    // Skip the whole tick when the screen isn't visible — none of the
    // label updates below are observable, and the work to build them
    // (snprintf, meshtastic_get_*) is otherwise burned every second
    // even on the clock face. The lvgl timer still fires; it just no-ops.
    if (!lora_screen_is_active()) return;

    if (!lora_powered) {
        lv_label_set_text(val_node_id,      "--");
        lv_label_set_text(val_frequency,    "--");
        lv_label_set_text(val_bandwidth,    "--");
        lv_label_set_text(val_rssi,         "--");
        lv_label_set_text(val_cad,          "--");
        lv_label_set_text(val_tx_power,     "--");
        lv_label_set_text(val_packet_count, "--");
        return;
    }

    char buf[32];

    // Node ID (derived from MAC, fixed for this device)
    snprintf(buf, sizeof(buf), "!%08lx", (unsigned long)meshtastic_get_node_id());
    lv_label_set_text(val_node_id, buf);

    // Static Meshtastic LongFast config
    lv_label_set_text(val_frequency, "906.9 MHz");
    lv_label_set_text(val_bandwidth, "250 kHz");
    lv_label_set_text(val_tx_power,  "10 dBm");

    // Live values from meshtastic module
    snprintf(buf, sizeof(buf), "%.0f dBm", meshtastic_get_rssi());
    lv_label_set_text(val_rssi, buf);

    lv_label_set_text(val_cad, meshtastic_get_cad() ? "Active" : "Clear");

    snprintf(buf, sizeof(buf), "%d", meshtastic_get_rx_count());
    lv_label_set_text(val_packet_count, buf);
}

static void on_toggle(lv_event_t *e)
{
    lora_powered = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    if (lora_powered) {
        // The SX1262 is shared. Stop any FSK scanner before LoRa claims the
        // radio, otherwise its background tick keeps polling and steals
        // IRQs/packets from meshtastic.
        pager_stop();
        tpms_stop();
        aprs_stop();
    }
    instance.powerControl(POWER_RADIO, lora_powered);
    if (lora_powered) {
        instance.initLoRa();
        // Re-apply Boosted RX Gain after every init — initLoRa resets registers,
        // and persist=true keeps it across the radio's internal RX↔standby cycles.
        radio.setRxBoostedGainMode(s_boosted_gain_enabled, true);
    }
    meshtastic_set_active(lora_powered);
    clock_screen_set_lora_active(lora_powered);
    update_status();
    lora_save_power(lora_powered); // survive reboot
}

static void on_boosted_changed(lv_event_t *e)
{
    s_boosted_gain_enabled = lv_obj_has_state(boosted_switch, LV_STATE_CHECKED);
    // SX126x setRxBoostedGainMode is a register write — safe to call in any
    // radio mode. When LoRa is off the radio isn't initialised, so just record
    // the intent and apply it on the next power-on.
    if (lora_powered)
        radio.setRxBoostedGainMode(s_boosted_gain_enabled, true);
}

void lora_screen_create()
{
    lora_screen = lv_obj_create(NULL);
    tools_attach_jump_gesture(lora_screen);   // swipe-down -> Tools grid
    lv_obj_set_style_bg_color(lora_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(lora_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "LoRa");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle (left of center)
    toggle_sw = lv_switch_create(lora_screen);
    lv_obj_set_ext_click_area(toggle_sw, 22);  // easier to hit
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle)
    status_label = lv_label_create(lora_screen);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);
    update_status();

    // Data panel — sized to fit the 7 rows exactly (7 * 36 = 252) so there's
    // no slack space at the bottom that renders as an empty row.
    lv_obj_t *data_panel = lv_obj_create(lora_screen);
    lv_obj_set_size(data_panel, 380, 252);
    lv_obj_align(data_panel, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_bg_color(data_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(data_panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(data_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(data_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(data_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(data_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_layout(data_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(data_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(data_panel, 0, LV_PART_MAIN);

    make_data_row(data_panel, "Node ID",      &val_node_id);
    make_data_row(data_panel, "Frequency",    &val_frequency);
    make_data_row(data_panel, "Bandwidth",    &val_bandwidth);
    make_data_row(data_panel, "RSSI",         &val_rssi);
    make_data_row(data_panel, "CAD",          &val_cad);
    make_data_row(data_panel, "TX Power",     &val_tx_power);
    make_data_row(data_panel, "Packet Count", &val_packet_count);

    // Boosted RX Gain toggle (SX1262 ~+2 dB sensitivity, costs ~+2 mA in RX)
    lv_obj_t *boost_row = lv_obj_create(lora_screen);
    lv_obj_set_size(boost_row, 380, 55);
    lv_obj_set_style_bg_opa(boost_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(boost_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(boost_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(boost_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(boost_row, LV_ALIGN_TOP_MID, 0, 400);

    lv_obj_t *boost_lbl = lv_label_create(boost_row);
    lv_obj_set_style_text_color(boost_lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(boost_lbl, &font_argus_label_20, LV_PART_MAIN);
    lv_label_set_text(boost_lbl, "Boosted RX Gain");
    lv_obj_align(boost_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    boosted_switch = lv_switch_create(boost_row);
    lv_obj_set_size(boosted_switch, 80, 40);
    lv_obj_set_style_bg_color(boosted_switch, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(boosted_switch, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_state(boosted_switch, LV_STATE_CHECKED);   // default ON (+2 dB RX sens)
    lv_obj_add_event_cb(boosted_switch, on_boosted_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(boosted_switch, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_timer_create(on_lora_update, 1000, NULL);
}

void lora_screen_show()
{
    lv_scr_load(lora_screen);
}

bool lora_screen_is_active()
{
    return lv_screen_active() == lora_screen;
}

bool lora_screen_is_powered()
{
    return lora_powered;
}

// Boot-time power-on: bring LoRa up the same way on_toggle does and reflect it on
// the switch. Called from setup() ONLY when the user opted LoRa into "Enable at
// boot" (boot_prefs), so the enable decision lives in the caller and there is no
// per-radio file check here. Must run after the SD card is mounted
// (instance.begin) and after lora_screen_create().
void lora_screen_restore_power()
{
    // Power LoRa on the same way on_toggle does: stop any FSK scanner sharing
    // the SX1262, power the radio, init LoRa, then re-apply Boosted RX Gain.
    pager_stop();
    tpms_stop();
    aprs_stop();
    instance.powerControl(POWER_RADIO, true);
    instance.initLoRa();
    radio.setRxBoostedGainMode(s_boosted_gain_enabled, true);
    lora_powered = true;
    meshtastic_set_active(true);
    clock_screen_set_lora_active(true);
    update_status();

    // Reflect on the toggle switch if the screen was already created.
    if (toggle_sw)
        lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
}
