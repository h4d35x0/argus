#include "background.h"
#include "argus_mode.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>          // millis()
#include <esp_heap_caps.h>

// Mode-aware wallpaper - each mode gets its own vibe:
//   Daily   -> /backgrounds/daily.rgb565    (ARGUS logo)
//   Defense -> /backgrounds/defense.rgb565  (Privacy is an Illusion)
//   Offense -> /backgrounds/offense.rgb565  (skull, red-team)
//
// Each wallpaper is a raw, panel-sized RGB565 file loaded ONCE into a PSRAM
// buffer and handed to LVGL as an in-memory image. This deliberately avoids the
// SD image DECODERS, all unfit on this board:
//   - PNG (LODEPNG) / JPEG (TJPGD) buffer the WHOLE compressed file in scarce
//     internal SRAM to inflate it -> OOM-crash / boot-loop on battery.
//   - The BMP decoder streams via get_area and RE-DECODES from the SD every
//     render, starving the loop enough to break the polled BOOT button.
// A raw RGB565 image needs no decode: one copy into PSRAM, then every render is
// a fast blit. Because each mode's raster lives in PSRAM, switching wallpaper on
// a mode change is just an lv_image_set_src() to the other buffer - instant, no
// decode, no SD read, so it cannot OOM/lag/brownout at swap time.
// See memory: bug-offense-wallpaper-runtime-decode-bootloop.
//
// Files are little-endian RGB565 (matching LV_COLOR_DEPTH 16), exactly WP_W*WP_H
// *2 bytes. A missing mode file falls back to Daily; a missing Daily file =
// graceful no-wallpaper. Never a crash.

static lv_obj_t *bg_img     = nullptr;
static bool      bg_enabled = false;
static uint8_t   bg_opa     = 75;      // faint by design (0..255)

// The panel is 410x502; each raw wallpaper must match exactly (no scaling).
static const int32_t WP_W = 410;
static const int32_t WP_H = 502;

// One raw wallpaper: its SD path, PSRAM buffer, and LVGL image source.
struct Raster {
    const char    *path;
    uint16_t      *buf;
    lv_image_dsc_t dsc;
    bool           loaded;
};
// Indexed by ArgusMode (Daily=0, Defense=1, Offense=2).
static Raster s_rasters[3] = {
    { "/backgrounds/daily.rgb565",   nullptr, {}, false },
    { "/backgrounds/defense.rgb565", nullptr, {}, false },
    { "/backgrounds/offense.rgb565", nullptr, {}, false },
};

// Defer the first wallpaper load until the rest of boot has settled. The exact
// panic was in the BHI260 callback rather than this SD/PSRAM path, but keeping
// the raster load outside early initialization avoids unnecessary overlap.
static const uint32_t kBootSettleMs = 10000;
static lv_timer_t    *s_defer_timer = nullptr;

// NOTE (2026-07-24): the fade-in ramp was REMOVED. It stepped opacity 0 -> bg_opa
// over ~825ms, and every step invalidated the full-screen image -> ~15 full-screen
// ALPHA-BLEND re-renders in under a second. On battery that sustained render burst
// tripped the interrupt watchdog (bootlog reset=5 INT_WDT, NOT 9 brownout), which
// is a BLOCKING problem, not a current-spike problem a gentle ramp would help. So
// the wallpaper now renders ONCE, directly at bg_opa. See memory
// bug-offense-wallpaper-runtime-decode-bootloop.

// Only the CURRENT mode raster is loaded on demand. Pre-loading all 3 rasters in
// setup() tripled the crash exposure window on battery and made the boot loop
// worse, so we stay on the single-raster baseline until a real backtrace says
// otherwise.

static void ensure_backgrounds_dir()
{
    if (!instance.isCardReady()) return;
    if (SD.exists("/backgrounds")) return;
    SD.mkdir("/backgrounds");
}

// Load one raster from the SD into a PSRAM buffer, once. Returns false (never
// crashes) if the card, file, exact byte size, or PSRAM is unavailable.
static bool load_raster(Raster *r)
{
    if (r->loaded) return true;
    if (!instance.isCardReady()) return false;
    if (!SD.exists(r->path))     return false;

    File f = SD.open(r->path);
    if (!f) return false;
    size_t need = (size_t)WP_W * (size_t)WP_H * 2u;
    if ((size_t)f.size() < need) { f.close(); return false; }

    if (!r->buf) {
        r->buf = (uint16_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (!r->buf) { f.close(); return false; }
    }
    size_t rd = f.read((uint8_t *)r->buf, need);
    f.close();
    if (rd != need) return false;

    r->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    r->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    r->dsc.header.flags  = 0;
    r->dsc.header.w      = WP_W;
    r->dsc.header.h      = WP_H;
    r->dsc.header.stride = WP_W * 2;
    r->dsc.data_size     = need;
    r->dsc.data          = (const uint8_t *)r->buf;
    r->loaded = true;
    return true;
}

// Show the wallpaper for the CURRENT mode; fall back to Daily if that mode's
// file is missing, or hide if even Daily is absent.
static void apply_current()
{
    if (!bg_img || !bg_enabled) return;

    int idx = (int)argus_mode_current();
    if (idx < 0 || idx > 2) idx = 0;

    Raster *r = nullptr;
    if (load_raster(&s_rasters[idx]))   r = &s_rasters[idx];
    else if (load_raster(&s_rasters[0])) r = &s_rasters[0];   // fall back to Daily
    if (!r) { lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN); return; }
    int32_t w = lv_display_get_horizontal_resolution(NULL);
    int32_t h = lv_display_get_vertical_resolution(NULL);
    lv_obj_set_size(bg_img, w, h);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);

    lv_image_set_src(bg_img, NULL);
    lv_image_set_src(bg_img, &r->dsc);
    lv_image_set_inner_align(bg_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_move_background(bg_img);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_image_opa(bg_img, bg_opa, LV_PART_MAIN);   // render ONCE at target opacity (no fade)
}

// One-shot: fires ~kBootSettleMs after boot, once the current surge has passed.
static void defer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_defer_timer = nullptr;
    if (bg_enabled) apply_current();
}

// Mode changed: swap to that mode's wallpaper. Before the boot-settle window the
// deferred timer applies the by-then-current mode, so do nothing early.
static void on_mode_change(ArgusMode)
{
    if (bg_enabled && bg_img && millis() >= kBootSettleMs) apply_current();
}

lv_obj_t *background_create(lv_obj_t *parent)
{
    ensure_backgrounds_dir();

    bg_img = lv_image_create(parent);
    lv_obj_remove_style_all(bg_img);
    lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(bg_img);   // lowest z-order (clock + matrix on top)

    argus_mode_on_change(on_mode_change);
    return bg_img;
}

void background_set_enabled(bool en)
{
    bg_enabled = en;
    if (!bg_img) return;
    if (en) {
        // Defer the first wallpaper work past the cold-boot current surge. The
        // current-raster-only baseline is the least-bad known behavior on battery;
        // loading all 3 rasters in setup() made the panic much more frequent.
        if (millis() < kBootSettleMs) {
            if (!s_defer_timer)
                s_defer_timer = lv_timer_create(defer_cb, kBootSettleMs - millis(), NULL);
            lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        apply_current();
    } else {
        if (s_defer_timer) { lv_timer_delete(s_defer_timer); s_defer_timer = nullptr; }
        lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    }
}

bool background_is_enabled() { return bg_enabled; }

void background_set_opacity(uint8_t opa)
{
    bg_opa = opa;
    if (bg_img && bg_enabled && !lv_obj_has_flag(bg_img, LV_OBJ_FLAG_HIDDEN))
        lv_obj_set_style_image_opa(bg_img, bg_opa, LV_PART_MAIN);
}
