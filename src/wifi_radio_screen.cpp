#include "wifi_radio_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Status / power-control screen for the on-board WiFi radio. Layout mirrors
// the GPS and LoRa screens: title + toggle + status line + 7-row data
// panel. The toggle just flips the radio between WIFI_OFF and WIFI_STA;
// the actual mode reported back can drift if other features bring the
// radio up in AP / scan mode (e.g. evil twin, the wifi tools screen), so
// the data panel always re-reads live state from esp_wifi each tick
// instead of trusting an internal flag.

static lv_obj_t *wifi_screen_root;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;

static lv_obj_t *val_mode;
static lv_obj_t *val_mac;
static lv_obj_t *val_channel;
static lv_obj_t *val_ssid;
static lv_obj_t *val_ip;
static lv_obj_t *val_rssi;
static lv_obj_t *val_tx_power;

// Cached desired state — true after the user flipped the toggle on, false
// after they flipped it off. The actual radio mode can still differ
// transiently (a scan can leave the radio in STA even after we asked for
// OFF), so update_status() always reconciles against esp_wifi_get_mode().
static bool wifi_powered = false;

static const char *mode_name(wifi_mode_t m)
{
    switch (m) {
    case WIFI_MODE_NULL: return "OFF";
    case WIFI_MODE_STA:  return "Station";
    case WIFI_MODE_AP:   return "Access Point";
    case WIFI_MODE_APSTA:return "AP + Station";
    default:             return "?";
    }
}

static bool radio_is_on()
{
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_wifi_get_mode(&m);
    return m != WIFI_MODE_NULL;
}

static void update_status()
{
    lv_label_set_text(status_label, radio_is_on() ? "Radio: ON" : "Radio: OFF");
}

// Build one label-on-the-left / value-on-the-right row inside the data
// panel. Identical styling to the GPS and LoRa screens' equivalent helper
// so the three radio screens feel like siblings.
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
    lv_obj_set_style_text_color(lbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, field);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    *val_out = val;
}

static void on_wifi_update(lv_timer_t *)
{
    // The toggle reconciliation + 7 label updates below are pure UI; skip
    // entirely when the screen isn't visible so the per-second cost of
    // esp_wifi_get_mode / get_mac / get_max_tx_power / get_channel +
    // WiFi.SSID/localIP/RSSI is only paid while the user is looking.
    if (!wifi_radio_screen_is_active()) return;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);

    // Reconcile the toggle if the radio came up (or went down) via another
    // path — keeps the switch from lying about the real state of things.
    bool on_now = (mode != WIFI_MODE_NULL);
    if (on_now != wifi_powered) {
        wifi_powered = on_now;
        if (on_now) lv_obj_add_state(toggle_sw,   LV_STATE_CHECKED);
        else        lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
    }
    update_status();

    if (mode == WIFI_MODE_NULL) {
        lv_label_set_text(val_mode,     "OFF");
        lv_label_set_text(val_mac,      "--");
        lv_label_set_text(val_channel,  "--");
        lv_label_set_text(val_ssid,     "--");
        lv_label_set_text(val_ip,       "--");
        lv_label_set_text(val_rssi,     "--");
        lv_label_set_text(val_tx_power, "--");
        return;
    }

    lv_label_set_text(val_mode, mode_name(mode));

    // MAC address — STA interface, even in AP mode (the AP MAC differs by
    // one bit but STA is what shows up on most routers' association lists).
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_label_set_text(val_mac, buf);

    // TX power — esp_wifi reports it in 0.25 dBm units. Pull it once per
    // tick rather than at toggle time so a setting change elsewhere shows
    // up here without a screen reload.
    int8_t tx_q = 0;
    esp_wifi_get_max_tx_power(&tx_q);
    snprintf(buf, sizeof(buf), "%.1f dBm", tx_q * 0.25f);
    lv_label_set_text(val_tx_power, buf);

    // Channel — only meaningful while STA is associated with something.
    // esp_wifi_get_channel returns 0 between associations, so render "--"
    // in that case rather than a misleading "0".
    uint8_t primary = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &sec);

    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)primary);
        lv_label_set_text(val_channel, buf);

        String ssid = WiFi.SSID();
        lv_label_set_text(val_ssid, ssid.length() ? ssid.c_str() : "--");

        IPAddress ip = WiFi.localIP();
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        lv_label_set_text(val_ip, buf);

        snprintf(buf, sizeof(buf), "%d dBm", WiFi.RSSI());
        lv_label_set_text(val_rssi, buf);
    } else {
        lv_label_set_text(val_channel, primary ? String(primary).c_str() : "--");
        lv_label_set_text(val_ssid,    "--");
        lv_label_set_text(val_ip,      "--");
        lv_label_set_text(val_rssi,    "--");
    }
}

static void on_toggle(lv_event_t *)
{
    wifi_powered = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    if (wifi_powered) {
        // STA is the default mode — scanning, evil twin, etc. transition
        // away from it as needed. WiFi.mode() is a no-op if already in
        // that mode, so this is safe to call repeatedly.
        WiFi.mode(WIFI_STA);
    } else {
        // Disconnect cleanly before turning the radio off so any open
        // socket gets a proper close rather than a stuck-half-open state.
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
    update_status();
}

void wifi_radio_screen_create()
{
    wifi_screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wifi_screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_screen_root, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(wifi_screen_root);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle (left of centre, identical placement to LoRa/GPS)
    toggle_sw = lv_switch_create(wifi_screen_root);
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle)
    status_label = lv_label_create(wifi_screen_root);
    lv_obj_set_style_text_color(status_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);
    update_status();

    // Data panel — same dimensions and 7 rows as the LoRa screen so the
    // bottom of the table lands at the same y on both screens.
    lv_obj_t *data_panel = lv_obj_create(wifi_screen_root);
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

    make_data_row(data_panel, "Mode",     &val_mode);
    make_data_row(data_panel, "MAC",      &val_mac);
    make_data_row(data_panel, "Channel",  &val_channel);
    make_data_row(data_panel, "SSID",     &val_ssid);
    make_data_row(data_panel, "IP",       &val_ip);
    make_data_row(data_panel, "RSSI",     &val_rssi);
    make_data_row(data_panel, "TX Power", &val_tx_power);

    // Seed the toggle with whatever the radio is doing right now — boot
    // path could have powered it on already (e.g. for an autoconnect).
    wifi_powered = radio_is_on();
    if (wifi_powered) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);

    lv_timer_create(on_wifi_update, 1000, NULL);
}

void wifi_radio_screen_show()
{
    lv_scr_load(wifi_screen_root);
}

bool wifi_radio_screen_is_active()
{
    return lv_screen_active() == wifi_screen_root;
}

bool wifi_radio_screen_is_powered()
{
    return wifi_powered;
}
