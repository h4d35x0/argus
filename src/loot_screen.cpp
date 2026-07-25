#include "loot_screen.h"
#include "theme.h"
#include "offense_wipe.h"
#include "usb_sd.h"
#include "usb_sd_screen.h"
#include "argus_mode.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *loot_screen;
static lv_obj_t *loot_title;
static lv_obj_t *loot_summary;   // grand-total line under the title
static lv_obj_t *loot_list;      // scrollable per-dir sections + file rows
static lv_obj_t *wipe_all_btn;   // dimmed when there's no loot / card busy

// Confirm modal (reused for "wipe this dir" and "wipe all loot"). The pending
// target is a dir index into offense_loot_dirs(), or -1 for "all".
static lv_obj_t *confirm_overlay;
static lv_obj_t *confirm_text;
static int       confirm_target = -2;   // -2 = none pending, -1 = all, >=0 = dir idx

static void loot_show_confirm(int target);   // fwd (used by the per-dir wipe link)

// Human caption for a canonical loot dir (display-only; falls back to the raw
// path so a dir added to offense_loot_dirs() still renders). Kept separate from
// the canonical set so cosmetics can't widen the wipe scope.
static const char *caption_for(const char *path)
{
    if (!strcmp(path, "/pwn"))         return "Handshakes  (.pcap)";
    if (!strcmp(path, "/Wardrive"))    return "Wardrive  (.csv)";
    if (!strcmp(path, "/PingSweeps"))  return "Ping sweeps  (.txt)";
    if (!strcmp(path, "/Screenshots")) return "Screenshots";
    return path;
}

static void fmt_size(uint64_t bytes, char *out, size_t n)
{
    if      (bytes < 1024ULL)               snprintf(out, n, "%u B",    (unsigned)bytes);
    else if (bytes < 1024ULL * 1024)        snprintf(out, n, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024) snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else                                    snprintf(out, n, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

// A per-directory header row: caption + "N files - SIZE" total + a red Wipe link
// carrying the dir index in user_data.
static void add_dir_header(const char *caption, int file_count,
                           uint64_t total_bytes, int dir_index)
{
    lv_obj_t *row = lv_obj_create(loot_list);
    lv_obj_set_size(row, 388, 34);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_make(0x1A, 0x14, 0x08), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(row);
    lv_obj_set_style_text_font(cap, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 0, 0);

    char tot[40]; char sz[24];
    fmt_size(total_bytes, sz, sizeof(sz));
    if (file_count > 0) snprintf(tot, sizeof(tot), "%d - %s", file_count, sz);
    else                snprintf(tot, sizeof(tot), "empty");
    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(t, tot);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, -56, 0);

    if (file_count > 0) {
        lv_obj_t *w = lv_label_create(row);
        lv_obj_set_style_text_font(w, &font_argus_label_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(w, HADES_RED, LV_PART_MAIN);
        lv_label_set_text(w, "Wipe");
        lv_obj_align(w, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(w, 12);
        lv_obj_set_user_data(w, (void *)(intptr_t)dir_index);
        lv_obj_add_event_cb(w, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
            loot_show_confirm(idx);
        }, LV_EVENT_CLICKED, NULL);
    }
}

static void add_file_row(const char *name, uint64_t bytes)
{
    lv_obj_t *row = lv_obj_create(loot_list);
    lv_obj_set_size(row, 388, 26);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nlab = lv_label_create(row);
    lv_obj_set_style_text_font(nlab, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nlab, ARGUS_TEXT, LV_PART_MAIN);
    lv_label_set_long_mode(nlab, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nlab, 280);
    lv_label_set_text(nlab, name);
    lv_obj_align(nlab, LV_ALIGN_LEFT_MID, 8, 0);

    char sz[24];
    fmt_size(bytes, sz, sizeof(sz));
    lv_obj_t *s = lv_label_create(row);
    lv_obj_set_style_text_font(s, &font_argus_label_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(s, sz);
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, -4, 0);
}

// List one directory's files (adds file rows). Same openNextFile + strrchr('/')
// basename idiom as background.cpp / offense_wipe.
static void list_dir_files(const char *dirpath)
{
    File dir = SD.open(dirpath);
    if (!dir) return;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        if (!e.isDirectory()) {
            const char *nm    = e.name();
            const char *slash = strrchr(nm, '/');
            const char *base  = slash ? slash + 1 : nm;
            if (base[0] != '.') add_file_row(base, e.size());
        }
        e.close();
    }
    dir.close();
}

// Size-only pass: {count, total bytes} for a directory.
static void dir_totals(const char *dirpath, int *out_count, uint64_t *out_bytes)
{
    int count = 0; uint64_t total = 0;
    File dir = SD.open(dirpath);
    if (dir) {
        for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
            if (!e.isDirectory()) {
                const char *nm    = e.name();
                const char *slash = strrchr(nm, '/');
                const char *base  = slash ? slash + 1 : nm;
                if (base[0] != '.') { total += e.size(); count++; }
            }
            e.close();
        }
        dir.close();
    }
    *out_count = count; *out_bytes = total;
}

static void rebuild()
{
    if (!loot_list) return;
    lv_obj_clean(loot_list);

    bool card = instance.isCardReady();
    bool busy = usb_sd_is_running();

    if (busy) {
        lv_label_set_text(loot_summary, "Card mounted over USB");
        lv_obj_t *l = lv_label_create(loot_list);
        lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_width(l, 360);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, "The SD card is exposed to a computer over USB.\n"
                             "Unmount it from USB SD to manage loot here.");
        if (wipe_all_btn) lv_obj_add_state(wipe_all_btn, LV_STATE_DISABLED);
        return;
    }
    if (!card) {
        lv_label_set_text(loot_summary, "No SD card");
        lv_obj_t *l = lv_label_create(loot_list);
        lv_obj_set_style_text_font(l, &font_argus_label_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, ARGUS_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(l, "No SD card inserted.");
        if (wipe_all_btn) lv_obj_add_state(wipe_all_btn, LV_STATE_DISABLED);
        return;
    }

    size_t ndirs = 0;
    const char *const *dirs = offense_loot_dirs(&ndirs);
    uint64_t grand = 0; int grand_files = 0;

    for (size_t i = 0; i < ndirs; i++) {
        int cnt; uint64_t bytes;
        dir_totals(dirs[i], &cnt, &bytes);
        add_dir_header(caption_for(dirs[i]), cnt, bytes, (int)i);
        list_dir_files(dirs[i]);
        grand += bytes; grand_files += cnt;
    }

    char sum[48]; char sz[24];
    fmt_size(grand, sz, sizeof(sz));
    snprintf(sum, sizeof(sum), "%d files - %s total", grand_files, sz);
    lv_label_set_text(loot_summary, sum);

    if (wipe_all_btn) {
        if (grand_files > 0) lv_obj_clear_state(wipe_all_btn, LV_STATE_DISABLED);
        else                 lv_obj_add_state(wipe_all_btn, LV_STATE_DISABLED);
    }
}

// ---- confirm modal ---------------------------------------------------------

static void loot_show_confirm(int target)
{
    if (!confirm_overlay) return;
    confirm_target = target;
    char buf[176];
    if (target < 0) {
        snprintf(buf, sizeof(buf),
                 "Wipe ALL loot?\n\n/pwn, /Wardrive, /PingSweeps and\n"
                 "/Screenshots are overwritten and\ndeleted. This cannot be undone.");
    } else {
        size_t n = 0;
        const char *const *dirs = offense_loot_dirs(&n);
        const char *path = ((size_t)target < n) ? dirs[target] : "?";
        snprintf(buf, sizeof(buf),
                 "Wipe %s?\n\nEvery file is overwritten then\ndeleted. This cannot be undone.",
                 path);
    }
    lv_label_set_text(confirm_text, buf);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(confirm_overlay);
}

static void hide_confirm()
{
    if (confirm_overlay) lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    confirm_target = -2;
}

static void on_confirm_cancel(lv_event_t *) { hide_confirm(); }

static void on_confirm_wipe(lv_event_t *)
{
    // Both paths reuse the SAME Tier-1 shred as the duress wipe; the SD/USB guard
    // lives inside offense_wipe, so a failed guard is a no-op here.
    if (confirm_target < 0) offense_wipe_loot_all();
    else {
        size_t n = 0;
        const char *const *dirs = offense_loot_dirs(&n);
        if ((size_t)confirm_target < n) offense_wipe_dir(dirs[confirm_target]);
    }
    hide_confirm();
    rebuild();
}

static void on_confirm_backdrop(lv_event_t *e)
{
    if ((lv_obj_t *)lv_event_get_target(e) == confirm_overlay) hide_confirm();
}

// ---- navigation ------------------------------------------------------------

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) tools_screen_show();
}

// ---- create / show ---------------------------------------------------------

void loot_screen_create()
{
    loot_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(loot_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(loot_screen, 0, LV_PART_MAIN);

    loot_title = lv_label_create(loot_screen);
    lv_obj_set_style_text_color(loot_title, ARGUS_OFFENSE_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(loot_title, &font_argus_label_28, LV_PART_MAIN);   // Orbitron
    lv_label_set_text(loot_title, "LOOT");
    lv_obj_align(loot_title, LV_ALIGN_TOP_MID, 0, 12);

    loot_summary = lv_label_create(loot_screen);
    lv_obj_set_style_text_font(loot_summary, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(loot_summary, ARGUS_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(loot_summary, "");
    lv_obj_align(loot_summary, LV_ALIGN_TOP_MID, 0, 66);

    loot_list = lv_obj_create(loot_screen);
    lv_obj_set_size(loot_list, 404, 330);
    lv_obj_align(loot_list, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_color(loot_list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(loot_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(loot_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(loot_list, 4, LV_PART_MAIN);
    lv_obj_set_layout(loot_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(loot_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(loot_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(loot_list, LV_SCROLLBAR_MODE_AUTO);

    // OFFLOAD -> hand off to the USB-SD mass-storage screen (a host pulls the card
    // there; this screen never moves bytes itself).
    lv_obj_t *offload_btn = lv_button_create(loot_screen);
    lv_obj_set_size(offload_btn, 190, 56);
    lv_obj_align(offload_btn, LV_ALIGN_BOTTOM_LEFT, 6, -12);
    lv_obj_set_style_radius(offload_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(offload_btn, lv_color_make(0x00, 0xAA, 0x44), LV_PART_MAIN);
    lv_obj_add_event_cb(offload_btn, [](lv_event_t *) { usb_sd_screen_show(); },
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(offload_btn);
    lv_label_set_text(ol, "OFFLOAD");
    lv_obj_set_style_text_color(ol, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(ol, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(ol);

    wipe_all_btn = lv_button_create(loot_screen);
    lv_obj_set_size(wipe_all_btn, 190, 56);
    lv_obj_align(wipe_all_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -12);
    lv_obj_set_style_radius(wipe_all_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wipe_all_btn, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wipe_all_btn, lv_color_make(0x3A, 0x3A, 0x3C), LV_STATE_DISABLED);
    lv_obj_add_event_cb(wipe_all_btn, [](lv_event_t *) { loot_show_confirm(-1); },
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(wipe_all_btn);
    lv_label_set_text(wl, "WIPE ALL");
    lv_obj_set_style_text_color(wl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(wl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(wl);

    // Confirm modal (Cancel / Wipe), hidden until loot_show_confirm() pops it.
    confirm_overlay = lv_obj_create(loot_screen);
    lv_obj_set_size(confirm_overlay, 410, 502);
    lv_obj_align(confirm_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(confirm_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(confirm_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(confirm_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(confirm_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(confirm_overlay, on_confirm_backdrop, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(confirm_overlay);
    lv_obj_set_size(panel, 350, 270);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x1C, 0x1C, 0x1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 16, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, HADES_RED, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 16, LV_PART_MAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   // swallow backdrop taps

    confirm_text = lv_label_create(panel);
    lv_obj_set_width(confirm_text, lv_pct(100));
    lv_obj_set_style_text_color(confirm_text, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(confirm_text, &font_argus_label_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(confirm_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(confirm_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(confirm_text, "Wipe loot?");
    lv_obj_align(confirm_text, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cancel_btn = lv_button_create(panel);
    lv_obj_set_size(cancel_btn, 145, 56);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_make(0x3A, 0x3A, 0x3C), LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, on_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cl, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(cl);

    lv_obj_t *wipe_btn = lv_button_create(panel);
    lv_obj_set_size(wipe_btn, 145, 56);
    lv_obj_align(wipe_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(wipe_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wipe_btn, HADES_RED, LV_PART_MAIN);
    lv_obj_add_event_cb(wipe_btn, on_confirm_wipe, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wb = lv_label_create(wipe_btn);
    lv_label_set_text(wb, "Wipe");
    lv_obj_set_style_text_color(wb, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(wb, &font_argus_label_20, LV_PART_MAIN);
    lv_obj_center(wb);

    lv_obj_add_event_cb(loot_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void loot_screen_show()
{
    // This screen only makes sense in Offense. If somehow reached outside it,
    // bounce back to Tools rather than expose the list.
    if (argus_mode_current() != ArgusMode::Offense) { tools_screen_show(); return; }
    hide_confirm();
    rebuild();
    lv_scr_load(loot_screen);
}

bool loot_screen_is_active() { return lv_screen_active() == loot_screen; }
