#include "tools_screen.h"
#include "theme.h"
#include "airtag.h"
#include "flipper.h"
#include "skimmer.h"
#include "evil_twin.h"
#include "flock.h"
#include "tesla_cp_screen.h"
#include "tpms_screen.h"
#include "pager_screen.h"
#include "mouse_screen.h"
#include "usb_sd_screen.h"
#include "aprs_screen.h"
#include "wifi_screen.h"
#include "analyze_screen.h"
#include <LilyGoLib.h>

// Defined in main.cpp
void clock_screen_show();
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *tools_screen;
static lv_obj_t *t_airtag;    // referenced by on_airtag_clicked for colour swap
static lv_obj_t *t_flipper;   // referenced by on_flipper_clicked for colour swap
static lv_obj_t *t_skimmer;   // referenced by on_skimmer_clicked for colour swap
static lv_obj_t *t_eviltwin;  // referenced by on_eviltwin_clicked for colour swap
static lv_obj_t *t_flock;     // referenced by on_flock_clicked for colour swap

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP)
        clock_screen_show();
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
        set_airtag_tile_running(ok);   // stays gray if BT init failed
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
        set_flipper_tile_running(ok);   // stays gray if BT init failed
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
        set_skimmer_tile_running(ok);   // stays gray if BT init failed
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
        bool ok = flock_start();
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
    lv_obj_set_size(tile, 180, 180);
    lv_obj_set_style_bg_color(tile, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(lbl, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, label_text);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -12);

    return tile;
}

// Upper-left: WiFi — signal glyph in cyan, for the site-survey / ping-sweep tool
static void draw_wifi_icon(lv_obj_t *tile)
{
    lv_obj_t *wifi = lv_label_create(tile);
    lv_obj_set_style_text_color(wifi, lv_color_make(0x33, 0xBB, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_align(wifi, LV_ALIGN_TOP_MID, 0, 44);
}

// Analyze — spectrum-analyzer logo: a row of vertical bars at varying heights
// sitting on a baseline, colours stepping from green through yellow to red to
// suggest channel saturation.
static void draw_analyzer_icon(lv_obj_t *tile)
{
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

// Upper-right: AirTag — round disc with a small dot in the centre
static void draw_airtag_icon(lv_obj_t *tile)
{
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
    // Centre-ish inside the tile; the label sits at the bottom so we
    // anchor a bit higher than dead-centre.
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 26);
}

// Skimmer detector icon — a credit card on its side with a thin magnetic
// stripe and a small red warning chip in the corner, hinting at "card +
// compromise". Reads at-a-glance as "card reader / skimmer".
static void draw_skimmer_icon(lv_obj_t *tile)
{
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
    lv_obj_set_style_text_font(bang, &lv_font_montserrat_20, LV_PART_MAIN);
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
    lv_color_t body_yellow = lv_color_make(0xFF, 0xCC, 0x33);
    lv_color_t body_shade  = lv_color_make(0xCC, 0x99, 0x22);
    lv_color_t btn_face    = lv_color_make(0x10, 0x10, 0x10);
    lv_color_t btn_edge    = lv_color_make(0x44, 0x44, 0x44);
    lv_color_t dpad_face   = lv_color_make(0x28, 0x28, 0x28);
    lv_color_t dpad_edge   = lv_color_make(0x55, 0x55, 0x55);

    // Yellow body -- body spans y=20..116 in tile coords
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 116, 96);
    lv_obj_set_style_radius(body, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, body_yellow, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, body_shade, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 20);

    // Green LCD display
    lv_obj_t *lcd = lv_obj_create(tile);
    lv_obj_set_size(lcd, 96, 30);
    lv_obj_set_style_radius(lcd, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lcd, ARGUS_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(lcd, lv_color_make(0x00, 0x77, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(lcd, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lcd, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lcd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 28);

    // Two darker bands standing in for pager message text
    for (int row = 0; row < 2; row++) {
        lv_obj_t *line = lv_obj_create(tile);
        lv_obj_set_size(line, 72, 3);
        lv_obj_set_style_radius(line, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(line, lv_color_make(0x00, 0x55, 0x22), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 35 + row * 9);
    }

    // ---- Control row ----
    // Left side: green and red action buttons.  Each is an 18x18 rounded-rect
    // key with a small oval LED indicator pill near the top, matching the
    // physical Motorola Advisor indicator lights.

    // Green action button
    lv_obj_t *gbtn = lv_obj_create(tile);
    lv_obj_set_size(gbtn, 18, 18);
    lv_obj_set_style_radius(gbtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gbtn, btn_face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(gbtn, btn_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(gbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gbtn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gbtn, LV_ALIGN_TOP_MID, -38, 76);

    lv_obj_t *gled = lv_obj_create(gbtn);
    lv_obj_set_size(gled, 8, 5);
    lv_obj_set_style_radius(gled, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gled, lv_color_make(0x22, 0xFF, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gled, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(gled, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gled, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gled, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gled, LV_ALIGN_TOP_MID, 0, 3);

    // Red action button
    lv_obj_t *rbtn = lv_obj_create(tile);
    lv_obj_set_size(rbtn, 18, 18);
    lv_obj_set_style_radius(rbtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rbtn, btn_face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(rbtn, btn_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(rbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rbtn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rbtn, LV_ALIGN_TOP_MID, -18, 76);

    lv_obj_t *rled = lv_obj_create(rbtn);
    lv_obj_set_size(rled, 8, 5);
    lv_obj_set_style_radius(rled, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rled, lv_color_make(0xFF, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rled, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rled, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rled, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rled, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rled, LV_ALIGN_TOP_MID, 0, 3);

    // Right side: Advisor-style 4-way directional pad.
    // Four separate rounded keys with a 2 px gap between each arm and the
    // centre circle.  D-pad geometric centre sits at tile (+30, 87).
    //
    // Geometry (tile TOP_MID relative, y from tile top):
    //   up    align(+30, 73) size(12, 8)  bottom at 81
    //   ctr   align(+30, 83) size( 8, 8)  top 83, bot 91   <-- 2 px gaps
    //   down  align(+30, 93) size(12, 8)  top  at 93
    //   left  align(+20, 81) size( 8,12)  right at tile+24  <-- 2 px to ctr
    //   right align(+40, 81) size( 8,12)  left  at tile+36  <-- 2 px from ctr
    struct { int x, y, w, h; } dp_keys[4] = {
        { 30, 73, 12,  8 },
        { 30, 93, 12,  8 },
        { 20, 81,  8, 12 },
        { 40, 81,  8, 12 },
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
    lv_obj_set_size(dp_ctr, 8, 8);
    lv_obj_set_style_radius(dp_ctr, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dp_ctr, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dp_ctr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dp_ctr, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(dp_ctr, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dp_ctr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dp_ctr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dp_ctr, LV_ALIGN_TOP_MID, 30, 83);
}

// Lower-right: TPMS — tire ring with a small valve stem
static void draw_tpms_icon(lv_obj_t *tile)
{
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

void tools_screen_create()
{
    tools_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tools_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tools_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(tools_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "TOOLS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Two-column flex grid. ROW_WRAP gives us 2 tiles per row (since each
    // 180px tile + the 12px column gap exceeds half the 384px inner width),
    // and the container scrolls vertically when future tiles overflow.
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

    // Insertion order maps to row-major (grid wraps every 2 tiles):
    //   [WiFi]      [Analyze]
    //   [Mouse]     [USB SD]
    //   [Pager]     [TPMS]
    //   [LoRa APRS] [Tesla CP]
    //   [AirTag]    [Flipper]
    //   [Skimmers]  [Evil Twin]
    //   [Flock]
    // The timepiece tiles (Alarm / Stopwatch / Timer / Calendar) used to live
    // at the bottom of this grid; they moved to the TIME screen (swipe up
    // from the clock face).
    lv_obj_t *t_wifi    = make_tile(grid, "WiFi");
    lv_obj_t *t_analyze = make_tile(grid, "Analyze");
    lv_obj_t *t_mouse   = make_tile(grid, "Mouse");
    lv_obj_t *t_usbsd   = make_tile(grid, "USB SD");
    lv_obj_t *t_pager   = make_tile(grid, "Pager");
    lv_obj_t *t_tpms    = make_tile(grid, "TPMS");
    lv_obj_t *t_aprs    = make_tile(grid, "LoRa APRS");
    lv_obj_t *t_tesla   = make_tile(grid, "Tesla CP");
    t_airtag            = make_tile(grid, "AirTag");
    t_flipper           = make_tile(grid, "Flipper");
    t_skimmer           = make_tile(grid, "Skimmers");
    t_eviltwin          = make_tile(grid, "Evil Twin");
    t_flock             = make_tile(grid, "Flock");

    draw_wifi_icon(t_wifi);
    draw_analyzer_icon(t_analyze);
    draw_mouse_icon(t_mouse);
    draw_microsd_icon(t_usbsd);
    draw_pager_icon(t_pager);
    draw_tpms_icon(t_tpms);
    draw_aprs_icon(t_aprs);
    draw_tesla_cp_icon(t_tesla);
    draw_airtag_icon(t_airtag);
    draw_flipper_icon(t_flipper);
    draw_skimmer_icon(t_skimmer);
    draw_eviltwin_icon(t_eviltwin);
    draw_flock_icon(t_flock);

    // Tesla CP tile opens the 315 MHz charge-port-open transmit screen.
    lv_obj_add_event_cb(t_tesla, [](lv_event_t *) { tesla_cp_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // AirTag tile toggles the BLE Find My sniffer and swaps to a dim green
    // background while running.
    lv_obj_add_event_cb(t_airtag, on_airtag_clicked, LV_EVENT_CLICKED, NULL);
    set_airtag_tile_running(airtag_is_running());

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
    set_flock_tile_running(flock_is_running());

    // TPMS tile opens the TPMS monitor screen.
    lv_obj_add_event_cb(t_tpms, [](lv_event_t *) { tpms_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Pager tile opens the POCSAG/FLEX decoder screen.
    lv_obj_add_event_cb(t_pager, [](lv_event_t *) { pager_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Mouse tile opens the Bluetooth HID mouse screen.
    lv_obj_add_event_cb(t_mouse, [](lv_event_t *) { mouse_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // USB SD tile opens the USB mass-storage card-reader screen.
    lv_obj_add_event_cb(t_usbsd, [](lv_event_t *) { usb_sd_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // APRS tile opens the LoRa APRS receive/transmit screen.
    lv_obj_add_event_cb(t_aprs, [](lv_event_t *) { aprs_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // WiFi tile opens the site-survey + ping-sweep screen.
    lv_obj_add_event_cb(t_wifi, [](lv_event_t *) { wifi_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Analyze tile opens the WiFi channel utilisation visualisation.
    lv_obj_add_event_cb(t_analyze, [](lv_event_t *) { analyze_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // lv_obj_create() creates objects with LV_OBJ_FLAG_CLICKABLE set by
    // default, so the icon shapes inside each tile would otherwise swallow
    // CLICKED events instead of letting them reach the tile. Walk every tile
    // and add LV_OBJ_FLAG_EVENT_BUBBLE to each of its children so a tap
    // anywhere inside the tile (icon shapes, label, or background) reaches
    // the tile's CLICKED handler.
    uint32_t tile_count = lv_obj_get_child_count(grid);
    for (uint32_t i = 0; i < tile_count; i++) {
        lv_obj_t *tile = lv_obj_get_child(grid, i);
        uint32_t kid_count = lv_obj_get_child_count(tile);
        for (uint32_t j = 0; j < kid_count; j++) {
            lv_obj_add_flag(lv_obj_get_child(tile, j), LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    lv_obj_add_event_cb(tools_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void tools_screen_show()
{
    main_loop_request_lvgl_priority(12);
    lv_scr_load(tools_screen);
}
bool tools_screen_is_active() { return lv_screen_active() == tools_screen; }
