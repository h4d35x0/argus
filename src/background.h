#pragma once
#include <lvgl.h>

// Full-screen SD-card wallpaper that renders BEHIND the clock UI (and
// behind the matrix rain). Call background_create() FIRST when building the
// clock screen so it sits at the lowest z-order; matrix_bg_create() and the
// clock widgets are then stacked on top of it.
//
// The image is loaded from /backgrounds/ on the SD card, in this order:
//   1) /backgrounds/wallpaper.{png,bmp,jpg,jpeg}  (predictable default name)
//   2) the first *.png / *.bmp / *.jpg / *.jpeg found in that directory
// It is drawn center-cropped to fill the panel at a LOW opacity so it reads
// as a faint wallpaper rather than a glaring image. It reuses the exact
// LVGL-image-from-SD path map_screen uses for its tiles ("A:/..." source).
//
// Graceful fallback: no SD card, no /backgrounds directory, or no usable
// image -> the object stays hidden and the screen stays black/matrix. It
// never crashes and never blocks.

// Create the (hidden) wallpaper image as the first child of `parent`. Also
// ensures the /backgrounds directory exists on the SD card (with a short
// README explaining how to add a wallpaper) so the drop-in folder is
// discoverable instead of something the user has to know to create.
lv_obj_t *background_create(lv_obj_t *parent);

// Enable/disable the wallpaper. On first enable it scans the SD card for an
// image; a graceful no-op (stays hidden) if none is found or no card is
// present. Coexists with the matrix rain: when both are on, the faint image
// sits behind and the rain renders on top.
void background_set_enabled(bool en);
bool background_is_enabled();

// Wallpaper opacity, 0 (invisible) .. 255 (opaque). Default 75 (faint).
void background_set_opacity(uint8_t opa);
