#include "background.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>          // millis()
#include <esp_heap_caps.h>

// The wallpaper is a raw, panel-sized RGB565 image loaded ONCE at boot into a
// PSRAM buffer and handed to LVGL as an in-memory image. This deliberately
// avoids the SD image DECODERS, every one of which is unfit on this board:
//   - PNG (LODEPNG) / JPEG (TJPGD) buffer the WHOLE compressed file in scarce
//     internal SRAM to inflate it, which OOM-crashes the boot on battery.
//   - The BMP decoder streams via get_area and RE-DECODES from the SD on every
//     render, starving the main loop enough to break the polled BOOT button.
// A raw RGB565 image needs no decode at all: one copy from SD into PSRAM at
// boot, then every render is a fast blit from PSRAM. No internal-SRAM pressure,
// no per-render work. See memory: bug-offense-wallpaper-runtime-decode-bootloop.
//
// The file is /backgrounds/wallpaper.rgb565 - exactly WP_W*WP_H*2 bytes of
// little-endian RGB565 (matching LV_COLOR_DEPTH 16). Generate it on the host
// from any image; the firmware only ever reads it.

static lv_obj_t *bg_img     = nullptr;
static bool      bg_enabled = false;
static uint8_t   bg_opa     = 75;      // faint by design (0..255)
static bool      bg_loaded  = false;

static uint16_t      *s_wp_buf = nullptr;   // PSRAM raster, allocated once
static lv_image_dsc_t s_wp_dsc;             // in-memory image source for LVGL

// The panel is 410x502; the raw wallpaper must match exactly (no scaling).
static const int32_t WP_W = 410;
static const int32_t WP_H = 502;
static const char   *kRawPath = "/backgrounds/wallpaper.rgb565";

// Defer the first wallpaper load until this long after boot. Loading + rendering
// the full-screen image while the display and radios are still initialising
// stacks onto the cold-boot current peak and browns out the BATTERY rail ->
// boot-loop (USB's stiff rail is unaffected; steady-state battery draw with the
// wallpaper is fine). Waiting past the surge avoids it. A runtime Settings
// toggle (millis() already past this) enables immediately.
static const uint32_t kBootSettleMs = 6000;
static lv_timer_t    *s_defer_timer = nullptr;

// Create the drop-in folder so the path exists; the firmware only reads it.
static void ensure_backgrounds_dir()
{
    if (!instance.isCardReady()) return;
    if (SD.exists("/backgrounds")) return;
    SD.mkdir("/backgrounds");
}

// Load the raw RGB565 wallpaper from the SD into a PSRAM buffer, once. Returns
// false (stays hidden, never crashes) if the card, the file, the exact byte
// size, or PSRAM is unavailable - all handled as a graceful no-wallpaper.
static bool load_source()
{
    if (!bg_img)   return false;
    if (bg_loaded) return true;
    if (!instance.isCardReady()) return false;
    if (!SD.exists(kRawPath))    return false;

    File f = SD.open(kRawPath);
    if (!f) return false;
    size_t need = (size_t)WP_W * (size_t)WP_H * 2u;
    if ((size_t)f.size() < need) { f.close(); return false; }   // wrong format/size

    if (!s_wp_buf) {
        s_wp_buf = (uint16_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (!s_wp_buf) { f.close(); return false; }             // no PSRAM -> graceful
    }
    size_t rd = f.read((uint8_t *)s_wp_buf, need);
    f.close();
    if (rd != need) return false;

    s_wp_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_wp_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_wp_dsc.header.flags  = 0;
    s_wp_dsc.header.w      = WP_W;
    s_wp_dsc.header.h      = WP_H;
    s_wp_dsc.header.stride = WP_W * 2;
    s_wp_dsc.data_size     = need;
    s_wp_dsc.data          = (const uint8_t *)s_wp_buf;

    int32_t w = lv_display_get_horizontal_resolution(NULL);
    int32_t h = lv_display_get_vertical_resolution(NULL);
    lv_obj_set_size(bg_img, w, h);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);

    lv_image_set_src(bg_img, NULL);
    lv_image_set_src(bg_img, &s_wp_dsc);
    lv_image_set_inner_align(bg_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_set_style_image_opa(bg_img, bg_opa, LV_PART_MAIN);
    bg_loaded = true;
    return true;
}

lv_obj_t *background_create(lv_obj_t *parent)
{
    ensure_backgrounds_dir();

    bg_img = lv_image_create(parent);
    lv_obj_remove_style_all(bg_img);
    lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_SCROLLABLE);
    // Lowest z-order so the clock + matrix rain render on top.
    lv_obj_move_background(bg_img);
    return bg_img;
}

// Do the actual reveal: load the raster (once) and unhide. Shared by the direct
// path and the deferred boot timer.
static void reveal_wallpaper()
{
    if (!bg_img) return;
    if (!bg_loaded && !load_source()) {
        lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_move_background(bg_img);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
}

// One-shot: fires ~kBootSettleMs after boot, once the current surge has passed.
static void defer_enable_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_defer_timer = nullptr;
    if (bg_enabled) reveal_wallpaper();
}

void background_set_enabled(bool en)
{
    bg_enabled = en;
    if (!bg_img) return;
    if (en) {
        // First load during the cold-boot window -> defer past the current surge
        // to avoid a battery brownout (see kBootSettleMs). Stay hidden until then.
        if (!bg_loaded && millis() < kBootSettleMs) {
            if (!s_defer_timer)
                s_defer_timer = lv_timer_create(defer_enable_cb, kBootSettleMs - millis(), NULL);
            lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        reveal_wallpaper();
    } else {
        if (s_defer_timer) { lv_timer_delete(s_defer_timer); s_defer_timer = nullptr; }
        lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    }
}

bool background_is_enabled() { return bg_enabled; }

void background_set_opacity(uint8_t opa)
{
    bg_opa = opa;
    if (bg_img && bg_loaded)
        lv_obj_set_style_image_opa(bg_img, bg_opa, LV_PART_MAIN);
}
