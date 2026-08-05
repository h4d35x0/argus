#include "tools_screen.h"
#include "theme.h"
#include "airtag.h"
#include "flipper.h"
#include "skimmer.h"
#include "evil_twin.h"
#include "flock.h"
#include "threat_radar_screen.h"
#include "pet_screen.h"
#include "handshake.h"
#include "tesla_cp_screen.h"
#include "tpms_screen.h"
#include "pager_screen.h"
#include "argus_mode.h"
#include "tracker_sweep.h"
#include "spycam_screen.h"
#include "nfc_field_screen.h"
#include "loot_screen.h"
#include "deauth_screen.h"
#include "tracker_timeline_screen.h"
#include "beacon_spam.h"
#include "deauth_attack.h"
#include "rogue_ap.h"
#include "probe_sniffer.h"
#include <string.h>
#include "mouse_screen.h"
#include "usb_sd_screen.h"
#include "aprs_screen.h"
#include "wifi_screen.h"
#include "notifications_screen.h"
#include "analyze_screen.h"
#include "ble_scan_manager.h"
#include <LilyGoLib.h>
#include <SD.h>

// Defined in main.cpp
void clock_screen_show();
void main_loop_request_lvgl_priority(int cycles);
void low_mem_show_dialog(const char *msg);

// A detector refused to start because the other radio owns the internal SRAM —
// on this board WiFi and the BLE controller cannot run at the same time. Tell
// the user which radio to free instead of leaving a tile stuck gray.
//   is_ble_feature = true  -> needs Bluetooth, blocked by WiFi  ("turn WiFi off")
//   is_ble_feature = false -> needs WiFi, blocked by Bluetooth  ("turn BT off")
static void show_radio_conflict_dialog(bool is_ble_feature)
{
    if (is_ble_feature)
        low_mem_show_dialog(
            "#ff5555 CAN'T START#\n\n"
            "This uses Bluetooth, which\n"
            "can't run while WiFi is on.\n\n"
            "Turn WiFi off, then\n"
            "try again.");
    else
        low_mem_show_dialog(
            "#ff5555 CAN'T START#\n\n"
            "This uses WiFi, which can't\n"
            "run while Bluetooth is on.\n\n"
            "Turn Bluetooth off (and BLE\n"
            "detectors), then try again.");
}

static lv_obj_t *tools_screen;
static lv_obj_t *tools_title;   // repainted on show() so it follows the current mode accent
static lv_obj_t *t_airtag;    // referenced by on_airtag_clicked for colour swap
static lv_obj_t *t_trackers;  // referenced by on_trackers_clicked for colour swap
static lv_obj_t *t_flipper;   // referenced by on_flipper_clicked for colour swap
static lv_obj_t *t_skimmer;   // referenced by on_skimmer_clicked for colour swap
static lv_obj_t *t_eviltwin;  // referenced by on_eviltwin_clicked for colour swap
static lv_obj_t *t_flock;     // referenced by on_flock_clicked for colour swap
static lv_obj_t *t_handshake; // referenced by on_handshake_clicked for colour swap
static lv_obj_t *tools_grid;  // the flex container holding the tiles

// --- Rearrangeable Tools grid ------------------------------------------------
// Every tile carries a STABLE string key in its user_data (independent of the
// display label) so a saved order survives label / firmware changes. A long
// press lifts a tile into "drag" mode; sliding the finger over another tile
// re-flows the grid live via lv_obj_move_to_index; releasing saves the new
// order to the SD card, one key per line.
#define TOOLS_ORDER_PATH "/Settings/tools_order.txt"

static lv_obj_t *s_drag_tile      = nullptr;  // tile currently being dragged
static bool      s_drag_active    = false;    // a long-press drag is in progress
static bool      s_suppress_click = false;    // swallow the CLICKED that follows a drag

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP)
        clock_screen_show();
}

// Keep LVGL prioritized while the grid is actively scrolling so the momentum
// animation stays smooth. Each scroll step tops up the priority window (the main
// loop then skips background ticks / pauses matrix until scrolling settles).
// Without this, the 12-cycle window from tools_screen_show() runs out mid-flick
// and per-iteration background work stutters the scroll ("lags behind").
static void on_grid_scroll(lv_event_t *)
{
    main_loop_request_lvgl_priority(8);
}

static void set_airtag_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_airtag,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_airtag_clicked(lv_event_t *e)
{
    if (airtag_is_running()) {
        airtag_stop();
        set_airtag_tile_running(false);
    } else {
        bool ok = airtag_start();
        if (!ok) show_radio_conflict_dialog(true);  // BLE feature blocked by WiFi
        set_airtag_tile_running(ok);   // stays gray if it couldn't start
    }
}

static void set_trackers_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_trackers,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

// Universal non-Apple tracker sweep (Tile/SmartTag/Chipolo). Passive BLE; feeds
// the Threat Radar correlation store. Same BLE-vs-WiFi conflict path as AirTag.
static void on_trackers_clicked(lv_event_t *)
{
    if (tracker_sweep_is_running()) {
        tracker_sweep_stop();
        set_trackers_tile_running(false);
    } else {
        bool ok = tracker_sweep_start();
        if (!ok) show_radio_conflict_dialog(true);  // BLE feature blocked by WiFi
        set_trackers_tile_running(ok);
    }
}

static void set_flipper_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_flipper,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_flipper_clicked(lv_event_t *e)
{
    if (flipper_is_running()) {
        flipper_stop();
        set_flipper_tile_running(false);
    } else {
        bool ok = flipper_start();
        if (!ok) show_radio_conflict_dialog(true);  // BLE feature blocked by WiFi
        set_flipper_tile_running(ok);   // stays gray if it couldn't start
    }
}

static void set_skimmer_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_skimmer,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_skimmer_clicked(lv_event_t *e)
{
    if (skimmer_is_running()) {
        skimmer_stop();
        set_skimmer_tile_running(false);
    } else {
        bool ok = skimmer_start();
        if (!ok) show_radio_conflict_dialog(true);  // BLE feature blocked by WiFi
        set_skimmer_tile_running(ok);   // stays gray if it couldn't start
    }
}

static void set_eviltwin_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_eviltwin,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_eviltwin_clicked(lv_event_t *e)
{
    if (evil_twin_is_running()) {
        evil_twin_stop();
        set_eviltwin_tile_running(false);
    } else {
        bool ok = evil_twin_start();
        if (!ok) show_radio_conflict_dialog(false);  // WiFi feature blocked by BT
        set_eviltwin_tile_running(ok);
    }
}

static void set_flock_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_flock,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_flock_clicked(lv_event_t *e)
{
    if (flock_is_running()) {
        flock_stop();
        set_flock_tile_running(false);
    } else {
        // Flock wants BOTH WiFi and BLE, but this board can only run one radio
        // at a time, so flock_start() comes up on whichever radio is free and
        // returns true. It only returns false if NEITHER could start (both
        // consumer tables full / odd state) — surface that rather than a dead
        // tile. (Degraded single-radio coverage is a known limitation; see the
        // Flock coexistence note.)
        bool ok = flock_start();
        if (!ok) {
            low_mem_show_dialog(
                "#ff5555 FLOCK CAN'T START#\n\n"
                "No radio is free right now.\n\n"
                "Turn off other detectors,\n"
                "then try again.");
        } else if (flock_wifi_active() && !flock_ble_active()) {
            // Came up WiFi-only. Green tile would imply full coverage, so tell
            // the user BLE surveillance detection is dark. Informational, not
            // an error -- the WiFi half is running fine.
            low_mem_show_dialog(
                "#33bbff FLOCK: WiFi ONLY#\n\n"
                "Running on WiFi. BLE\n"
                "surveillance detection needs\n"
                "Bluetooth, which can't run\n"
                "while WiFi is on.");
        } else if (flock_ble_active() && !flock_wifi_active()) {
            // Mirror of the above: BLE came up, WiFi half is dark.
            low_mem_show_dialog(
                "#33bbff FLOCK: BLE ONLY#\n\n"
                "Running on Bluetooth. WiFi\n"
                "surveillance detection needs\n"
                "WiFi, which can't run while\n"
                "Bluetooth is on.");
        }
        set_flock_tile_running(ok);
    }
}

// Tile container — 180x180 button-like card with a label at the bottom.
// The icon-drawing helpers below fill the upper portion using LVGL primitives
// (no image assets needed). The tile is clickable so future feature wiring
// is a single lv_obj_add_event_cb call per tile.
static lv_obj_t *make_tile(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, 118, 118);   // 3-across grid (was 180 for 2-across)
    // ARGUS tile face: dark vertical gradient + steel-blue rounded rim, so the
    // premium look lives here (one place, every tile) and the HD glyph sprites stay
    // transparent (glyph + neon glow only). Matches tools/gen_icon_sprites.py.
    lv_obj_set_style_bg_color(tile, lv_color_make(0x16, 0x1E, 0x28), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(tile, lv_color_make(0x0D, 0x13, 0x1B), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, ARGUS_ACCENT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(lbl, ARGUS_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_argus_label_14, LV_PART_MAIN);   // Orbitron (fits 118px tile)
    lv_label_set_text(lbl, label_text);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

    return tile;
}

// Put the HD sprite /Icons/<name>.png on a tile if it exists on the SD card,
// otherwise fall back to the procedural draw_*_icon(). The sprites are the
// ARGUS HD icon set (tools/gen_icon_sprites.py -> transparent glyph + glow);
// the fallback keeps every tile working when the card lacks the /Icons folder, so
// dropping the art on the card is a pure visual upgrade with no firmware risk.
// Same pattern as the HexHound pet sprite (pup_icon.png).
static void tile_icon(lv_obj_t *tile, const char *name, void (*fallback)(lv_obj_t *))
{
    char sdpath[40];
    snprintf(sdpath, sizeof(sdpath), "/Icons/%s.png", name);
    if (SD.exists(sdpath)) {
        char lvpath[44];
        snprintf(lvpath, sizeof(lvpath), "A:/Icons/%s.png", name);
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, lvpath);
        // Sprites are 140px (drawn for the old 180px tiles); scale to ~78px so
        // the glyph sits above the label on the 118px 3-across tile.
        lv_image_set_scale(img, 142);                // 140 * 142/256 ~= 78px
        // set_scale shrinks the image toward its pivot. The default centre pivot
        // leaves the ~78px glyph rendered in the MIDDLE of the 140px layout box
        // (tile y~31..109) — floating low over the label. Pin the pivot to
        // top-centre (x=70 keeps it horizontally centred) so the glyph renders
        // from the top of the box down, then anchor near the tile top.
        lv_image_set_pivot(img, 70, 0);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);   // glyph band ~y=6..84, clear of the label
    } else {
        fallback(tile);
    }
}

// The procedural draw_*_icon() helpers below were authored in the original
// 180px tile coordinate space (glyph band ~y=20..126, horizontally centred on a
// 180-wide tile). The grid later shrank to 118px 3-across, which left every
// procedural fallback both overflowing the label and mis-centred horizontally.
// Rather than re-tune dozens of primitives per icon, each draws into a fixed
// 180x180 layer that is scaled as a unit (~0.77x) and top-anchored, so the
// original composition is preserved and framed to match the ~78px HD sprites.
// One place normalises the whole set; the per-icon geometry stays untouched.
// (pager/hexhound are handled directly for the 118px tile and are NOT wrapped.)
static lv_obj_t *icon_layer(lv_obj_t *tile)
{
    lv_obj_t *layer = lv_obj_create(tile);
    lv_obj_remove_style_all(layer);                              // transparent, no bg/border/pad
    lv_obj_set_size(layer, 180, 180);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(layer, 90, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(layer, 73, LV_PART_MAIN); // centre of the glyph band
    lv_obj_set_style_transform_scale_x(layer, 198, LV_PART_MAIN); // 256 * 0.77 -> band ~82px
    lv_obj_set_style_transform_scale_y(layer, 198, LV_PART_MAIN);
    lv_obj_align(layer, LV_ALIGN_TOP_LEFT, -31, -28);            // (118-180)/2 x; lifts band to y~4..86
    return layer;
}

// Upper-left: WiFi — signal glyph in cyan, for the site-survey / ping-sweep tool
static void draw_wifi_icon(lv_obj_t *tile)
{
    lv_obj_t *wifi = lv_label_create(tile);
    lv_obj_set_style_text_color(wifi, lv_color_make(0x33, 0xBB, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    // y=14 matches draw_notify_icon, the other direct 48px-symbol tile. This was
    // 44, which put a 48px glyph at y=44..92+ and ran it into the label band
    // (~y=94), covering the text.
    lv_obj_align(wifi, LV_ALIGN_TOP_MID, 0, 14);
}

// Notify tile: a bell glyph in amber, for the phone-notification mirror screen.
static void draw_notify_icon(lv_obj_t *tile)
{
    lv_obj_t *bell = lv_label_create(tile);
    lv_obj_set_style_text_color(bell, lv_color_make(0xF0, 0xB4, 0x30), LV_PART_MAIN);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_align(bell, LV_ALIGN_TOP_MID, 0, 14);   // lifted to sit in the upper band, clear of the label
}

// Analyze — spectrum-analyzer logo: a row of vertical bars at varying heights
// sitting on a baseline, colours stepping from green through yellow to red to
// suggest channel saturation.
static void draw_analyzer_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Baseline (axis) under the bars.
    lv_obj_t *base = lv_obj_create(tile);
    lv_obj_set_size(base, 116, 2);
    lv_obj_set_style_bg_color(base, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 116);

    // Bar heights / colours give a spectrum-analyzer look with a clear peak.
    static const int heights[7]    = { 22, 44, 70, 96, 78, 50, 30 };
    static const uint32_t colors[7] = {
        0x00CC66, 0x00CC66, 0x44BBFF, 0xFFCC00,
        0xFF8844, 0xFFCC00, 0x00CC66,
    };
    const int bar_w = 12;
    const int gap   = 4;
    const int total = 7 * bar_w + 6 * gap;     // 84 + 24 = 108 px
    const int start = (180 - total) / 2;       // = 36 px

    for (int i = 0; i < 7; i++) {
        lv_obj_t *b = lv_obj_create(tile);
        lv_obj_set_size(b, bar_w, heights[i]);
        lv_obj_set_style_bg_color(b,
            lv_color_make((colors[i] >> 16) & 0xFF,
                          (colors[i] >>  8) & 0xFF,
                          (colors[i]      ) & 0xFF),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        // Each bar stands on the baseline at y=116, growing upward.
        lv_obj_align(b, LV_ALIGN_TOP_LEFT,
                     start + i * (bar_w + gap),
                     116 - heights[i]);
    }
}

// Trackers — a Tile-style tag (rounded square + hanging hole + a red centre dot)
// for the universal non-Apple BLE tracker sweep. Procedural fallback for
// /Icons/trackers.png.
// Trackers - a tag body with a lanyard hole and an LED dot. The whole
// composition sits 16px higher in layer space than it was authored: the 84x100
// body anchored at y=44 bottomed out at layer y=144, which the icon_layer
// transform maps to tile y~99.9, past the label top (~94) - the tag visibly
// covered the word "Trackers". Shifted as a unit (44/56/96 -> 28/40/80) so the
// tag keeps its proportions; the bottom now lands at ~87.5, the same clearance
// the Pwn icon uses.
static void draw_trackers_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t steel = lv_color_make(0x9B, 0xBC, 0xD6);
    lv_color_t dark  = lv_color_make(0x1A, 0x24, 0x30);

    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 84, 100);
    lv_obj_set_style_radius(body, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, steel, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *hole = lv_obj_create(tile);
    lv_obj_set_size(hole, 20, 20);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hole, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(hole, steel, LV_PART_MAIN);
    lv_obj_set_style_border_width(hole, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hole, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(hole, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 24, 24);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 80);
}

// NFC Field — concentric RF-field rings with a red core, for the reader-field
// (pocket-skim) detector. Procedural fallback for /Icons/nfcfield.png.
static void draw_nfcfield_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t steel = ARGUS_ACCENT;
    const int d[3] = { 100, 68, 36 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(tile);
        lv_obj_set_size(r, d[i], d[i]);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(r, steel, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 30 + (100 - d[i]) / 2);
    }
    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 22, 22);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 69);
}

// Spycam — a CCTV camera (body + lens + mount) in threat-red, for the passive
// wireless-camera fingerprint results tile. Procedural fallback for /Icons/spycam.png.
static void draw_spycam_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t red  = HADES_RED;
    lv_color_t dark = lv_color_make(0x1A, 0x24, 0x30);

    lv_obj_t *mount = lv_obj_create(tile);       // ceiling mount stalk
    lv_obj_set_size(mount, 8, 22);
    lv_obj_set_style_radius(mount, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mount, red, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mount, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mount, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mount, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mount, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mount, LV_ALIGN_TOP_MID, -28, 40);

    lv_obj_t *body = lv_obj_create(tile);        // camera body
    lv_obj_set_size(body, 96, 54);
    lv_obj_set_style_radius(body, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, red, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, -6, 58);

    lv_obj_t *lens = lv_obj_create(tile);        // lens ring
    lv_obj_set_size(lens, 34, 34);
    lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lens, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lens, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(lens, red, LV_PART_MAIN);
    lv_obj_set_style_border_width(lens, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lens, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lens, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lens, LV_ALIGN_TOP_MID, 32, 68);

    lv_obj_t *dot = lv_obj_create(tile);         // glowing lens centre
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, red, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 32, 79);
}

// Timeline -- a vertical rail with dots (one red), for the tail-timeline screen.
static void draw_timeline_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t steel = ARGUS_ACCENT;

    lv_obj_t *rail = lv_obj_create(tile);        // the vertical connector
    lv_obj_set_size(rail, 6, 84);
    lv_obj_set_style_radius(rail, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rail, ARGUS_ACCENT_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rail, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rail, LV_ALIGN_TOP_MID, -34, 26);

    const int ys[3] = { 26, 60, 94 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(tile);
        lv_obj_set_size(dot, 26, 26);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, i == 1 ? HADES_RED : steel, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, -34, ys[i]);

        lv_obj_t *bar = lv_obj_create(tile);     // a "card" stub next to each dot
        lv_obj_set_size(bar, 74, 14);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, ARGUS_ACCENT_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 24, ys[i] + 6);
    }
}

// Deauth -- concentric WiFi/broadcast rings with a red "kick" badge (a circle
// with an X) in the corner, meaning "clients knocked off". No strike-through
// (that read as "disabled"). Shared by the Defense detector + Offense Deauther.
static void draw_deauth_impl(lv_obj_t *tile, lv_color_t ringc)
{
    tile = icon_layer(tile);
    lv_color_t steel = ringc;

    const int d[3] = { 100, 68, 36 };            // WiFi wave rings (nfcfield band)
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(tile);
        lv_obj_set_size(r, d[i], d[i]);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(r, steel, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 24 + (100 - d[i]) / 2);
    }
    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 20, 20);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, steel, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 64);

    // Red "kick" badge (circle + X), inset into the top-right like the Skimmer
    // warning badge so it sits ON the glyph, not off the edge.
    lv_obj_t *badge = lv_obj_create(tile);
    lv_obj_set_size(badge, 34, 34);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -18, 24);
    lv_obj_t *x = lv_label_create(badge);
    lv_obj_set_style_text_font(x, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(x, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(x, "X");
    lv_obj_center(x);
}

// Two flavours sharing the geometry above: steel-blue for the Defense deauth
// DETECTOR tile, red for the Offense deauth ATTACK tile.
static void draw_deauth_icon(lv_obj_t *tile)     { draw_deauth_impl(tile, ARGUS_ACCENT); }
static void draw_deauth_atk_icon(lv_obj_t *tile) { draw_deauth_impl(tile, ARGUS_OFFENSE_ACCENT); }

// Probes -- an amber magnifying glass (recon) over a few device dots, for the
// probe-request sniffer. Procedural fallback.
static void draw_probes_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t amber = ARGUS_OFFENSE_ACCENT;

    lv_obj_t *lens = lv_obj_create(tile);        // lens ring
    lv_obj_set_size(lens, 80, 80);
    lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lens, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(lens, amber, LV_PART_MAIN);
    lv_obj_set_style_border_width(lens, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lens, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lens, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lens, LV_ALIGN_TOP_MID, -12, 24);

    // device dots inside the lens
    const int dx[3] = { -26, 0, 20 };
    const int dy[3] = { 44, 56, 40 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(tile);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, i == 1 ? HADES_RED : amber, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, dx[i], dy[i]);
    }

    lv_obj_t *handle = lv_obj_create(tile);      // magnifier handle
    lv_obj_set_size(handle, 10, 40);
    lv_obj_set_style_radius(handle, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(handle, amber, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(handle, 5, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(handle, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(handle, -450, LV_PART_MAIN);   // -45 deg
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 30, 96);
}

// Rogue AP -- a red access-point "router" with two horn-tipped antennas (the
// rogue/evil-twin tell) and status LEDs. Compact in the glyph band so it never
// runs into the tile label. Procedural fallback.
static void draw_rogueap_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t red  = ARGUS_OFFENSE_ACCENT;
    lv_color_t dark = lv_color_make(0x1E, 0x12, 0x12);

    // Two horn-tipped antennas rising and splaying outward from the router.
    static lv_point_precise_t ant_l[] = { {74, 66}, {56, 38} };
    static lv_point_precise_t ant_r[] = { {106, 66}, {124, 38} };
    lv_point_precise_t *ant[] = { ant_l, ant_r };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *a = lv_line_create(tile);
        lv_line_set_points(a, ant[i], 2);
        lv_obj_set_style_line_color(a, red, 0);
        lv_obj_set_style_line_width(a, 8, 0);
        lv_obj_set_style_line_rounded(a, true, 0);
    }

    // Router / AP body.
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 96, 42);
    lv_obj_set_style_radius(body, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, red, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 62);

    // Three status LEDs across the front.
    for (int i = -1; i <= 1; i++) {
        lv_obj_t *led = lv_obj_create(tile);
        lv_obj_set_size(led, 12, 12);
        lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(led, red, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(led, 0, LV_PART_MAIN);
        lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(led, LV_ALIGN_TOP_MID, i * 28, 77);
    }
}

// Beacon flood -- an amber broadcast mast with arcs and scattered SSID "ghost"
// squares spraying out, for the Offense beacon/SSID flood. Procedural fallback.
static void draw_beaconspam_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t amber = ARGUS_OFFENSE_ACCENT;
    lv_color_t red   = HADES_RED;

    lv_obj_t *mast = lv_obj_create(tile);        // vertical mast
    lv_obj_set_size(mast, 8, 60);
    lv_obj_set_style_radius(mast, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mast, amber, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mast, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mast, LV_ALIGN_TOP_MID, 0, 52);

    const int d[2] = { 44, 72 };                 // broadcast arcs at the top
    for (int i = 0; i < 2; i++) {
        lv_obj_t *a = lv_obj_create(tile);
        lv_obj_set_size(a, d[i], d[i]);
        lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(a, amber, LV_PART_MAIN);
        lv_obj_set_style_border_width(a, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 22 + (72 - d[i]) / 2);
    }
    // Scattered ghost SSID squares (kept up in the glyph band, clear of the label).
    const int gx[4] = { -58, 52, -46, 58 };
    const int gy[4] = { 70, 78, 100, 96 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *g = lv_obj_create(tile);
        lv_obj_set_size(g, 22, 14);
        lv_obj_set_style_radius(g, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g, i & 1 ? red : amber, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(g, 0, LV_PART_MAIN);
        lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(g, LV_ALIGN_TOP_MID, gx[i], gy[i]);
    }
}

// Loot -- an amber folder with a red download/offload glyph, for the Offense-only
// captured-artifact manager. Procedural fallback for /Icons/loot.png.
static void draw_loot_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);
    lv_color_t amber = ARGUS_OFFENSE_ACCENT;
    lv_color_t dark  = lv_color_make(0x1A, 0x24, 0x30);

    lv_obj_t *tab = lv_obj_create(tile);          // folder tab
    lv_obj_set_size(tab, 40, 16);
    lv_obj_set_style_radius(tab, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tab, amber, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tab, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tab, LV_ALIGN_TOP_MID, -26, 28);

    lv_obj_t *body = lv_obj_create(tile);         // folder body
    lv_obj_set_size(body, 100, 70);
    lv_obj_set_style_radius(body, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, dark, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, amber, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *dl = lv_label_create(tile);         // download/offload glyph
    lv_obj_set_style_text_color(dl, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(dl, LV_SYMBOL_DOWNLOAD);
    lv_obj_align(dl, LV_ALIGN_TOP_MID, 0, 48);
}

// Upper-right: AirTag — round disc with a small dot in the centre
static void draw_airtag_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Outer disc (off-white)
    lv_obj_t *outer = lv_obj_create(tile);
    lv_obj_set_size(outer, 84, 84);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(outer, lv_color_make(0xEE, 0xEE, 0xEE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(outer, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(outer, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(outer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(outer, LV_ALIGN_TOP_MID, 0, 28);

    // Small dot (the AirTag's Apple-logo placement)
    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 20, 20);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 60);
}

// Flipper Zero — stylized leaping dolphin (the Flipper mascot), facing LEFT
// like the Flipper Zero logo. The silhouette is layered from rounded pills to
// form a tapered body (head → torso → peduncle), with a backward-leaning
// dorsal fin and a horizontal two-lobe fluke at the back. The fin + V-fluke
// pair are what make it read as a dolphin at a glance.
// Defined in flipper_logo_img.c — 1-bit alpha mask of the canonical
// Flipper Zero dolphin logo (120×80), generated from the official PNG
// by tools/gen_flipper_logo.py. Recoloured to brand orange at draw time
// via the image widget's style.
extern "C" const lv_image_dsc_t flipper_logo_img;

static void draw_flipper_icon(lv_obj_t *tile)
{
    lv_color_t flip_orange = lv_color_make(0xFF, 0x82, 0x00);

    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, &flipper_logo_img);
    // A1 images are an alpha mask only — LVGL draws the image_recolor
    // style colour where the mask is 1 and nothing where it's 0, so the
    // orange tint is applied here at draw time rather than baked into
    // the bitmap.
    lv_obj_set_style_image_recolor(img, flip_orange, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
    // flipper_logo_img is 120x80 — full size overflows the 118px tile. Scale to
    // ~88px wide and pin the pivot top-centre so it anchors high, like the sprites.
    lv_image_set_scale(img, 188);                // 120 * 188/256 ~= 88px wide
    lv_image_set_pivot(img, 60, 0);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 16);
}

// Skimmer detector icon — a credit card on its side with a thin magnetic
// stripe and a small red warning chip in the corner, hinting at "card +
// compromise". Reads at-a-glance as "card reader / skimmer".
static void draw_skimmer_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Card body — wide rounded rectangle in a neutral plastic colour.
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, 116, 74);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_make(0xDD, 0xDD, 0xDD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);   // y=36..110

    // Magnetic stripe — the long black band across the upper portion.
    lv_obj_t *stripe = lv_obj_create(tile);
    lv_obj_set_size(stripe, 116, 14);
    lv_obj_set_style_radius(stripe, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stripe, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(stripe, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stripe, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stripe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(stripe, LV_ALIGN_TOP_MID, 0, 48);

    // EMV chip — small gold square in the lower-left quadrant of the card.
    lv_obj_t *chip = lv_obj_create(tile);
    lv_obj_set_size(chip, 18, 14);
    lv_obj_set_style_radius(chip, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_make(0xD4, 0xAF, 0x37), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_make(0x88, 0x66, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 44, 74);

    // Two short digit-stripe placeholders on the card face to suggest
    // embossed numbers without trying to render real digits.
    for (int row = 0; row < 2; row++) {
        lv_obj_t *digits = lv_obj_create(tile);
        lv_obj_set_size(digits, 44, 3);
        lv_obj_set_style_radius(digits, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(digits, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(digits, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(digits, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(digits, 0, LV_PART_MAIN);
        lv_obj_clear_flag(digits, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(digits, LV_ALIGN_TOP_MID, 16, 92 + row * 6);
    }

    // Warning badge in the upper-right — red circle with a "!" so the icon
    // reads as "compromise" rather than just "credit card".
    lv_obj_t *badge = lv_obj_create(tile);
    lv_obj_set_size(badge, 26, 26);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -8, 26);

    lv_obj_t *bang = lv_label_create(badge);
    lv_obj_set_style_text_font(bang, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(bang, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(bang, "!");
    lv_obj_center(bang);
}

// Evil Twin -- two overlapping WiFi wedge shapes, the second one in red to
// signal "impostor".  The legitimate AP is drawn in white/grey at left-center;
// the rogue is drawn slightly offset and smaller in red at right, so at a
// glance the icon reads as "two APs claiming the same name".
static void draw_eviltwin_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Legitimate AP: three concentric arcs (large, medium, small) + dot,
    // stacked vertically, centre-left of the tile.
    lv_color_t legit  = lv_color_make(0xCC, 0xCC, 0xCC);
    lv_color_t rogue  = lv_color_make(0xDD, 0x22, 0x22);
    lv_color_t shared = lv_color_make(0xFF, 0xAA, 0x00);

    struct ArcDef { int x, y, w, h; lv_color_t col; };
    ArcDef arcs[] = {
        // legit AP arcs (left side)
        { -24, 28, 56, 28, legit },
        { -24, 42, 40, 20, legit },
        { -24, 56, 24, 12, legit },
        // rogue AP arcs (right side, red)
        {  12, 38, 56, 28, rogue },
        {  12, 52, 40, 20, rogue },
        {  12, 66, 24, 12, rogue },
    };
    for (auto &a : arcs) {
        lv_obj_t *arc = lv_obj_create(tile);
        lv_obj_set_size(arc, a.w, a.h);
        lv_obj_set_style_radius(arc, a.h / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(arc, a.col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(arc, LV_ALIGN_TOP_MID, a.x, a.y);
    }

    // Dot for legit AP
    lv_obj_t *d1 = lv_obj_create(tile);
    lv_obj_set_size(d1, 8, 8);
    lv_obj_set_style_radius(d1, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d1, legit, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d1, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d1, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d1, 0, LV_PART_MAIN);
    lv_obj_clear_flag(d1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(d1, LV_ALIGN_TOP_MID, -24, 68);

    // Dot for rogue AP
    lv_obj_t *d2 = lv_obj_create(tile);
    lv_obj_set_size(d2, 8, 8);
    lv_obj_set_style_radius(d2, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d2, rogue, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d2, 0, LV_PART_MAIN);
    lv_obj_clear_flag(d2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(d2, LV_ALIGN_TOP_MID, 12, 78);

    // Small "=" badge between the two APs to suggest "same name, different AP"
    for (int i = 0; i < 2; i++) {
        lv_obj_t *eq = lv_obj_create(tile);
        lv_obj_set_size(eq, 12, 3);
        lv_obj_set_style_radius(eq, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(eq, shared, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eq, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(eq, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(eq, 0, LV_PART_MAIN);
        lv_obj_clear_flag(eq, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(eq, LV_ALIGN_TOP_MID, -6, 56 + i * 7);
    }
}

// Flock -- camera silhouette (rectangular body + circular lens) to represent
// surveillance cameras and drones detected by OUI/name matching.
static void draw_flock_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    lv_color_t cam_body  = lv_color_make(0x33, 0x33, 0x33);
    lv_color_t cam_edge  = lv_color_make(0x66, 0x66, 0x66);
    lv_color_t lens_ring = lv_color_make(0x88, 0x88, 0x88);
    lv_color_t lens_fill = lv_color_make(0x11, 0x44, 0x88);
    lv_color_t lens_glint= lv_color_make(0xAA, 0xCC, 0xFF);
    lv_color_t alert_red = lv_color_make(0xCC, 0x22, 0x22);

    // Camera body
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 88, 56);
    lv_obj_set_style_radius(body, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, cam_body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, cam_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 30);

    // Lens ring
    lv_obj_t *lring = lv_obj_create(tile);
    lv_obj_set_size(lring, 36, 36);
    lv_obj_set_style_radius(lring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lring, lens_ring, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lring, LV_ALIGN_TOP_MID, 0, 40);

    // Lens fill
    lv_obj_t *lfill = lv_obj_create(tile);
    lv_obj_set_size(lfill, 26, 26);
    lv_obj_set_style_radius(lfill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lfill, lens_fill, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lfill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lfill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lfill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lfill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lfill, LV_ALIGN_TOP_MID, 0, 45);

    // Lens glint
    lv_obj_t *glint = lv_obj_create(tile);
    lv_obj_set_size(glint, 7, 7);
    lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(glint, lens_glint, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(glint, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(glint, 0, LV_PART_MAIN);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, -6, 48);

    // Small mount stub on top of the body
    lv_obj_t *mount = lv_obj_create(tile);
    lv_obj_set_size(mount, 18, 10);
    lv_obj_set_style_radius(mount, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mount, cam_body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mount, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(mount, cam_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(mount, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mount, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mount, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mount, LV_ALIGN_TOP_MID, 0, 21);

    // Red recording indicator dot (top-right of body)
    lv_obj_t *rec = lv_obj_create(tile);
    lv_obj_set_size(rec, 10, 10);
    lv_obj_set_style_radius(rec, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rec, alert_red, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rec, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rec, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rec, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rec, LV_ALIGN_TOP_RIGHT, -24, 38);
}

// Lower-left: microSD card — rounded body with a chamfered top-left corner and
// a row of gold contact pins. For the USB Mass Storage card-reader tool.
// Lower-left: microSD card — slimmer body than a full-size SD card, eight
// gold contact pins (the microSD pin count), and a small notch carved out of
// the lower-left edge as the distinguishing microSD silhouette feature.
static void draw_microsd_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Card body — narrower and a touch darker than the previous SD-ish slab.
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, 60, 88);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_make(0x2A, 0x3A, 0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x7A, 0x90, 0xBA), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // 180-wide tile → card spans x=60..120; top edge at y=24
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 24);

    // Chamfered top-left corner — a tile-coloured square rotated 45° about
    // its own centre, positioned over the card's corner.
    lv_obj_t *chamfer = lv_obj_create(tile);
    lv_obj_set_size(chamfer, 30, 30);
    lv_obj_set_style_radius(chamfer, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chamfer, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chamfer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(chamfer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chamfer, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(chamfer, 15, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(chamfer, 15, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(chamfer, 450, LV_PART_MAIN);
    lv_obj_clear_flag(chamfer, LV_OBJ_FLAG_SCROLLABLE);
    // Centre the 30x30 square on the card's top-left corner (60, 24)
    lv_obj_align(chamfer, LV_ALIGN_TOP_LEFT, 45, 9);

    // Eight gold contact pins — the microSD card layout (vs the nine of an
    // SD card). Pins centred under the chamfer.
    for (int i = 0; i < 8; i++) {
        lv_obj_t *pin = lv_obj_create(tile);
        lv_obj_set_size(pin, 4, 18);
        lv_obj_set_style_radius(pin, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(pin, lv_color_make(0xD4, 0xAF, 0x37), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(pin, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pin, 0, LV_PART_MAIN);
        lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
        // Pin centres at offsets -21 .. +21 in steps of 6 from tile centre
        lv_obj_align(pin, LV_ALIGN_TOP_MID, -21 + i * 6, 50);
    }

    // The microSD-defining notch on the lower-left edge — tile-coloured
    // rectangle that overlaps the card border to carve a small chunk out.
    lv_obj_t *notch = lv_obj_create(tile);
    lv_obj_set_size(notch, 6, 8);
    lv_obj_set_style_radius(notch, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(notch, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(notch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(notch, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(notch, 0, LV_PART_MAIN);
    lv_obj_clear_flag(notch, LV_OBJ_FLAG_SCROLLABLE);
    // Left edge of card is at x=60; place the notch straddling it so a few
    // px of card disappear (the rest of the notch sits over the tile bg).
    lv_obj_align(notch, LV_ALIGN_TOP_LEFT, 57, 88);
}

// Pager -- iconic yellow plastic body, wide green LCD across the top,
// and a control row below it: two rectangular keys each with a coloured
// LED indicator dot at the top (green and red), then a 4-way directional
// pad made of four separate rounded keys around a centre circle.
static void draw_pager_icon(lv_obj_t *tile)
{
    // Fine-tune: draw the whole gadget into a full-tile layer scaled ~0.91 about
    // its centre, so it comes out a touch smaller and a hair lower without
    // re-tuning every element. (Transform cascades to children in LVGL 9.5.)
    lv_obj_t *wrap = lv_obj_create(tile);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, 118, 118);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(wrap, 59, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(wrap, 42, LV_PART_MAIN);   // pager's vertical centre
    lv_obj_set_style_transform_scale_x(wrap, 233, LV_PART_MAIN);  // 233/256 ~= 0.91
    lv_obj_set_style_transform_scale_y(wrap, 233, LV_PART_MAIN);
    lv_obj_align(wrap, LV_ALIGN_TOP_MID, 0, 3);                   // tiny downward nudge
    tile = wrap;

    lv_color_t body_yellow = lv_color_make(0xFF, 0xCC, 0x33);
    lv_color_t body_shade  = lv_color_make(0xCC, 0x99, 0x22);
    lv_color_t btn_face    = lv_color_make(0x10, 0x10, 0x10);
    lv_color_t btn_edge    = lv_color_make(0x44, 0x44, 0x44);
    lv_color_t dpad_face   = lv_color_make(0x28, 0x28, 0x28);
    lv_color_t dpad_edge   = lv_color_make(0x55, 0x55, 0x55);

    // Yellow body -- compact 87x72 envelope (0.75x of the old 116x96) so the
    // glyph clears the label. Body spans y=4..76 in tile coords.
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 87, 72);
    lv_obj_set_style_radius(body, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, body_yellow, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, body_shade, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 4);

    // Green LCD display
    lv_obj_t *lcd = lv_obj_create(tile);
    lv_obj_set_size(lcd, 72, 23);
    lv_obj_set_style_radius(lcd, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lcd, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);  // 13-37 green LCD (user: use theirs)
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(lcd, lv_color_make(0x00, 0x77, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(lcd, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lcd, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lcd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 10);

    // Two darker bands standing in for pager message text
    for (int row = 0; row < 2; row++) {
        lv_obj_t *line = lv_obj_create(tile);
        lv_obj_set_size(line, 54, 3);
        lv_obj_set_style_radius(line, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(line, lv_color_make(0x00, 0x55, 0x22), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 15 + row * 7);
    }

    // ---- Control row ----
    // Left side: green and red action buttons.  Each is an 18x18 rounded-rect
    // key with a small oval LED indicator pill near the top, matching the
    // physical Motorola Advisor indicator lights.

    // Green action button
    lv_obj_t *gbtn = lv_obj_create(tile);
    lv_obj_set_size(gbtn, 14, 14);
    lv_obj_set_style_radius(gbtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gbtn, btn_face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(gbtn, btn_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(gbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gbtn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gbtn, LV_ALIGN_TOP_MID, -29, 46);

    lv_obj_t *gled = lv_obj_create(gbtn);
    lv_obj_set_size(gled, 6, 4);
    lv_obj_set_style_radius(gled, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gled, lv_color_make(0x22, 0xFF, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gled, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(gled, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gled, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gled, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gled, LV_ALIGN_TOP_MID, 0, 2);

    // Red action button
    lv_obj_t *rbtn = lv_obj_create(tile);
    lv_obj_set_size(rbtn, 14, 14);
    lv_obj_set_style_radius(rbtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rbtn, btn_face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(rbtn, btn_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(rbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rbtn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rbtn, LV_ALIGN_TOP_MID, -14, 46);

    lv_obj_t *rled = lv_obj_create(rbtn);
    lv_obj_set_size(rled, 6, 4);
    lv_obj_set_style_radius(rled, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rled, lv_color_make(0xFF, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rled, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rled, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rled, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rled, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rled, LV_ALIGN_TOP_MID, 0, 2);

    // Right side: Advisor-style 4-way directional pad — four rounded arm keys
    // around a centre circle, at 0.75x scale to match the compact body. Order:
    // up, down, left, right (tile TOP_MID relative). Centre key follows below.
    struct { int x, y, w, h; } dp_keys[4] = {
        { 23, 44, 9, 6 },
        { 23, 59, 9, 6 },
        { 15, 50, 6, 9 },
        { 30, 50, 6, 9 },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *key = lv_obj_create(tile);
        lv_obj_set_size(key, dp_keys[i].w, dp_keys[i].h);
        lv_obj_set_style_radius(key, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(key, dpad_face, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(key, dpad_edge, LV_PART_MAIN);
        lv_obj_set_style_border_width(key, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(key, 0, LV_PART_MAIN);
        lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(key, LV_ALIGN_TOP_MID, dp_keys[i].x, dp_keys[i].y);
    }

    // Centre circle (OK / select key)
    lv_obj_t *dp_ctr = lv_obj_create(tile);
    lv_obj_set_size(dp_ctr, 6, 6);
    lv_obj_set_style_radius(dp_ctr, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dp_ctr, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dp_ctr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dp_ctr, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(dp_ctr, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dp_ctr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dp_ctr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dp_ctr, LV_ALIGN_TOP_MID, 23, 51);
}

// Lower-right: TPMS — tire ring with a small valve stem
static void draw_tpms_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Tire (thick gray ring)
    lv_obj_t *tire = lv_obj_create(tile);
    lv_obj_set_size(tire, 84, 84);
    lv_obj_set_style_radius(tire, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tire, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(tire, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_border_width(tire, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tire, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tire, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tire, LV_ALIGN_TOP_MID, 0, 24);

    // Inner rim ring for contrast
    lv_obj_t *rim = lv_obj_create(tile);
    lv_obj_set_size(rim, 32, 32);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(rim, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(rim, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rim, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rim, LV_ALIGN_TOP_MID, 0, 50);

    // Valve stem protruding from the bottom of the tire
    lv_obj_t *valve = lv_obj_create(tile);
    lv_obj_set_size(valve, 6, 12);
    lv_obj_set_style_radius(valve, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(valve, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(valve, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(valve, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(valve, 0, LV_PART_MAIN);
    lv_obj_clear_flag(valve, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(valve, LV_ALIGN_TOP_MID, 0, 102);
}

// Lower-right: Mouse — rounded body with a button-divider line and scroll wheel
static void draw_mouse_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Mouse body — rounded, taller than wide
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 62, 92);
    lv_obj_set_style_radius(body, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 26);

    // Vertical divider separating the two top buttons
    lv_obj_t *divider = lv_obj_create(tile);
    lv_obj_set_size(divider, 2, 32);
    lv_obj_set_style_bg_color(divider, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 30);

    // Scroll wheel
    lv_obj_t *wheel = lv_obj_create(tile);
    lv_obj_set_size(wheel, 8, 16);
    lv_obj_set_style_radius(wheel, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wheel, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wheel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wheel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wheel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(wheel, LV_ALIGN_TOP_MID, 0, 42);
}

// APRS — broadcast antenna with two radiating signal arcs
static void draw_aprs_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Signal arcs radiating up from the transmitter tip (tip centre at y=59)
    for (int i = 0; i < 2; i++) {
        lv_coord_t d = 40 + i * 30;
        lv_obj_t *wave = lv_arc_create(tile);
        lv_obj_set_size(wave, d, d);
        lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(wave, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(wave, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(wave, 0, LV_PART_MAIN);
        lv_obj_set_style_arc_color(wave, ARGUS_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(wave, 4, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_arc_set_bg_angles(wave, 0, 360);
        lv_arc_set_angles(wave, 210, 330);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(wave, LV_ALIGN_TOP_MID, 0, 59 - d / 2);
    }

    // Transmitter tip
    lv_obj_t *tip = lv_obj_create(tile);
    lv_obj_set_size(tip, 14, 14);
    lv_obj_set_style_radius(tip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tip, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 52);

    // Antenna mast
    lv_obj_t *mast = lv_obj_create(tile);
    lv_obj_set_size(mast, 5, 52);
    lv_obj_set_style_radius(mast, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mast, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mast, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mast, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mast, LV_ALIGN_TOP_MID, 0, 64);

    // Antenna base
    lv_obj_t *base = lv_obj_create(tile);
    lv_obj_set_size(base, 36, 5);
    lv_obj_set_style_radius(base, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(base, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 112);
}

// Tesla charge-port icon — stylized rear-quarter view of a real Tesla
// charge port: a rounded matte-black housing with the three connector
// prongs visible inside. A small red dot off to the side stands in for
// the port-status LED so the icon doesn't read as "generic outlet".
static void draw_tesla_cp_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    // Outer port housing — matte black with subtle bezel.
    lv_obj_t *port = lv_obj_create(tile);
    lv_obj_set_size(port, 120, 78);
    lv_obj_set_style_radius(port, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(port, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(port, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(port, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(port, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(port, 0, LV_PART_MAIN);
    lv_obj_clear_flag(port, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(port, LV_ALIGN_TOP_MID, 0, 38);

    // Three connector prongs, evenly spaced across the housing's middle.
    for (int i = -1; i <= 1; i++) {
        lv_obj_t *prong = lv_obj_create(port);
        lv_obj_set_size(prong, 12, 20);
        lv_obj_set_style_radius(prong, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(prong, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(prong, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(prong, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(prong, 0, LV_PART_MAIN);
        lv_obj_clear_flag(prong, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(prong, LV_ALIGN_CENTER, i * 24, -6);
    }

    // Tiny red status LED — the "is the port unlocked" tell on a real Tesla.
    // Makes the icon legible as a *Tesla* charge port rather than a power
    // socket, without resorting to the literal Tesla logo.
    lv_obj_t *led = lv_obj_create(port);
    lv_obj_set_size(led, 8, 8);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(led, lv_color_make(0xFF, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(led, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(led, 0, LV_PART_MAIN);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(led, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
}

// --- pwnpet + passive handshake capture (Phase 3a: WiFi-beacon cluster) -------

// Passive WPA handshake / PMKID capture toggle. Green while capturing (matches
// the other detector tiles' "running" state).
static void set_handshake_tile_running(bool on)
{
    lv_obj_set_style_bg_color(t_handshake,
        on ? lv_color_make(0x00, 0x55, 0x22) : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_handshake_clicked(lv_event_t *)
{
    if (argus_mode_current() != ArgusMode::Offense) return;   // offensive: Offense mode only
    if (handshake_is_running()) {
        handshake_stop();
        set_handshake_tile_running(false);
    } else {
        bool ok = handshake_start();
        if (!ok) show_radio_conflict_dialog(false);  // WiFi feature blocked by BT
        set_handshake_tile_running(ok);
    }
}

// Threat Radar — concentric sweep rings with a single red blip, evoking a
// radar scope. Rings are transparent circles with an ARGUS steel-blue border;
// the blip is a contact riding a ring, the spoke a faint sweep line. (Fork drew
// this in matrix-green; rethemed to the ARGUS accent, threat blip in HADES.)
static void draw_radar_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    lv_color_t accent = ARGUS_ACCENT;
    const int rings[3] = { 96, 64, 32 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(tile);
        lv_obj_set_size(r, rings[i], rings[i]);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(r, accent, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 30 + (96 - rings[i]) / 2);
    }
    lv_obj_t *spoke = lv_obj_create(tile);
    lv_obj_set_size(spoke, 3, 48);
    lv_obj_set_style_radius(spoke, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(spoke, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(spoke, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(spoke, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(spoke, 0, LV_PART_MAIN);
    lv_obj_clear_flag(spoke, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(spoke, LV_ALIGN_TOP_MID, 18, 34);

    lv_obj_t *blip = lv_obj_create(tile);
    lv_obj_set_size(blip, 14, 14);
    lv_obj_set_style_radius(blip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(blip, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(blip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(blip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(blip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(blip, LV_ALIGN_TOP_MID, 28, 44);
}

// HexHound — an angular cyber-hound head (skull + pointed ears + scanner eyes +
// snout) in ARGUS steel-blue, matching the pet screen's creature.
static void draw_pet_icon(lv_obj_t *tile)
{
    // Packet Pup sprite (SD /HexHound/pup_icon.png), matching the HexHound pet's
    // remastered art. Decodes lazily on the first Tools-screen view, when the SD
    // card is mounted; a missing card just leaves the icon blank (the "HexHound"
    // label still shows). The EVENT_BUBBLE flag added to every tile child in
    // tools_screen_create() makes a tap on the image reach the tile handler.
    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, "A:/HexHound/pup_icon.png");
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    // pup_icon.png is 142px native — without this it renders full-size and buries
    // the label. Scale + top-centre pivot so it anchors high like the HD sprites
    // in tile_icon() (a centre pivot would leave the scaled render floating low).
    lv_image_set_scale(img, 142);                // 142 * 142/256 ~= 79px
    lv_image_set_pivot(img, 71, 0);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);
}

// Handshake capture — signal rings with a captured packet dropping out (orange).
static void draw_handshake_icon(lv_obj_t *tile)
{
    tile = icon_layer(tile);

    lv_color_t o = lv_color_make(0xff, 0x8c, 0x1a);
    const int d[3] = { 96, 66, 36 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *a = lv_obj_create(tile);
        lv_obj_set_size(a, d[i], d[i]);
        lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(a, o, LV_PART_MAIN);
        lv_obj_set_style_border_width(a, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 30 + (96 - d[i]) / 2);
    }
    lv_obj_t *pkt = lv_obj_create(tile);
    lv_obj_set_size(pkt, 22, 16);
    lv_obj_set_style_radius(pkt, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pkt, o, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pkt, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(pkt, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pkt, 0, LV_PART_MAIN);
    lv_obj_clear_flag(pkt, LV_OBJ_FLAG_SCROLLABLE);
    // The packet block is the lowest element in this icon. At y=124 its 16px
    // height put its scaled bottom at tile y~96.8, just past the label top
    // (~94), so it clipped the top of the "w" in "Pwn". y=112 lands the bottom
    // at ~87.5, clear of the label.
    lv_obj_align(pkt, LV_ALIGN_TOP_MID, 0, 112);
}

// Write the current tile order (top-to-bottom, left-to-right) to SD, one stable
// key per line. Same SD guard pattern the settings / detector code uses.
static void tools_order_save()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(TOOLS_ORDER_PATH, FILE_WRITE);   // FILE_WRITE = "w" (truncate)
    if (!f) return;
    if (tools_grid) {
        uint32_t n = lv_obj_get_child_count(tools_grid);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *tile = lv_obj_get_child(tools_grid, i);
            const char *key = (const char *)lv_obj_get_user_data(tile);
            if (key) f.printf("%s\n", key);
        }
    }
    f.close();
}

// Reorder the freshly-created tiles to match a saved order (if any). Walk the
// saved keys in order, pulling each matching tile to the front in turn. Tiles
// whose key is absent from the file (e.g. added in a later firmware) keep their
// relative position after the restored ones, so the grid never loses a tile.
static void tools_order_load(lv_obj_t *grid)
{
    if (!instance.isCardReady()) return;
    if (!SD.exists(TOOLS_ORDER_PATH)) return;
    File f = SD.open(TOOLS_ORDER_PATH, FILE_READ);
    if (!f) return;
    uint32_t target = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        uint32_t n = lv_obj_get_child_count(grid);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *tile = lv_obj_get_child(grid, i);
            const char *key = (const char *)lv_obj_get_user_data(tile);
            if (key && line.equals(key)) {
                lv_obj_move_to_index(tile, (int32_t)target);
                target++;
                break;
            }
        }
    }
    f.close();
}

// A tile that was just dragged also emits LV_EVENT_CLICKED on release (LVGL
// fires CLICKED even after a long press). This guard is registered BEFORE each
// tile's real action callback, so on release it runs first and stops the event
// when a drag armed the suppressor -- a drag never triggers the tile feature. A
// plain short tap leaves s_suppress_click false, so the action fires normally.
static void tile_click_guard(lv_event_t *e)
{
    if (s_suppress_click) {
        s_suppress_click = false;
        lv_event_stop_processing(e);
    }
}

// Fresh press: clear any stale suppressor left by a drag that released between
// slots (no CLICKED followed to consume it), so a later real tap is never eaten.
static void tile_pressed(lv_event_t *)
{
    s_suppress_click = false;
}

// Long press lifts the tile into drag mode: highlight its border and freeze
// grid scrolling so vertical finger motion reorders instead of scrolling. We do
// NOT move it to the foreground -- that would change its flex child index and
// jump it to the end of the grid.
static void tile_long_pressed(lv_event_t *e)
{
    lv_obj_t *tile = (lv_obj_t *)lv_event_get_current_target(e);
    s_drag_tile   = tile;
    s_drag_active = true;
    lv_obj_set_style_border_color(tile, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 3, LV_PART_MAIN);
    if (tools_grid) lv_obj_clear_flag(tools_grid, LV_OBJ_FLAG_SCROLLABLE);
}

// While dragging, whichever tile the finger is over becomes the drop target:
// move the dragged tile to that slot so the flex grid re-flows live.
static void tile_pressing(lv_event_t *e)
{
    if (!s_drag_active || !s_drag_tile || !tools_grid) return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    uint32_t n = lv_obj_get_child_count(tools_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *cand = lv_obj_get_child(tools_grid, i);
        if (cand == s_drag_tile) continue;
        lv_area_t a;
        lv_obj_get_coords(cand, &a);
        if (p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2) {
            lv_obj_move_to_index(s_drag_tile, (int32_t)lv_obj_get_index(cand));
            break;
        }
    }
}

// Shared drag teardown: restore the resting border (make_tile defaults),
// re-enable grid scrolling, and persist the new order. arm_suppress is true on
// a normal release (a CLICKED is coming and must be swallowed) and false on a
// lost press (no CLICKED will follow, so leaving the suppressor armed would eat
// the next genuine tap).
static void tile_drag_finish(lv_obj_t *tile, bool arm_suppress)
{
    if (!s_drag_active) return;
    s_drag_active = false;
    lv_obj_set_style_border_color(tile, ARGUS_ACCENT_DIM, LV_PART_MAIN);   // make_tile resting rim
    lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN);
    if (tools_grid) lv_obj_add_flag(tools_grid, LV_OBJ_FLAG_SCROLLABLE);
    s_drag_tile = nullptr;
    if (arm_suppress) s_suppress_click = true;
    tools_order_save();
}

static void tile_released(lv_event_t *e)
{
    tile_drag_finish((lv_obj_t *)lv_event_get_current_target(e), true);
}

static void tile_press_lost(lv_event_t *e)
{
    tile_drag_finish((lv_obj_t *)lv_event_get_current_target(e), false);
}

void tools_screen_create()
{
    tools_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tools_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tools_screen, 0, LV_PART_MAIN);

    // Title
    tools_title = lv_label_create(tools_screen);
    lv_obj_set_style_text_color(tools_title, argus_base_accent(), LV_PART_MAIN);
    lv_obj_set_style_text_font(tools_title, &font_argus_label_28, LV_PART_MAIN);
    lv_label_set_text(tools_title, "TOOLS");
    lv_obj_align(tools_title, LV_ALIGN_TOP_MID, 0, 8);

    // Three-column flex grid. ROW_WRAP gives us 3 tiles per row: three 118px
    // tiles plus two 12px column gaps is 378px, which fits the 384px inner
    // width, while a fourth would not. The container scrolls vertically, which
    // it does today - 27 tiles is 9 rows.
    lv_obj_t *grid = lv_obj_create(tools_screen);
    lv_obj_set_size(grid, 400, 432);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(grid, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
        LV_FLEX_ALIGN_SPACE_EVENLY,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);
    tools_grid = grid;   // remembered for save() and the drag handlers

    // DEFAULT tile order (insertion order maps to row-major, grid wraps every 3):
    // grouped by purpose, most safety-relevant first. Users can long-press-drag
    // to reorder; that is persisted to tools_order.txt and overrides this default
    // (via tools_order_load below). This block is only the first-run layout.
    // Visibility is NOT decided here - tile_mode() below is the authority, and
    // tools_apply_mode() hides on it: Daily hides ALL 27, Defense shows the 19
    // non-Offense tiles, Offense shows ONLY its 8. Grouping below is the
    // first-run ORDER; it deliberately mirrors tile_mode()'s classes so the two
    // do not drift apart again.
    //   Defense (16):  Radar, Deauth, Timeline, AirTag, Trackers, Spycam,
    //                  NFC Field, Flock, Skimmers, Flipper, WiFi, Analyze,
    //                  Evil Twin, HexHound, Pager, TPMS
    //   Daily (3):     Notify, LoRa APRS, USB SD  (neutral, survive a Daily glance)
    //   Offense (8):   Pwn, Loot, Beacon, Deauther, Rogue AP, Probes, Mouse, Tesla CP
    // Note "Deauth" is the DETECTOR (Defense, always live); "Deauther" is the
    // attack tool (Offense). Pwn/Mouse/Tesla CP read as neutral but are Offense.
    //   Daily / comms:                Notify, Pager, LoRa APRS, HexHound
    //   Peripheral tools:             Mouse, USB SD, TPMS, Tesla CP
    // The timepiece tiles (Alarm / Stopwatch / Timer / Calendar) live on the TIME
    // screen (swipe up from the clock face), not here.
    lv_obj_t *t_radar   = make_tile(grid, "Radar");
    lv_obj_t *t_deauth  = make_tile(grid, "Deauth");
    lv_obj_t *t_timeline= make_tile(grid, "Timeline");
    t_airtag            = make_tile(grid, "AirTag");
    t_trackers          = make_tile(grid, "Trackers");
    lv_obj_t *t_spycam  = make_tile(grid, "Spycam");
    lv_obj_t *t_nfcfld  = make_tile(grid, "NFC Field");
    t_flock             = make_tile(grid, "Flock");
    t_skimmer           = make_tile(grid, "Skimmers");
    t_flipper           = make_tile(grid, "Flipper");
    lv_obj_t *t_wifi    = make_tile(grid, "WiFi");
    lv_obj_t *t_analyze = make_tile(grid, "Analyze");
    t_eviltwin          = make_tile(grid, "Evil Twin");
    t_handshake         = make_tile(grid, "Pwn");
    lv_obj_t *t_loot    = make_tile(grid, "Loot");
    lv_obj_t *t_beacon  = make_tile(grid, "Beacon");
    lv_obj_t *t_deauthatk = make_tile(grid, "Deauther");
    lv_obj_t *t_rogueap = make_tile(grid, "Rogue AP");
    lv_obj_t *t_probes  = make_tile(grid, "Probes");
    lv_obj_t *t_notify  = make_tile(grid, "Notify");
    lv_obj_t *t_pager   = make_tile(grid, "Pager");
    lv_obj_t *t_aprs    = make_tile(grid, "LoRa APRS");
    lv_obj_t *t_pet     = make_tile(grid, "HexHound");
    lv_obj_t *t_mouse   = make_tile(grid, "Mouse");
    lv_obj_t *t_usbsd   = make_tile(grid, "USB SD");
    lv_obj_t *t_tpms    = make_tile(grid, "TPMS");
    lv_obj_t *t_tesla   = make_tile(grid, "Tesla CP");

    // ARGUS HD sprites from SD /Icons/<name>.png, procedural draw_*_icon as
    // fallback. Pager uses the procedural icon on purpose (the 13-37 gadget); Flipper
    // and HexHound keep their existing image icons.
    tile_icon(t_wifi,     "wifi",     draw_wifi_icon);
    tile_icon(t_analyze,  "analyzer", draw_analyzer_icon);
    tile_icon(t_mouse,    "mouse",    draw_mouse_icon);
    tile_icon(t_usbsd,    "microsd",  draw_microsd_icon);
    draw_pager_icon(t_pager);                                   // 13-37 procedural pager
    tile_icon(t_tpms,     "tpms",     draw_tpms_icon);
    tile_icon(t_aprs,     "aprs",     draw_aprs_icon);
    tile_icon(t_tesla,    "tesla",    draw_tesla_cp_icon);
    tile_icon(t_airtag,   "airtag",   draw_airtag_icon);
    tile_icon(t_trackers, "trackers", draw_trackers_icon);
    tile_icon(t_spycam,   "spycam",   draw_spycam_icon);
    tile_icon(t_nfcfld,   "nfcfield", draw_nfcfield_icon);
    draw_flipper_icon(t_flipper);                               // keep 13-37 Flipper logo
    tile_icon(t_skimmer,  "skimmer",  draw_skimmer_icon);
    tile_icon(t_eviltwin, "eviltwin", draw_eviltwin_icon);
    tile_icon(t_flock,    "flock",    draw_flock_icon);
    tile_icon(t_radar,    "radar",    draw_radar_icon);
    tile_icon(t_deauth,   "deauth",   draw_deauth_icon);
    tile_icon(t_timeline, "timeline", draw_timeline_icon);
    draw_pet_icon(t_pet);                                       // keep HexHound HD sprite
    tile_icon(t_handshake, "pwn",     draw_handshake_icon);
    tile_icon(t_loot,     "loot",     draw_loot_icon);
    tile_icon(t_beacon,   "beaconspam", draw_beaconspam_icon);
    tile_icon(t_deauthatk, "deauthatk", draw_deauth_atk_icon);
    tile_icon(t_rogueap,  "rogueap",  draw_rogueap_icon);
    tile_icon(t_probes,   "probes",   draw_probes_icon);
    tile_icon(t_notify,   "notify",   draw_notify_icon);

    // --- Rearrangeable-grid wiring ---------------------------------------
    // Give every tile a STABLE key (independent of its label) and attach the
    // long-press drag handlers. The CLICKED "guard" is registered HERE, before
    // each tile's real action callback below, so on release it runs first and
    // can swallow the click that a drag would otherwise trigger. Keys are the
    // stable identifiers written to /Settings/tools_order.txt.
    struct TileKey { lv_obj_t *tile; const char *key; };
    const TileKey tile_keys[] = {
        { t_wifi,     "wifi"      },
        { t_analyze,  "analyze"   },
        { t_mouse,    "mouse"     },
        { t_usbsd,    "usbsd"     },
        { t_pager,    "pager"     },
        { t_tpms,     "tpms"      },
        { t_aprs,     "aprs"      },
        { t_tesla,    "tesla"     },
        { t_airtag,   "airtag"    },
        { t_trackers, "trackers"  },
        { t_spycam,   "spycam"    },
        { t_nfcfld,   "nfcfield"  },
        { t_flipper,  "flipper"   },
        { t_skimmer,  "skimmer"   },
        { t_eviltwin, "eviltwin"  },
        { t_flock,    "flock"     },
        { t_radar,    "radar"     },
        { t_deauth,   "deauth"    },
        { t_timeline, "timeline"  },
        { t_pet,      "pet"       },
        { t_handshake,"handshake" },
        { t_loot,     "loot"      },
        { t_beacon,   "beaconspam" },
        { t_deauthatk,"deauthatk" },
        { t_rogueap,  "rogueap"   },
        { t_probes,   "probes"    },
        { t_notify,   "notify"    },
    };
    for (auto &tk : tile_keys) {
        lv_obj_set_user_data(tk.tile, (void *)tk.key);
        lv_obj_add_event_cb(tk.tile, tile_pressed,      LV_EVENT_PRESSED,      NULL);
        lv_obj_add_event_cb(tk.tile, tile_click_guard,  LV_EVENT_CLICKED,      NULL);
        lv_obj_add_event_cb(tk.tile, tile_long_pressed, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(tk.tile, tile_pressing,     LV_EVENT_PRESSING,     NULL);
        lv_obj_add_event_cb(tk.tile, tile_released,     LV_EVENT_RELEASED,     NULL);
        lv_obj_add_event_cb(tk.tile, tile_press_lost,   LV_EVENT_PRESS_LOST,   NULL);
    }

    // Tesla CP tile opens the 315 MHz charge-port-open transmit screen.
    lv_obj_add_event_cb(t_tesla, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; tesla_cp_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // AirTag tile toggles the BLE Find My sniffer and swaps to a dim green
    // background while running.
    lv_obj_add_event_cb(t_airtag, on_airtag_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_trackers, on_trackers_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_spycam, [](lv_event_t *) { spycam_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_nfcfld, [](lv_event_t *) { nfc_field_screen_show(); }, LV_EVENT_CLICKED, NULL);
    set_airtag_tile_running(airtag_is_running());
    set_trackers_tile_running(tracker_sweep_is_running());

    // Flipper tile toggles the BLE Flipper Zero detector. Same dim-green
    // running indication as AirTag.
    lv_obj_add_event_cb(t_flipper, on_flipper_clicked, LV_EVENT_CLICKED, NULL);
    set_flipper_tile_running(flipper_is_running());

    // Skimmers tile toggles the HC-0x card-skimmer detector. Same green-
    // when-running affordance as AirTag and Flipper.
    lv_obj_add_event_cb(t_skimmer, on_skimmer_clicked, LV_EVENT_CLICKED, NULL);
    set_skimmer_tile_running(skimmer_is_running());

    // Evil Twin tile toggles the rogue-AP detector (WiFi beacon scan).
    lv_obj_add_event_cb(t_eviltwin, on_eviltwin_clicked, LV_EVENT_CLICKED, NULL);
    set_eviltwin_tile_running(evil_twin_is_running());

    // Flock tile toggles the surveillance-vendor detector (WiFi + BLE scan).
    lv_obj_add_event_cb(t_flock, on_flock_clicked, LV_EVENT_CLICKED, NULL);

    // HexHound tile opens the cyber-recon pet; Pwn tile toggles passive handshake capture.
    // Radar tile opens the Threat Radar spatio-temporal correlation screen.
    lv_obj_add_event_cb(t_radar, [](lv_event_t *) { threat_radar_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_deauth, [](lv_event_t *) { deauth_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_timeline, [](lv_event_t *) { tracker_timeline_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_pet, [](lv_event_t *) { pet_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_handshake, on_handshake_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_loot, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; loot_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_beacon, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; beacon_spam_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_deauthatk, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; deauth_attack_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_rogueap, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; rogue_ap_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_probes, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; probe_sniffer_screen_show(); }, LV_EVENT_CLICKED, NULL);
    set_flock_tile_running(flock_is_running());

    // TPMS tile opens the TPMS monitor screen.
    lv_obj_add_event_cb(t_tpms, [](lv_event_t *) { tpms_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Pager tile opens the POCSAG/FLEX decoder screen.
    lv_obj_add_event_cb(t_pager, [](lv_event_t *) { pager_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Mouse tile opens the Bluetooth HID mouse screen.
    lv_obj_add_event_cb(t_mouse, [](lv_event_t *) { if (argus_mode_current() != ArgusMode::Offense) return; mouse_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // USB SD tile opens the USB mass-storage card-reader screen.
    lv_obj_add_event_cb(t_usbsd, [](lv_event_t *) { usb_sd_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // APRS tile opens the LoRa APRS receive/transmit screen.
    lv_obj_add_event_cb(t_aprs, [](lv_event_t *) { aprs_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // WiFi tile opens the site-survey + ping-sweep screen.
    lv_obj_add_event_cb(t_wifi, [](lv_event_t *) { wifi_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Analyze tile opens the WiFi channel utilisation visualisation.
    lv_obj_add_event_cb(t_analyze, [](lv_event_t *) { analyze_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Notify tile opens the phone-notification mirror (ANCS today; Gadgetbridge later).
    lv_obj_add_event_cb(t_notify, [](lv_event_t *) { notifications_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // lv_obj_create() creates objects with LV_OBJ_FLAG_CLICKABLE set by
    // default, so the icon shapes inside each tile would otherwise swallow
    // CLICKED events instead of letting them reach the tile. Walk every tile
    // and add LV_OBJ_FLAG_EVENT_BUBBLE to each descendant so a tap anywhere
    // inside the tile (icon shapes, label, or background) reaches the tile's
    // CLICKED handler. The walk is RECURSIVE because icon_layer() nests the
    // procedural glyph primitives one level deeper (grandchildren of the tile).
    struct BubbleWalk {
        static void apply(lv_obj_t *obj) {
            uint32_t n = lv_obj_get_child_count(obj);
            for (uint32_t k = 0; k < n; k++) {
                lv_obj_t *child = lv_obj_get_child(obj, k);
                lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
                apply(child);
            }
        }
    };
    uint32_t tile_count = lv_obj_get_child_count(grid);
    for (uint32_t i = 0; i < tile_count; i++) {
        BubbleWalk::apply(lv_obj_get_child(grid, i));
    }

    // Restore the user's saved tile order now that every tile has its key.
#ifndef SCREENSHOT_AUTO
    tools_order_load(grid);
#else
    // The screenshot-capture build ignores any saved order so the grid always
    // renders the true default layout for the README, regardless of SD state.
#endif

    lv_obj_add_event_cb(tools_screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(grid, on_grid_scroll, LV_EVENT_SCROLL, NULL);   // smooth-scroll priority

    // Gate the grid to the current ARGUS mode now, and re-apply on every mode
    // change (even when Tools isn't the active screen) so it's correct on entry.
    argus_mode_on_change([](ArgusMode) { tools_apply_mode(); });
    tools_apply_mode();
}

// Lowest ArgusMode at which a tile is allowed to appear. Offensive tools require
// Offense; the three innocent utilities are Daily; everything else is a Defense
// detector/recon tool. (Classification per tasks/MODE-ARCHITECTURE-PLAN.md sec 4.)
static ArgusMode tile_mode(const char *key)
{
    if (!key) return ArgusMode::Defense;
    if (!strcmp(key, "handshake") || !strcmp(key, "mouse") ||
        !strcmp(key, "tesla") || !strcmp(key, "loot") ||
        !strcmp(key, "beaconspam") || !strcmp(key, "deauthatk") ||
        !strcmp(key, "rogueap") || !strcmp(key, "probes"))
        return ArgusMode::Offense;
    if (!strcmp(key, "notify") || !strcmp(key, "aprs") || !strcmp(key, "usbsd"))
        return ArgusMode::Daily;
    return ArgusMode::Defense;
}

void tools_apply_mode()
{
    if (!tools_grid) return;
    ArgusMode cur = argus_mode_current();
    uint32_t n = lv_obj_get_child_count(tools_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *tile = lv_obj_get_child(tools_grid, i);
        ArgusMode tm = tile_mode((const char *)lv_obj_get_user_data(tile));
        bool visible;
        switch (cur) {
        // Strict separation: Offense shows ONLY offensive tools (no defensive
        // detectors bleeding in). Defense shows the detectors PLUS the neutral
        // day-to-day utilities (notify/aprs/usbsd, classified Daily) so they stay
        // reachable - Daily gates the whole grid, so those have no other home yet.
        // Where the neutral utilities ultimately live is a redesign decision.
        case ArgusMode::Offense: visible = (tm == ArgusMode::Offense); break;
        case ArgusMode::Defense: visible = (tm != ArgusMode::Offense); break;
        default:                 visible = false;                      break;  // Daily: hide all (grid is gated anyway)
        }
        if (visible) lv_obj_clear_flag(tile, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN);
    }
}

// Attach a "swipe-DOWN -> Tools grid" shortcut to any tool/radio sub-screen so
// the grid is reachable from anywhere (not only by back-chaining to the clock).
// Mirrors the clock's swipe-down->Tools and is gated to Defense/Offense (Daily
// hides Tools). Screens with vertically-scrolling content still scroll; the
// gesture only fires when the scroll doesn't consume the swipe.
static void tools_jump_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM &&
        argus_mode_current() != ArgusMode::Daily)
        tools_screen_show();
}

void tools_attach_jump_gesture(lv_obj_t *screen)
{
    if (screen) lv_obj_add_event_cb(screen, tools_jump_gesture_cb, LV_EVENT_GESTURE, NULL);
}

void tools_screen_show()
{
    main_loop_request_lvgl_priority(12);
    tools_apply_mode();   // reflect the current mode before the screen paints
    // Repaint the title with the MODE accent on entry: red-team red in Offense,
    // calm steel-blue in Daily/Defense. Deliberately argus_base_accent(), not
    // argus_accent(): a live threat must not turn Defense-side headings red.
    if (tools_title) lv_obj_set_style_text_color(tools_title, argus_base_accent(), LV_PART_MAIN);
    lv_scr_load(tools_screen);
}
bool tools_screen_is_active() { return lv_screen_active() == tools_screen; }
