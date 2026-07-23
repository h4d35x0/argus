#include "background.h"
#include "image_dims.h"
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

// Largest source image we will hand to LVGL, in pixels. The panel is 410x502
// (~206k px); LVGL decodes the SOURCE image at full resolution into an ARGB
// buffer BEFORE the LV_IMAGE_ALIGN_COVER downscale, so a multi-megapixel phone
// photo (e.g. 4000x3000 = 12M px ~ 48MB ARGB) OOMs the watch. 1.2M px gives
// generous headroom for slightly-larger source art while still rejecting the
// multi-megapixel photos students may upload. Reject over-budget images by
// reading only the header, never decoding them.
// ~400k px (e.g. up to ~630x630, comfortably above the 410x502 panel). Two
// reasons: a huge source image OOMs the full-res decode, AND the decoded image
// must fit the 2 MB LVGL image cache (LV_CACHE_DEF_SIZE) or it gets re-decoded
// from SD every render (~2.9s -> clock skips seconds). 400k px ARGB ~= 1.6 MB,
// under the cache. Panel-sized art (like the bundled 410x502 backgrounds) is
// well within this.
static constexpr uint32_t kMaxWallpaperPixels = 400000u;

// SECOND, independent budget: the COMPRESSED file size. The PNG/JPEG decoders
// buffer the whole compressed image in INTERNAL SRAM while inflating it, and
// there is only ~100-130 KB free internal SRAM at boot. So a detailed image can
// pass the pixel budget above (same 410x502 raster) yet still OOM the decode and
// HARD-CRASH -> boot-loop. That is exactly what happened: ARGUS.png (85 KB
// compressed) decodes fine, but a 144 KB "Privacy" wallpaper of the SAME pixel
// dimensions boot-looped the watch. Reject any wallpaper whose file is over a
// safe fixed cap, or whose size alone would not fit in the internal SRAM free
// right now. A rejected wallpaper is simply not shown (graceful) - it can never
// crash the boot. Reads only the file size, never decodes.
static constexpr uint32_t kMaxWallpaperFileBytes = 100u * 1024u;

// Inspect the header of the located wallpaper and reject it if decoding it
// would blow the pixel budget. Reads only the first bytes of the file (never
// the whole image) via the pure image_dims probe, so this cannot itself OOM.
// Returns true if the image is safe to load OR if the header is unrecognized
// (in which case we defer to LVGL exactly as before, to avoid regressing
// currently-working small images). `lv_path` is the "A:/backgrounds/<file>"
// LVGL source string filled in by find_wallpaper().
static bool wallpaper_within_budget(const char *lv_path)
{
    // find_wallpaper() stores "A:<fs path>"; the SD API wants the path without
    // the "A:" LVGL drive letter.
    const char *fs_path = lv_path;
    if (fs_path[0] == 'A' && fs_path[1] == ':') fs_path += 2;

    File f = SD.open(fs_path);
    if (!f) return true;   // cannot inspect -> let LVGL try (no regression)

    uint8_t head[64];
    int rd = f.read(head, sizeof(head));
    f.close();
    size_t n = (rd > 0) ? (size_t)rd : 0;

    ImageDims dims;
    if (!image_probe_dims(head, n, &dims))
        return true;       // unknown/unrecognized header -> defer to LVGL

    if (image_dims_within_budget(dims, kMaxWallpaperPixels))
        return true;

    Serial.printf("[background] skipping oversized wallpaper %s (%lux%lu px > %lu px budget)\n",
                  fs_path, (unsigned long)dims.width, (unsigned long)dims.height,
                  (unsigned long)kMaxWallpaperPixels);
    return false;
}

// Reject a wallpaper whose COMPRESSED file is over a fixed safe cap. On this board
// a large wallpaper file (like a 147 KB photo) can boot-loop the decode, while the
// bundled 410x502 art (57-87 KB) is safe. Skipping an oversized wallpaper is
// graceful (simply not shown), never a crash. Reads only the file length, never
// decodes. IMPORTANT: this is deliberately a FIXED cap, NOT a free-RAM-relative
// check. An earlier version also skipped when file_bytes >= free internal SRAM,
// but free internal dips during boot and that wrongly rejected the known-good
// 87 KB wallpaper (it silently never rendered). The fixed cap is reliable.
static bool wallpaper_fits_memory(const char *lv_path)
{
    const char *fs_path = lv_path;
    if (fs_path[0] == 'A' && fs_path[1] == ':') fs_path += 2;

    File f = SD.open(fs_path);
    if (!f) return true;                         // cannot inspect -> defer
    uint32_t bytes = (uint32_t)f.size();
    f.close();

    if (bytes > kMaxWallpaperFileBytes) {
        Serial.printf("[background] skipping wallpaper %s: %lu B compressed "
                      "(cap %lu B) - too large, would risk a boot-loop\n",
                      fs_path, (unsigned long)bytes, (unsigned long)kMaxWallpaperFileBytes);
        return false;
    }
    return true;
}

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
    // Guard against an oversized source image OOMing the LVGL decode: inspect
    // the header BEFORE handing the path to LVGL. Over-budget -> stay hidden.
    // Two independent checks: pixel dimensions AND compressed file size (the
    // latter is what buffers in scarce internal SRAM during decode).
    if (!wallpaper_within_budget(bg_lv_path)) return false;
    if (!wallpaper_fits_memory(bg_lv_path))   return false;

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

// Make the /backgrounds drop-in folder discoverable: create it on the SD card
// if it is missing, and leave a short README so a student/kid knows exactly
// what to put there. The firmware only ever READS wallpapers, so without this
// the folder never appears and the feature looks broken ("there is no
// backgrounds folder"). Runs once at UI setup after the card is mounted; a
// missing card or an existing folder is a graceful no-op, never a crash.
static void ensure_backgrounds_dir()
{
    if (!instance.isCardReady()) return;
    if (SD.exists(kBgDir)) return;
    if (!SD.mkdir(kBgDir)) return;   // read-only / full card -> silently skip

    File readme = SD.open("/backgrounds/README.txt", FILE_WRITE);
    if (readme) {
        readme.print(
            "ARGUS - wallpaper drop folder\r\n"
            "\r\n"
            "Put an image here to use it as your watch background:\r\n"
            "  - Name it wallpaper.png (or .bmp / .jpg), or drop any image file.\r\n"
            "  - Keep it small: 410x502 and UNDER 100 KB is ideal.\r\n"
            "    Images that are too large in pixels OR in file size are\r\n"
            "    skipped automatically so they cannot crash the watch.\r\n"
            "  - It shows faintly behind the clock and the Matrix rain.\r\n"
            "\r\n"
            "Then turn Wallpaper ON in Settings.\r\n");
        readme.close();
    }
}

lv_obj_t *background_create(lv_obj_t *parent)
{
    ensure_backgrounds_dir();

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
