#include "bluetooth_screen.h"
#include "tools_screen.h"
#include "theme.h"
#include "ble_scan_manager.h"
#include <LilyGoLib.h>
#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>

// Modal low-memory dialog, defined in main.cpp. Shown when a radio fails to
// start for lack of free internal SRAM.
void low_mem_show_dialog(const char *msg);

// Status / power-control screen for the on-board Bluetooth (BLE) radio.
// Layout mirrors the GPS / LoRa / WiFi radio screens: title + toggle +
// status line + 7-row data table. The toggle hooks into the existing
// ble_scan_manager via a no-op consumer, so flipping it ON acts like any
// other scanner registering — the controller comes up if no-one else has
// it on yet, and stays up while at least one other module is using it.

static lv_obj_t *bt_screen_root;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;

static lv_obj_t *val_mode;
static lv_obj_t *val_status;
static lv_obj_t *val_mac;
static lv_obj_t *val_tx_power;
static lv_obj_t *val_scans;
static lv_obj_t *val_scan_type;
static lv_obj_t *val_connections;

// This screen's claim on the BLE controller, expressed as a no-op scan
// consumer. While registered, the controller is held up via the
// reference count in ble_scan_manager; on removal the manager tears it
// down if no other consumer still wants it.
static void noop_scan_cb(esp_ble_gap_cb_param_t *) { /* intentionally empty */ }

// Has the user asked the radio to be on via *this* screen? Distinct from
// the actual controller state — other modules (wardriver, airtag, etc.)
// can hold the controller up while our claim is off.
static bool s_radio_wanted = false;

static inline __attribute__((unused)) bool radio_is_on()
{
    return esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE;
}

static const char *bt_status_name()
{
    switch (esp_bt_controller_get_status()) {
    case ESP_BT_CONTROLLER_STATUS_IDLE:    return "Idle";
    case ESP_BT_CONTROLLER_STATUS_INITED:  return "Inited";
    case ESP_BT_CONTROLLER_STATUS_ENABLED: return ble_scan_active() ? "Scanning" : "Enabled";
    default:                               return "?";
    }
}

static void update_status()
{
    lv_label_set_text(status_label, s_radio_wanted ? "Radio: ON" : "Radio: OFF");
}

// Convert the ESP-IDF BLE TX power enum to dBm. Levels are 3 dBm apart;
// ESP_PWR_LVL_N0 (value 4) corresponds to 0 dBm, so dBm = (level - 4) * 3.
static int tx_power_dbm_from_level(esp_power_level_t level)
{
    return ((int)level - (int)ESP_PWR_LVL_N0) * 3;
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

static void on_bt_update(lv_timer_t *)
{
    // Skip entirely when the screen isn't visible — none of the work
    // below (esp_bt_controller_get_status + esp_bt_dev_get_address +
    // esp_ble_tx_power_get + ble_scan_consumer_count) is observable.
    // Toggle state re-syncs within 1 s of opening the screen.
    if (!bluetooth_screen_is_active()) return;

    // The BLE controller is kept up for the app's life (boot keep-alive), so
    // radio_is_on() is always true and no longer distinguishes this screen's
    // intent. The switch therefore reflects s_radio_wanted (did the user turn BT
    // on from here?). Keep it matched in case the state was set elsewhere.
    bool checked = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    if (s_radio_wanted != checked) {
        if (s_radio_wanted) lv_obj_add_state(toggle_sw,   LV_STATE_CHECKED);
        else                lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
    }
    update_status();

    if (!s_radio_wanted) {
        lv_label_set_text(val_mode,        "OFF");
        lv_label_set_text(val_status,      "Idle");
        lv_label_set_text(val_mac,         "--");
        lv_label_set_text(val_tx_power,    "--");
        lv_label_set_text(val_scans,       "0");
        lv_label_set_text(val_scan_type,   "--");
        lv_label_set_text(val_connections, "0");
        return;
    }

    // ESP32-S3 supports BLE only — no Classic. Naming it explicitly here
    // (rather than "Bluetooth") is clearer for anyone trying to debug
    // pairing issues from a phone that's expecting Classic.
    lv_label_set_text(val_mode,   "BLE");
    lv_label_set_text(val_status, bt_status_name());

    // MAC — only valid after Bluedroid is enabled. esp_bt_dev_get_address
    // returns NULL otherwise, in which case we fall back to "--".
    const uint8_t *mac = esp_bt_dev_get_address();
    if (mac) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        lv_label_set_text(val_mac, buf);
    } else {
        lv_label_set_text(val_mac, "--");
    }

    // TX power — pulled fresh in case another module tuned it. The
    // "DEFAULT" type covers advertising and scan responses; connections
    // can override it per-handle but we don't accept connections here.
    esp_power_level_t lvl = esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT);
    char buf[16];
    snprintf(buf, sizeof(buf), "%+d dBm", tx_power_dbm_from_level(lvl));
    lv_label_set_text(val_tx_power, buf);

    // Active scan consumers — this includes the no-op claim we register
    // when the toggle is on, plus whatever else (wardriver, airtag,
    // skimmer, flock) is also scanning.
    snprintf(buf, sizeof(buf), "%d", ble_scan_consumer_count());
    lv_label_set_text(val_scans, buf);

    // The ble_scan_manager hard-codes passive scanning today; reflect
    // that here rather than reading a runtime value that doesn't exist.
    lv_label_set_text(val_scan_type, ble_scan_active() ? "Passive" : "--");

    // The watch never accepts incoming connections (no GATT server
    // surface exposed); hard-coded zero is honest and lets the user know
    // they shouldn't expect to pair their phone with the watch.
    lv_label_set_text(val_connections, "0");
}

static void on_toggle(lv_event_t *)
{
    // The BLE controller is already up (boot keep-alive), so this just adds/removes
    // this screen's scan consumer against a live controller — instant, no blocking
    // bring-up, no UI freeze, no spurious re-fire. A single tap sticks.
    bool checked = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    if (checked && !s_radio_wanted) {
        // First consumer brings the BLE controller up. It needs internal SRAM,
        // and with WiFi already up there may not be enough - ble_scan_add()
        // returns false. Revert the switch and tell the user rather than
        // leaving a dead toggle stuck "on".
        if (!ble_scan_add(noop_scan_cb)) {
            lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
            low_mem_show_dialog(
                "#ff5555 BLUETOOTH COULD NOT START#\n\n"
                "Not enough free memory while\n"
                "WiFi is on.\n\n"
                "Turn WiFi off, then\n"
                "turn Bluetooth on.");
            update_status();
            return;
        }
        s_radio_wanted = true;
    } else if (!checked && s_radio_wanted) {
        ble_scan_remove(noop_scan_cb);
        s_radio_wanted = false;
    }
    update_status();
}

void bluetooth_screen_create()
{
    bt_screen_root = lv_obj_create(NULL);
    tools_attach_jump_gesture(bt_screen_root);   // swipe-down -> Tools grid
    lv_obj_set_style_bg_color(bt_screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bt_screen_root, 0, LV_PART_MAIN);

    // Title — kept under 8 chars so it sits cleanly on the round display
    // at the 48 pt size used by the rest of the radio screens.
    lv_obj_t *title = lv_label_create(bt_screen_root);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "Bluetooth");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    toggle_sw = lv_switch_create(bt_screen_root);
    lv_obj_set_ext_click_area(toggle_sw, 22);  // easier to hit
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    status_label = lv_label_create(bt_screen_root);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);
    update_status();

    // Data panel — same dimensions and 7 rows as LoRa/WiFi so the table
    // bottom lands at the same y across all four radio screens.
    lv_obj_t *data_panel = lv_obj_create(bt_screen_root);
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

    make_data_row(data_panel, "Mode",        &val_mode);
    make_data_row(data_panel, "Status",      &val_status);
    make_data_row(data_panel, "MAC",         &val_mac);
    make_data_row(data_panel, "TX Power",    &val_tx_power);
    make_data_row(data_panel, "Active Scans",&val_scans);
    make_data_row(data_panel, "Scan Type",   &val_scan_type);
    make_data_row(data_panel, "Connections", &val_connections);

    // If the controller is already running (something else brought it
    // up before this screen was opened), reflect that on the toggle so
    // the very first frame doesn't show OFF over a live radio.
    if (s_radio_wanted) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);

    lv_timer_create(on_bt_update, 1000, NULL);
}

void bluetooth_screen_show()
{
    lv_scr_load(bt_screen_root);
}

bool bluetooth_screen_is_active()
{
    return lv_screen_active() == bt_screen_root;
}

bool bluetooth_screen_is_powered()
{
    return s_radio_wanted;
}

// Boot-time BLE bring-up: called from setup() only when the user opted Bluetooth
// into "Enable at boot". Brings the controller up the same on-demand way the
// toggle does (adds this screen's scan consumer), so the refcount + toggle state
// stay consistent. WiFi is guaranteed off at this point because the boot chooser
// makes WiFi and Bluetooth mutually exclusive; ble_scan_add() still guards
// against WiFi being up and simply returns false (leaving BT off) if so.
// NOTE: auto-bringing BLE up at boot has historically boot-looped this watch;
// this path is gated behind an explicit user opt-in and must be verified on
// hardware with a BOOT+RST recovery ready.
void bluetooth_screen_restore_power()
{
    if (s_radio_wanted) return;               // already up
    if (!ble_scan_add(noop_scan_cb)) return;  // guard refuses if WiFi is up
    s_radio_wanted = true;
    if (toggle_sw) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
}
