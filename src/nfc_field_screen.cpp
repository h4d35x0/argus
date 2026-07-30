#include "nfc_field_screen.h"
#include "theme.h"
#include <LilyGoLib.h>          // instance, NFCReader, POWER_NFC
#include <Arduino.h>            // millis()
#include <stdio.h>

void tools_screen_show();       // tools_screen.cpp

static lv_obj_t *nfc_field_screen;
static lv_obj_t *status_label;
static lv_obj_t *sub_label;

static bool     s_powered        = false;
static int      s_hi             = 0;      // consecutive external-field reads
static bool     s_detected       = false;  // debounced state
static uint32_t s_field_start_ms = 0;

// Power the NFC front-end for passive field sensing. initNFC() sets the External
// Field Detector to auto-EFD (rfal_rfst25r3916.cpp:102) and leaves our field OFF
// because we never call rfalNfcDiscover() - so efd_o tracks an external reader.
static void nfc_power_on()
{
    if (s_powered) return;
    instance.powerControl(POWER_NFC, true);
    instance.initNFC();
    s_powered = true;
    s_hi = 0; s_detected = false; s_field_start_ms = 0;
}

static void nfc_power_off()
{
    if (!s_powered) return;
    instance.powerControl(POWER_NFC, false);
    s_powered = false;
}

static void on_tick(lv_timer_t *)
{
    // Own the NFC power only while this screen is up; release it on ANY exit path.
    if (lv_screen_active() != nfc_field_screen) { nfc_power_off(); return; }
    if (!s_powered) return;

    bool ext = NFCReader.getRfalRf()->rfalIsExtFieldOn();   // efd_o on the RF layer
    if (ext) { if (s_hi < 10) s_hi++; } else { s_hi = 0; }
    bool detected = (s_hi >= 2);   // debounce: 2 consecutive polls (~400 ms)

    if (detected && !s_detected) {          // rising edge -> buzz once, start dwell
        s_detected = true;
        s_field_start_ms = millis();
        instance.vibrator();
    } else if (!detected) {
        s_detected = false;
        s_field_start_ms = 0;
    }

    if (s_detected) {
        lv_label_set_text(status_label, "READER FIELD");
        lv_obj_set_style_text_color(status_label, HADES_RED, LV_PART_MAIN);
        char b[32];
        snprintf(b, sizeof(b), "active  %lus",
                 (unsigned long)((millis() - s_field_start_ms) / 1000));
        lv_label_set_text(sub_label, b);
    } else {
        lv_label_set_text(status_label, "Clear");
        lv_obj_set_style_text_color(status_label, ARGUS_ACCENT, LV_PART_MAIN);
        lv_label_set_text(sub_label, "scanning for a reader field...");
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        nfc_power_off();
        tools_screen_show();
    }
}

void nfc_field_screen_create()
{
    nfc_field_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(nfc_field_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(nfc_field_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(nfc_field_screen);
    lv_obj_set_style_text_color(title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_argus_ui, LV_PART_MAIN);
    lv_label_set_text(title, "NFC FIELD");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(nfc_field_screen);
    lv_obj_set_style_text_font(status_label, &font_argus_label_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, ARGUS_ACCENT, LV_PART_MAIN);
    lv_label_set_text(status_label, "Clear");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

    sub_label = lv_label_create(nfc_field_screen);
    lv_obj_set_style_text_font(sub_label, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub_label, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(sub_label, "scanning for a reader field...");
    lv_obj_align(sub_label, LV_ALIGN_CENTER, 0, 26);

    lv_obj_t *hint = lv_label_create(nfc_field_screen);
    lv_obj_set_style_text_font(hint, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_width(hint, 380);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint,
        "Warns when an RFID/NFC reader is powered within a few cm - e.g. someone "
        "trying to skim a card in your pocket. Near-field range only.");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_timer_create(on_tick, 200, NULL);
    lv_obj_add_event_cb(nfc_field_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void nfc_field_screen_show()
{
    nfc_power_on();
    lv_scr_load(nfc_field_screen);
}

bool nfc_field_screen_is_active() { return lv_screen_active() == nfc_field_screen; }
