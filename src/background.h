#pragma once
#include <lvgl.h>

// Full-screen SD-card wallpaper that renders BEHIND the clock UI (and
// behind the matrix rain). Call background_create() FIRST when building the
// clock screen so it sits at the lowest z-order; matrix_bg_create() and the
// clock widgets are then stacked on top of it.
//
// One wallpaper per mode is loaded from a FIXED path on the SD card, chosen by
// the current ArgusMode:
//   Daily   -> /backgrounds/daily.rgb565
//   Defense -> /backgrounds/defense.rgb565
//   Offense -> /backgrounds/offense.rgb565
// There is deliberately NO directory scan and NO png/bmp/jpg fallback: every
// image DECODER is unfit on this board (see the comment block at the top of
// background.cpp). Each file is raw, panel-sized, little-endian RGB565, exactly
// WP_W * WP_H * 2 bytes, read once into PSRAM and blitted from there. Generate
// them with tools/gen_wallpapers.py; sdcard/backgrounds/ ships ready to copy.
//
// It is drawn center-cropped to fill the panel at a LOW opacity so it reads
// as a faint wallpaper rather than a glaring image.
//
// Graceful fallback: a missing mode file falls back to Daily; no SD card, no
// /backgrounds directory, or no Daily file -> the object stays hidden and the
// screen stays black/matrix. It never crashes and never blocks.

// Create the (hidden) wallpaper image as the first child of `parent`. Also
// creates the /backgrounds directory on the SD card if it is absent, so the
// drop-in folder is discoverable instead of something the user has to know to
// create.
lv_obj_t *background_create(lv_obj_t *parent);

// Enable/disable the wallpaper. On first enable it loads the current mode's
// raster from the SD card; a graceful no-op (stays hidden) if the file is
// missing or no card is present. Coexists with the matrix rain: when both are
// on, the faint image sits behind and the rain renders on top.
void background_set_enabled(bool en);
bool background_is_enabled();

// Wallpaper opacity, 0 (invisible) .. 255 (opaque). Default 75 (faint).
void background_set_opacity(uint8_t opa);
