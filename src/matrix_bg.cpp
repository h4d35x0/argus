#include "matrix_bg.h"
#include <esp_system.h>   // esp_random
#include <stdio.h>

#define MX_COLS    22
#define MX_ROWS    26
#define MX_COL_W   18     // 22 * 18 = 396 px, centred on the 410 px panel
#define MX_TICK_MS 120    // opt-in effect; modest rate keeps the QSPI flush sane

static lv_obj_t  *mx_cont = nullptr;
static lv_obj_t  *mx_col[MX_COLS];
static lv_timer_t *mx_timer = nullptr;
static bool       mx_enabled = false;

static int  head[MX_COLS];          // current head row; negative = still entering
static int  tlen[MX_COLS];          // trail length
static char cell[MX_COLS][MX_ROWS]; // stable glyphs so the trail doesn't fully reshuffle
// Easter-egg state per column. 0 = no egg (regular rain). >0 = chars
// [0..egg_len-1] of cell[c][] hold a fixed string; mx_tick skips its
// head-randomize and trail-flicker for those cells so the word stays
// readable as the bright head passes through it.
static int  egg_len[MX_COLS];

// No '#' — it is the LVGL recolor escape character and would corrupt parsing.
static const char MX_CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@$%&*+=<>?";
static const int MX_CHARSET_LEN = sizeof(MX_CHARSET) - 1;

// Easter-egg strings. All chars must live in MX_CHARSET (no '#' or
// lowercase). Roughly 1-in-30 column resets seeds the column's top
// cells with one of these.
static const char *MX_EGGS[] = {
    "ARGUS",
    "WATCH",
    "HADES",
    "REDTEAM",        // was the 1337 homage; swapped for ARGUS voice (differentiate from 13-37)
    "HACKTHEPLANET",
};
static const int MX_EGG_COUNT = sizeof(MX_EGGS) / sizeof(MX_EGGS[0]);

static char rnd_char() { return MX_CHARSET[esp_random() % MX_CHARSET_LEN]; }

static void col_reset(int c)
{
    tlen[c] = 6 + (int)(esp_random() % 13);          // 6..18
    head[c] = -(int)(esp_random() % MX_ROWS);        // stagger entry from above
    for (int r = 0; r < MX_ROWS; r++)
        cell[c][r] = MX_CHARSET[esp_random() % MX_CHARSET_LEN];
    egg_len[c] = 0;
    // ~1 in 30 reset chances, repaint the whole column with one of the
    // easter-egg strings tiled top-to-bottom. egg_len = MX_ROWS so
    // mx_tick's randomize + flicker skip every cell, leaving the
    // repeated text frozen while the head's bright gradient slides
    // down through it. Bump tlen to MX_ROWS so the trail covers the
    // full column when the head reaches the bottom.
    if ((esp_random() % 30) == 0) {
        const char *s = MX_EGGS[esp_random() % MX_EGG_COUNT];
        int slen = 0;
        while (s[slen]) slen++;
        if (slen > 0) {
            for (int r = 0; r < MX_ROWS; r++)
                cell[c][r] = s[r % slen];
            egg_len[c] = MX_ROWS;
            tlen[c]    = MX_ROWS;
        }
    }
}

// Distance 0 = bright head, increasing distance = dimmer trail.
static const char *shade(int dist, int len)
{
    if (dist == 0)        return "CCFFCC";
    if (dist <= len / 4)  return "5BFF8C";
    if (dist <= len / 2)  return "22BB44";
    return "0E6622";
}

static void render_col(int c)
{
    char buf[MX_ROWS * 12];
    int  n = 0;
    for (int r = 0; r < MX_ROWS; r++) {
        int dist = head[c] - r;   // 0 at head, grows up the trail
        if (head[c] >= 0 && r <= head[c] && dist < tlen[c]) {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "#%s %c#\n", shade(dist, tlen[c]), cell[c][r]);
        } else {
            buf[n++] = '\n';      // empty row keeps vertical alignment
        }
    }
    if (n > 0 && buf[n - 1] == '\n') n--;   // trim trailing newline
    buf[n] = '\0';
    lv_label_set_text(mx_col[c], buf);
}

static void mx_tick(lv_timer_t *)
{
    for (int c = 0; c < MX_COLS; c++) {
        head[c]++;
        int el = egg_len[c];
        // Skip randomization for cells that hold easter-egg chars
        // (rows [0..el-1]); randomize freely outside that range.
        if (head[c] >= el && head[c] < MX_ROWS)
            cell[c][head[c]] = rnd_char();          // fresh glyph at the head
        if ((esp_random() & 0x0F) == 0) {           // occasional trail flicker
            int r = (int)(esp_random() % MX_ROWS);
            if (r >= el) cell[c][r] = rnd_char();
        }
        if (head[c] - tlen[c] > MX_ROWS)
            col_reset(c);
        render_col(c);
    }
}

lv_obj_t *matrix_bg_create(lv_obj_t *parent)
{
    mx_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(mx_cont);
    lv_obj_set_size(mx_cont, MX_COL_W * MX_COLS, 502);
    lv_obj_align(mx_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(mx_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_CLICKABLE);

    for (int c = 0; c < MX_COLS; c++) {
        lv_obj_t *l = lv_label_create(mx_cont);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(l, MX_COL_W);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        // Non-recoloured glyphs (blank rows) render invisibly on the black screen
        lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(l, c * MX_COL_W, 0);
        mx_col[c] = l;
        col_reset(c);
    }

    lv_obj_add_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
    return mx_cont;
}

void matrix_bg_set_enabled(bool en)
{
    mx_enabled = en;
    if (!mx_cont) return;
    if (en) {
        lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
        if (!mx_timer) mx_timer = lv_timer_create(mx_tick, MX_TICK_MS, nullptr);
    } else {
        lv_obj_add_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
        if (mx_timer) { lv_timer_del(mx_timer); mx_timer = nullptr; }
    }
}

bool matrix_bg_is_enabled() { return mx_enabled; }

void matrix_bg_set_paused(bool paused)
{
    if (!mx_timer) return;
    if (paused) lv_timer_pause(mx_timer);
    else        lv_timer_resume(mx_timer);
}
