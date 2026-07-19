#include "background.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// LVGL image object for the wallpaper plus a persistent path buffer LVGL
// keeps referencing as the image source (must outlive the set_src call).
static lv_obj_t *bg_img     = nullptr;
static bool      bg_enabled = false;
static uint8_t   bg_opa     = 75;      // faint by design (0..255)
static char      bg_lv_path[96];       // "A:/backgrounds/<name>" for LVGL
static bool      bg_loaded  = false;   // have we already found + set a source?

static const char *kBgDir = "/backgrounds";

// Case-insensitive "does `name` end with `ext`?"
static bool ends_with_ci(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (le > ln) return false;
    const char *p = name + (ln - le);
    for (size_t i = 0; i < le; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)ext[i]))
            return false;
    return true;
}

static bool is_supported_image(const char *name)
{
    // PNG (LODEPNG), BMP and JPEG (TJPGD) decoders are all enabled in
    // lv_conf.h, so any of these will decode from the SD card.
    return ends_with_ci(name, ".png") ||
           ends_with_ci(name, ".bmp") ||
           ends_with_ci(name, ".jpg") ||
           ends_with_ci(name, ".jpeg");
}

// File part of a path that may be absolute ("/backgrounds/x.png") or bare
// ("x.png"); the ESP32 SD core differs on which it returns from name().
static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Locate a usable wallpaper on the SD card. Fills bg_lv_path with the LVGL
// source string ("A:/backgrounds/<file>") and returns true on success.
static bool find_wallpaper()
{
    if (!instance.isCardReady()) return false;
    if (!SD.exists(kBgDir))      return false;

    // 1) Predictable default names first, so a user who drops exactly
    //    /backgrounds/wallpaper.png gets deterministic behaviour.
    static const char *kPreferred[] = {
        "/backgrounds/wallpaper.png",
        "/backgrounds/wallpaper.bmp",
        "/backgrounds/wallpaper.jpg",
        "/backgrounds/wallpaper.jpeg",
    };
    for (size_t i = 0; i < sizeof(kPreferred) / sizeof(kPreferred[0]); i++) {
        if (SD.exists(kPreferred[i])) {
            snprintf(bg_lv_path, sizeof(bg_lv_path), "A:%s", kPreferred[i]);
            return true;
        }
    }

    // 2) Otherwise take the first supported image in the directory.
    File dir = SD.open(kBgDir);
    if (!dir) return false;
    bool found = false;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        if (!e.isDirectory()) {
            const char *base = basename_of(e.name());
            if (base[0] != '.' && is_supported_image(base)) {
                snprintf(bg_lv_path, sizeof(bg_lv_path),
                         "A:%s/%s", kBgDir, base);
                found = true;
            }
        }
        e.close();
        if (found) break;
    }
    dir.close();
    return found;
}

// Load the located image into the LVGL object, sizing it to cover the panel
// at the configured opacity. Returns false if no image was found.
static bool load_source()
{
    if (!bg_img) return false;
    if (!find_wallpaper()) return false;

    int32_t w = lv_display_get_horizontal_resolution(NULL);
    int32_t h = lv_display_get_vertical_resolution(NULL);
    lv_obj_set_size(bg_img, w, h);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);

    // Force a reload even if the path pointer is unchanged (mirrors the
    // map_screen tile pattern).
    lv_image_set_src(bg_img, NULL);
    lv_image_set_src(bg_img, bg_lv_path);
    // COVER: scale to fill the widget, preserving aspect ratio, and
    // center-crop the overflow. Faint by design via image_opa.
    lv_image_set_inner_align(bg_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_set_style_image_opa(bg_img, bg_opa, LV_PART_MAIN);
    bg_loaded = true;
    return true;
}

lv_obj_t *background_create(lv_obj_t *parent)
{
    bg_img = lv_image_create(parent);
    lv_obj_remove_style_all(bg_img);
    lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_SCROLLABLE);
    // Lowest z-order on the parent so the clock + matrix rain render on top.
    lv_obj_move_background(bg_img);
    return bg_img;
}

void background_set_enabled(bool en)
{
    bg_enabled = en;
    if (!bg_img) return;
    if (en) {
        // Lazily locate + load the SD image on first enable. If nothing is
        // found we simply stay hidden (black / matrix) — no crash, no error.
        if (!bg_loaded && !load_source()) {
            lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_move_background(bg_img);   // keep it behind matrix + clock
        lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
    } else {
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
