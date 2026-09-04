// ARGUS simulator harness. SIM ONLY.
//
// Opens an SDL display at the T-Watch Ultra's exact panel size, builds the REAL
// screens from ../src, and either shows them live or dumps one PPM per frame
// for ffmpeg. Frames come from lv_snapshot_take(), i.e. straight out of LVGL's
// renderer, so a captured frame is what the firmware's own draw code produced.
//
// Time is driven by the harness (lv_tick_inc of a fixed step), not by the wall
// clock, so a capture is deterministic and frame N always shows the same thing.
// That is the difference between a repeatable asset pipeline and a screen
// recording.
//
//   argus_sim.exe --live              interactive window
//   argus_sim.exe --frames <dir>      dump the storyboard as PPM frames
//   argus_sim.exe --frames <dir> --fps 60
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include "Arduino.h"
#include "LilyGoLib.h"

#include "time_screen.h"
#include "world_clock_screen.h"
#include "calendar_screen.h"
#include "stopwatch_screen.h"
#include "timer_screen.h"
#include "tools_screen.h"
#include "threat_radar_screen.h"
#include "theme.h"
#include "argus_mode.h"

// Must precede SDL.h: we supply our own main(), not SDL's WinMain shim.
// Defined here rather than in the Makefile's DEFS because lv_sdl_window.c
// defines it too, and a command-line copy just warns about redefinition.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void sim_set_threat_level(int lvl);
void sim_set_utc_offset(int hours);

#define PANEL_W 410
#define PANEL_H 502

// ---------------------------------------------------------------------------
// Frame capture
// ---------------------------------------------------------------------------
// PPM rather than PNG on purpose: no image library, no version skew, and
// ffmpeg reads a numbered PPM sequence natively. The bytes are RGB888 taken
// from the snapshot buffer.
static bool dump_ppm(const char *path)
{
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (!snap) { fprintf(stderr, "snapshot failed\n"); return false; }

    FILE *f = fopen(path, "wb");
    if (!f) { lv_draw_buf_destroy(snap); return false; }

    const uint32_t w = snap->header.w, h = snap->header.h;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = snap->data + (size_t)y * snap->header.stride;
        for (uint32_t x = 0; x < w; x++) {
            // LVGL's RGB888 buffer is byte order B,G,R.
            const uint8_t *px = row + x * 3;
            fputc(px[2], f); fputc(px[1], f); fputc(px[0], f);
        }
    }
    fclose(f);
    lv_draw_buf_destroy(snap);
    return true;
}

// ---------------------------------------------------------------------------
// Storyboard
// ---------------------------------------------------------------------------
enum ScreenId { SCR_TIME, SCR_WORLD, SCR_CAL, SCR_STOPWATCH, SCR_TIMER,
                SCR_TOOLS, SCR_RADAR };

struct Scene {
    ScreenId    screen;
    const char *label;
    int         seconds;
    int         threat;      // threatradar_top_level() during this scene
    ArgusMode   mode;
};

// Only screens the simulator actually builds. Every one is real firmware
// source; nothing here is a mockup.
//
// The TOOLS grid carries the reel. tools_apply_mode() genuinely gates the 27
// tiles per mode - Daily hides ALL of them (the innocent-watch disguise),
// Defense shows the 19 non-offensive, Offense shows ONLY the offensive ones -
// so cutting between the three modes here shows a real, visible difference,
// unlike the TIME screen where mode and threat level are deliberately
// invisible (byte-identical frames, verified by image diff: headings use
// argus_base_accent(), and red means Offense only).
static const Scene STORY[] = {
    { SCR_TOOLS,     "tools-daily",   4, 0, ArgusMode::Daily   },
    { SCR_TOOLS,     "tools-defense", 6, 0, ArgusMode::Defense },
    { SCR_RADAR,     "radar",         5, 0, ArgusMode::Defense },
    { SCR_TOOLS,     "tools-offense", 5, 0, ArgusMode::Offense },
    { SCR_WORLD,     "worldclock",    5, 0, ArgusMode::Defense },
    { SCR_CAL,       "calendar",      3, 0, ArgusMode::Defense },
    { SCR_STOPWATCH, "stopwatch",     3, 0, ArgusMode::Defense },
    { SCR_TIMER,     "timer",         3, 0, ArgusMode::Defense },
    { SCR_TIME,      "time",          4, 0, ArgusMode::Defense },
};

static void show(ScreenId s)
{
    switch (s) {
    case SCR_TIME:      time_screen_show();        break;
    case SCR_WORLD:     world_clock_screen_show(); break;
    case SCR_CAL:       calendar_screen_show();    break;
    case SCR_STOPWATCH: stopwatch_screen_show();   break;
    case SCR_TIMER:     timer_screen_show();       break;
    case SCR_TOOLS:     tools_screen_show();       break;
    case SCR_RADAR:     threat_radar_screen_show();break;
    }
}

int main(int argc, char **argv)
{
    bool        live = false;
    const char *outdir = NULL;
    int         fps = 30;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--live"))                    live = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)  outdir = argv[++i];
        else if (!strcmp(argv[i], "--fps")    && i + 1 < argc)  fps = atoi(argv[++i]);
    }
    if (!live && !outdir) { fprintf(stderr, "need --live or --frames <dir>\n"); return 2; }
    if (fps < 1 || fps > 240) { fprintf(stderr, "bad --fps\n"); return 2; }

    // MUST run before any screen code. world_clock_screen.cpp's zone_hm()
    // normalises a shifted wall clock with mktime(), and its comment states the
    // assumption that lets that be safe: "system TZ is UTC-neutral on this
    // build". That is true on the ESP32 and FALSE on a laptop in a
    // DST-observing zone, where mktime() reads the struct as local time, sets
    // tm_isdst itself and moves the hour.
    //
    // Measured before this fix: every zone row was exactly +1 h (UTC read 02:24
    // for an 01:24 UTC clock, and Honolulu - which has no DST at all - read
    // 16:24 instead of 15:24), while the LOCAL row was correct because it goes
    // through clocktime:: instead. So it was the host environment, not the DST
    // table: the offset LABELS were right the whole time.
    //
    // Pinning TZ=UTC makes the host match the environment the firmware
    // documents, rather than patching firmware to suit the simulator.
    putenv((char *)"TZ=UTC");
    tzset();

    lv_init();
    lv_display_t *disp = lv_sdl_window_create(PANEL_W, PANEL_H);
    if (!disp) { fprintf(stderr, "no SDL display\n"); return 1; }
    lv_sdl_window_set_title(disp, "ARGUS simulator");

    // Scene defaults. These are stated, not measured - see LilyGoLib.h.
    sim_set_clock(2026, 9, 4, 1, 24, 0);   // 21:24 EDT on 2026-09-03
    sim_set_battery(82, false, false);
    sim_set_utc_offset(-4);

    argus_mode_init();
    time_screen_create();
    world_clock_screen_create();
    calendar_screen_create();
    stopwatch_screen_create();
    timer_screen_create();
    tools_screen_create();
    threat_radar_screen_create();

    const int step_ms = 1000 / fps;

    if (live) {
        show(STORY[0].screen);
        printf("live window open; close it to exit\n");
        for (;;) {
            lv_timer_handler();
            lv_tick_inc(step_ms);
            SDL_Delay(step_ms);
            SDL_Event e;
            while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) return 0;
        }
    }

    int n = 0;
    char path[1024];
    for (size_t s = 0; s < sizeof(STORY) / sizeof(STORY[0]); s++) {
        const Scene &sc = STORY[s];
        // Mode switching is NOT symmetric, and getting this wrong produced a
        // wrong capture once already.
        //
        // argus_mode_set() REJECTS Offense by design (Offense is PIN-gated and
        // never persisted), so entering it needs enter_offense(). Leaving it
        // needs lock_offense(): while inside Offense, argus_mode_set() only
        // updates the REMEMBERED Daily/Defense choice and deliberately does not
        // drop out of Offense. Without the lock_offense() below, the "Daily"
        // scene rendered the OFFENSE grid, and the Radar heading came out RED -
        // which reads as a live threat when it is really just the Offense
        // accent. Both were caught by image-diffing the frames.
        if (sc.mode == ArgusMode::Offense) {
            enter_offense();
        } else {
            if (argus_mode_current() == ArgusMode::Offense) lock_offense();
            argus_mode_set(sc.mode);
        }
        sim_set_threat_level(sc.threat);
        show(sc.screen);

        // Let layout settle before the first captured frame, so no scene opens
        // on a half-built tree.
        for (int i = 0; i < 3; i++) { lv_timer_handler(); lv_tick_inc(step_ms); }

        const int frames = sc.seconds * fps;
        for (int i = 0; i < frames; i++) {
            lv_timer_handler();
            lv_tick_inc(step_ms);
            snprintf(path, sizeof(path), "%s/f%05d.ppm", outdir, n++);
            if (!dump_ppm(path)) return 1;
        }
        fprintf(stderr, "  %-14s %3d frames (mode=%d threat=%d)\n",
                sc.label, frames, (int)sc.mode, sc.threat);
    }
    printf("%d frames -> %s\n", n, outdir);
    return 0;
}
