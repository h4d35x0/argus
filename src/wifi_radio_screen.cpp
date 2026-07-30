#include "wifi_radio_screen.h"
#include "tools_screen.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include "usb_sd.h"
#include "radio_coexist.h"

// Modal low-memory dialog, defined in main.cpp. Shown when a radio fails to
// start for lack of free internal SRAM.
void low_mem_show_dialog(const char *msg);

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

// Persist the radio on/off state so WiFi survives a reboot. Mirrors the
// small-file pattern used by gps_screen.cpp.
#define WIFI_PATH "/Settings/wifi.txt"

static void wifi_save_power(bool on)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return; // host owns SD when mounted
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(WIFI_PATH, FILE_WRITE);   // FILE_WRITE = truncate
    if (!f) return;
    f.printf("%d\n", on ? 1 : 0);
    f.close();
}

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
        // COEXISTENCE GUARD (must run BEFORE WiFi.mode()): on this board
        // WiFi.mode(WIFI_STA) HANGS inside the call — never returns, hard-freeze
        // — when the BLE controller is already up. The low-mem fallback below
        // only helps when WiFi.mode() RETURNS false; a hang never reaches it. So
        // check the BLE controller first and refuse cleanly.
#if !ARGUS_RADIO_COEXIST
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
            wifi_powered = false;
            lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
            low_mem_show_dialog(
                "#ff5555 WiFi COULD NOT START#\n\n"
                "Bluetooth is using the radio.\n\n"
                "Turn Bluetooth off (and any\n"
                "BLE detectors), then turn\n"
                "WiFi on.");
            update_status();
            wifi_save_power(false);
            return;
        }
#endif
        // STA is the default mode — scanning, evil twin, etc. transition
        // away from it as needed. WiFi.mode() is a no-op if already in
        // that mode, so this is safe to call repeatedly.
        // On this board WiFi + the BLE controller together exceed the free
        // CONTIGUOUS internal SRAM, so esp_wifi_init fails (returns false) when
        // Bluetooth is already up. Detect that, revert the switch, and tell the
        // user plainly rather than leaving a dead toggle stuck "on".
#if ARGUS_COEX_MEASURE
        // COEXIST DIAGNOSTIC: WiFi.mode(WIFI_STA) below can HANG (never return)
        // when base Bluedroid plus resident firmware exhaust contiguous internal
        // DRAM. This reproduces without a phone or ANCS connection.
        // Log the largest free CONTIGUOUS internal block to serial AND the SD card
        // right before the call, and flush, so the value survives even if the very
        // next line freezes the watch. This confirms (or rules out) DRAM exhaustion.
        {
            unsigned big  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            unsigned fint = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            unsigned dma  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
            unsigned bten = (unsigned)esp_bt_controller_get_status();
            Serial.printf("[COEX] pre-WiFi.mode: largest_internal=%u free_internal=%u largest_dma=%u bt_status=%u\n",
                          big, fint, dma, bten);
            Serial.flush();
            File cf = SD.open("/Settings/coexlog.txt", FILE_APPEND);
            if (cf) {
                cf.printf("pre-WiFi.mode largest_internal=%u free_internal=%u largest_dma=%u bt_status=%u\n",
                          big, fint, dma, bten);
                cf.flush();
                cf.close();
            }
        }
#endif
        if (!WiFi.mode(WIFI_STA) || WiFi.getMode() != WIFI_MODE_STA) {
            wifi_powered = false;
            lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
            WiFi.mode(WIFI_OFF);   // release any partial allocation
            low_mem_show_dialog(
                "#ff5555 WiFi COULD NOT START#\n\n"
                "Not enough free memory while\n"
                "Bluetooth is on.\n\n"
                "Turn Bluetooth off, then\n"
                "turn WiFi on.");
            update_status();
            wifi_save_power(false);
            return;
        }
    } else {
        // Disconnect cleanly before turning the radio off so any open
        // socket gets a proper close rather than a stuck-half-open state.
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
    update_status();
    wifi_save_power(wifi_powered); // survive reboot
}

void wifi_radio_screen_create()
{
    wifi_screen_root = lv_obj_create(NULL);
    tools_attach_jump_gesture(wifi_screen_root);   // swipe-down -> Tools grid
    lv_obj_set_style_bg_color(wifi_screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_screen_root, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(wifi_screen_root);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle (left of centre, identical placement to LoRa/GPS)
    toggle_sw = lv_switch_create(wifi_screen_root);
    lv_obj_set_ext_click_area(toggle_sw, 22);  // easier to hit
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, ARGUS_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle)
    status_label = lv_label_create(wifi_screen_root);
    lv_obj_set_style_text_color(status_label, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &font_argus_label_20, LV_PART_MAIN);
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

// Boot-time power-on: bring WiFi up the same way on_toggle does and reflect it on
// the switch. Called from setup() ONLY when the user opted WiFi into "Enable at
// boot" (boot_prefs), so the enable decision lives in the caller and there is no
// per-radio file check here. Must run after the SD card is mounted
// (instance.begin) and after wifi_radio_screen_create().
void wifi_radio_screen_restore_power()
{
    // COEXISTENCE GUARD: never bring WiFi up while the BLE controller is enabled
    // (WiFi.mode() would hang). At boot BLE is normally down so this is a no-op,
    // but keep it defensive in case restore runs after something started BLE.
#if !ARGUS_RADIO_COEXIST
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) return;
#endif

    // Power WiFi on the same way on_toggle does. STA is the default mode.
    WiFi.mode(WIFI_STA);
    wifi_powered = true;
    update_status();

    // Reflect on the toggle switch if the screen was already created.
    if (toggle_sw)
        lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
}
