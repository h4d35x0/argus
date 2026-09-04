/* Toolchain proof only: LVGL 9.5 + SDL at the T-Watch Ultra's 410x502, one
 * label, render 30 frames headless-ish and exit 0. Prove the plumbing before
 * blaming any shim - if this does not build and run, nothing downstream will.
 */
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include <stdio.h>

int main(void)
{
    lv_init();
    lv_display_t *disp = lv_sdl_window_create(410, 502);
    if (!disp) { printf("SMOKE FAIL: no display\n"); return 1; }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070A), LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "ARGUS SIM OK");
    lv_obj_set_style_text_color(l, lv_color_hex(0x9BBCD6), LV_PART_MAIN);
    lv_obj_center(l);

    for (int i = 0; i < 30; i++) {
        lv_timer_handler();
        lv_tick_inc(33);
    }
    printf("SMOKE OK: lvgl %d.%d.%d, sdl display %dx%d\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
           (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));
    return 0;
}
