"""ARGUS - social demo reel builder.

Builds short demo videos from REAL device screenshots (img/argus/*.png, captured
by the on-device screenshot feature at the panel's native 410x502) and exports
one storyboard to three aspect ratios:

    vertical  1080x1920  9:16   TikTok / Reels / Shorts
    square    1080x1350  4:5    X / LinkedIn / Mastodon feed
    wide      1920x1080  16:9   README / YouTube  (+ a looping WebM and GIF)

WHY REAL CAPTURES AND NOT A MOCKUP. The repo is a public security project whose
flagship claim is still awaiting field validation, so the demo must not overstate
anything. Every pixel of the watch screen in these videos came off the device.
The only synthetic elements are the bezel, the background and the captions, and
nothing in the caption set asserts a capability that has not been verified - see
CLAIMS below.

CLAIMS POLICY (deliberate, do not loosen without a reason):
  - The detector scene claims exactly what the README claims after 28d00b0, and
    the two must not drift apart. What is field-proven is the LIKELY rung: on
    2026-07-30 a real Find My tracker co-moving on an outdoor drive reached
    Likely across 21 waypoints and about 7.5 km, and Likely is the threshold
    that fires the haptic, the badge and the red accent. What is NOT proven is
    CONFIRMED (>=4 waypoints AND >=1500 m AND >=18 min together), so the
    Confirmed-gated Meshtastic broadcast must never be shown working.
    An earlier version of this file said "field validation still pending" for
    the whole feature, which was inherited from the README and was itself wrong
    in the understating direction. Do not reintroduce it, and do not upgrade the
    caption to imply Confirmed either.
  - Red is reserved. On this watch red means a LIVE THREAT and gray means a
    hardware fault (see theme.h and the chrome-vs-alert accent rule), so the
    reel is built on the steel-blue brand accent and never decorates with red.

Fonts are the two in-repo SIL OFL faces (tools/brandfonts/), never a system or
commercial font - this repo has already had its history rewritten once to purge
commercial font blobs.

Usage:
    python3 tools/make_demo_reel.py                 # all three aspects
    python3 tools/make_demo_reel.py --aspect vertical
    python3 tools/make_demo_reel.py --fps 30 --out artifacts/demo
"""

import argparse
import math
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHOTS = os.path.join(ROOT, "img", "argus")
FONTS = os.path.join(HERE, "brandfonts")

ORBITRON = os.path.join(FONTS, "Orbitron.ttf")      # SIL OFL
VT323 = os.path.join(FONTS, "VT323-Regular.ttf")    # SIL OFL

# --- brand palette, lifted verbatim from src/theme.h ------------------------
ACCENT = (0x9B, 0xBC, 0xD6)         # ARGUS_ACCENT        steel blue
ACCENT_HI = (0xC8, 0xDE, 0xF0)      # ARGUS_ACCENT_ACTIVE
ACCENT_DK = (0x5B, 0x7C, 0x96)      # ARGUS_ACCENT_DIM
TEXT = (0xED, 0xE8, 0xDA)           # ARGUS_TEXT          cream
TEXT_DIM = (0xB2, 0xAB, 0x98)       # ARGUS_TEXT_DIM
BG0 = (0x05, 0x07, 0x0A)
BG1 = (0x0D, 0x12, 0x18)

PANEL_W, PANEL_H = 410, 502         # the T-Watch Ultra panel, 1:1 with the shots


# ---------------------------------------------------------------------------
# Storyboard
# ---------------------------------------------------------------------------
# (screenshot, headline, subline, seconds)
#
# Ordered as a story rather than as a feature list: the hook is the wallpaper
# because it reads in half a second on a muted autoplay, then what the thing is,
# then the capabilities that are actually verified, then the licence and the
# upstream credit.
# ASSET AUDIT, 2026-09-03. Every capture in img/argus/ was taken 2026-07-21,
# which is BEFORE the rebrand (23a7cd5), so three of the twenty are unusable and
# the rest are only usable because they carry no brand string:
#   config_1.png  - showed the old product name in a text field. REMOVED from
#                   the repo 2026-09-08 along with hexhound.png (old stage
#                   label); neither exists any more.
#   radar.png     - renders tofu boxes where LV_SYMBOL_* glyphs failed on a
#                   brand-font label (the ASCII-only subset font bug). Excluded
#                   on appearance grounds AND because the tail verdict is the
#                   one claim this reel must not make.
#   notify.png    - same tofu boxes. Excluded.
# Empty-state screens (meshtastic "No messages yet", stopwatch "No laps
# recorded") are excluded as weak demo material rather than as defects.
# REFRESH THIS SET off current firmware before publishing anything.
STORYBOARD = [
    ("clock.png",
     "PRIVACY IS AN ILLUSION",
     "So we built a watch that watches back.",
     3.2),
    ("clock.png",
     "ARGUS",
     "Open-source anti-surveillance firmware\nfor the LilyGo T-Watch Ultra.",
     3.0),
    ("tools_1.png",
     "THE DETECTOR SUITE",
     "Find My trackers, Flock cameras,\ncard skimmers, rogue access points.\nTail alerting field-tested over 7.5 km.",
     3.6),
    ("tools_2.png",
     "AND AN RF TOOLKIT",
     "Flipper hunting, WiFi survey,\nspectrum analyse, evil-twin spotting.",
     3.2),
    ("tools_4.png",
     "SEVENTEEN TOOLS",
     "HexHound, LoRa APRS, TPMS,\nBLE mouse, USB SD card reader.",
     3.2),
    ("tools_4.png",
     "HEXHOUND",
     "Sub-GHz and NFC tooling,\nwith a mascot that levels up.",
     3.0),
    ("settings_1.png",
     "TUNE EVERY LAST THING",
     "Brightness, analog or digital face,\n12 or 24 hour, dim timeout, haptics,\nwrist-raise wake.",
     3.4),
    ("calendar.png",
     "AND IT IS STILL A WATCH",
     "Calendar, alarms, timer, stopwatch,\nworld clock with real daylight saving.",
     3.2),
    (None,
     "BUILD IT YOURSELF",
     "MIT licensed. Built on r3dfish's\nopen-source T-Watch Ultra firmware.\n\ngithub.com/h4d35x0/argus",
     3.8),
]

# ---------------------------------------------------------------------------
# Simulator storyboard - LIVE screens captured from current source
# ---------------------------------------------------------------------------
# Frames come from sim/build/frames (see sim/Makefile: `make frames`), which
# renders the REAL screen source through LVGL's own renderer at 30 fps. Unlike
# the still storyboard above, these are current-source and they MOVE.
#
# (first_frame, count, headline, subline). Ranges must match sim STORY[] order.
SIM_STORYBOARD = [
    (0,    120, "DAILY MODE",
     "Looks like an ordinary watch.\nThe tools are hidden, not greyed out."),
    (120,  180, "DEFENSE MODE",
     "Find My trackers, Flock cameras,\nskimmers, spycams, rogue access points."),
    (300,  150, "THREAT RADAR",
     "Scores whether a device is co-moving\nwith you, not merely nearby.\n"
     "Likely rung field-tested over 7.5 km."),
    (450,  150, "OFFENSE MODE",
     "PIN-gated, hidden, never persisted.\nStrict separation: no detectors here."),
    (600,  150, "REAL DAYLIGHT SAVING",
     "Ten zones, each on its own DST rule.\nThe UTC label moves with the time."),
    (750,  270, "AND STILL A WATCH",
     "Calendar, stopwatch, timer."),
    (1020, 120, "BUILD IT YOURSELF",
     "MIT licensed. Built on r3dfish's\nopen-source T-Watch Ultra firmware.\n\n"
     "github.com/h4d35x0/argus"),
]
SIM_FPS = 30      # the rate sim/main_sim.cpp dumped at

ASPECTS = {
    #  name        W     H     screen_w  screen_y  layout
    "vertical": (1080, 1920, 780, 300, "stack"),
    "square":   (1080, 1350, 620, 190, "stack"),
    "wide":     (1920, 1080, 620, 200, "side"),
}


def font(path, size):
    return ImageFont.truetype(path, size)


def load_shot(name):
    """Load a real device capture at native resolution."""
    p = os.path.join(SHOTS, name)
    if not os.path.exists(p):
        raise SystemExit("missing screenshot: %s" % p)
    return Image.open(p).convert("RGB")


def make_background(w, h):
    """Static brand background: radial wash, vignette, faint scanlines.

    Built once per aspect and reused for every frame - the per-frame cost is
    then just one copy plus two pastes, which is what keeps a 900-frame render
    to a couple of minutes instead of half an hour.
    """
    bg = Image.new("RGB", (w, h), BG0)
    d = ImageDraw.Draw(bg)
    cx, cy = w // 2, int(h * 0.40)
    maxr = int(math.hypot(max(cx, w - cx), max(cy, h - cy)))
    for r in range(maxr, 0, -6):
        t = r / maxr
        col = tuple(int(BG1[k] * (1 - t) + BG0[k] * t) for k in range(3))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)

    # scanlines: 1px every 4px, very low alpha. Reads as CRT texture at
    # 1080p and disappears at thumbnail size, which is the intent.
    sl = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sl)
    for y in range(0, h, 4):
        sd.line([(0, y), (w, y)], fill=(0, 0, 0, 26))
    bg = Image.alpha_composite(bg.convert("RGBA"), sl).convert("RGB")

    # corner vignette
    vig = Image.new("L", (w, h), 0)
    vd = ImageDraw.Draw(vig)
    vd.ellipse([-int(w * 0.35), -int(h * 0.30),
                w + int(w * 0.35), h + int(h * 0.30)], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(min(w, h) // 12))
    bg = Image.composite(bg, Image.new("RGB", (w, h), BG0), vig)
    return bg


def build_bezel(screen_w):
    """Bezel + glow built ONCE, with the screen area left empty.

    Split out from make_device so an ANIMATED scene can paste a different
    captured frame into the same bezel 30 times a second without rebuilding the
    body, the rim light and the gaussian glow each time. That is the difference
    between a two-minute render and a twenty-minute one.

    Returns (base_rgba, paste_xy, mask, screen_size).
    """
    sw = screen_w
    sh = int(round(sw * PANEL_H / PANEL_W))
    pad = max(10, sw // 26)
    rad = max(18, sw // 11)
    W, H = sw + pad * 2, sh + pad * 2

    body = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=rad, fill=(14, 16, 20, 255))
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=rad,
                        outline=ACCENT_DK + (255,), width=max(2, sw // 300))

    glow = Image.new("RGBA", (W + pad * 6, H + pad * 6), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.rounded_rectangle([pad * 3, pad * 3, pad * 3 + W, pad * 3 + H],
                         radius=rad, fill=ACCENT + (60,))
    glow = glow.filter(ImageFilter.GaussianBlur(pad * 2))
    glow.alpha_composite(body, (pad * 3, pad * 3))

    mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, sw - 1, sh - 1], radius=max(8, rad - pad // 2), fill=255)

    return glow, (pad * 3 + pad, pad * 3 + pad), mask, (sw, sh)


def paste_screen(bezel_base, paste_xy, mask, size, screen_img):
    """One device image: the prebuilt bezel with this frame's screen in it."""
    out = bezel_base.copy()
    scr = screen_img.resize(size, Image.LANCZOS).convert("RGBA")
    out.paste(scr, paste_xy, mask)
    return out


def make_device(shot, screen_w):
    """Composite a real capture into a watch bezel, returned RGBA with glow.

    The screen is scaled with LANCZOS from the native 410x502. Scaling up a
    device capture is honest as long as nothing is added to it; the bezel and
    the rim light are clearly frame, not content.
    """
    sw = screen_w
    sh = int(round(sw * PANEL_H / PANEL_W))
    pad = max(10, sw // 26)          # bezel thickness
    rad = max(18, sw // 11)          # outer corner radius
    W, H = sw + pad * 2, sh + pad * 2

    dev = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(dev)
    # body
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=rad, fill=(14, 16, 20, 255))
    # rim light
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=rad,
                        outline=ACCENT_DK + (255,), width=max(2, sw // 300))

    if shot is not None:
        scr = shot.resize((sw, sh), Image.LANCZOS).convert("RGBA")
        mask = Image.new("L", (sw, sh), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            [0, 0, sw - 1, sh - 1], radius=max(8, rad - pad // 2), fill=255)
        dev.paste(scr, (pad, pad), mask)
    else:
        # Outro: no capture. The wordmark fills the panel the way the boot
        # splash does, with the eye motif rule under it - an empty bezel with a
        # small word in the middle reads as a missing asset.
        dd = ImageDraw.Draw(dev)
        f = font(ORBITRON, max(38, sw // 4))
        dd.text((W // 2, H // 2 - sh // 12), "ARGUS", font=f,
                fill=ACCENT_HI, anchor="mm")
        rw = int(sw * 0.42)
        ry = H // 2 + sh // 12
        dd.line([(W // 2 - rw // 2, ry), (W // 2 + rw // 2, ry)],
                fill=ACCENT + (255,), width=max(2, sw // 260))
        sf2 = font(VT323, max(22, sw // 13))
        dd.text((W // 2, ry + sh // 11), "IT WATCHES BACK", font=sf2,
                fill=TEXT_DIM, anchor="mm")

    # glow behind the device, composited by the caller
    glow = Image.new("RGBA", (W + pad * 6, H + pad * 6), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.rounded_rectangle([pad * 3, pad * 3, pad * 3 + W, pad * 3 + H],
                         radius=rad, fill=ACCENT + (60,))
    glow = glow.filter(ImageFilter.GaussianBlur(pad * 2))
    glow.alpha_composite(dev, (pad * 3, pad * 3))
    return glow


def wrap(draw, text, f, maxw):
    """Greedy word wrap that honours explicit newlines in the storyboard."""
    out = []
    for para in text.split("\n"):
        if not para.strip():
            out.append("")
            continue
        line = ""
        for word in para.split():
            trial = (line + " " + word).strip()
            if draw.textlength(trial, font=f) <= maxw or not line:
                line = trial
            else:
                out.append(line)
                line = word
        out.append(line)
    return out


def make_caption(w, h, headline, subline, layout, scale):
    """Pre-render the caption block once per scene as an RGBA layer."""
    lay = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)

    hf = font(ORBITRON, int(52 * scale))
    sf = font(VT323, int(46 * scale))
    maxw = int(w * (0.82 if layout == "stack" else 0.44))

    hl = wrap(d, headline, hf, maxw)
    sl = wrap(d, subline, sf, maxw)

    hlh = int(62 * scale)
    slh = int(50 * scale)
    total = len(hl) * hlh + int(18 * scale) + len(sl) * slh

    if layout == "stack":
        x = w // 2
        y = h - total - int(150 * scale)
        anchor = "ma"
    else:
        x = int(w * 0.52)
        y = (h - total) // 2
        anchor = "la"

    # accent rule above the headline
    rw = int(90 * scale)
    if layout == "stack":
        d.line([(x - rw // 2, y - int(34 * scale)),
                (x + rw // 2, y - int(34 * scale))],
               fill=ACCENT + (255,), width=max(2, int(3 * scale)))
    else:
        d.line([(x, y - int(34 * scale)), (x + rw, y - int(34 * scale))],
               fill=ACCENT + (255,), width=max(2, int(3 * scale)))

    for ln in hl:
        d.text((x, y), ln, font=hf, fill=ACCENT_HI, anchor=anchor)
        y += hlh
    y += int(18 * scale)
    for ln in sl:
        # the pending-validation line is the one caption that must not be
        # skimmed past, so it gets full-brightness cream instead of the dim.
        col = TEXT if ("pending" in ln or "github.com" in ln) else TEXT_DIM
        d.text((x, y), ln, font=sf, fill=col, anchor=anchor)
        y += slh
    return lay


def ease(t):
    """Smootherstep - no linear ramps anywhere in the motion."""
    t = max(0.0, min(1.0, t))
    return t * t * t * (t * (t * 6 - 15) + 10)


def render(aspect, fps, outdir, keep_frames=False):
    w, h, screen_w, screen_y, layout = ASPECTS[aspect]
    scale = h / 1920.0 if layout == "stack" else h / 1080.0 * 0.92
    bg = make_background(w, h)

    tmp = os.path.join(outdir, "_frames_%s" % aspect)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp, exist_ok=True)

    # Pre-render every scene's device + caption once.
    scenes = []
    for shot_name, headline, subline, secs in STORYBOARD:
        shot = load_shot(shot_name) if shot_name else None
        dev = make_device(shot, screen_w)
        cap = make_caption(w, h, headline, subline, layout, scale)
        scenes.append((dev, cap, max(1, int(round(secs * fps)))))

    xfade = max(1, int(0.35 * fps))     # cross-dissolve length in frames
    n = 0

    def compose(dev, cap, k, total):
        """One frame of one scene at progress k/total."""
        p = k / max(1, total - 1)
        # slow push-in on the device: 1.00 -> 1.045 eased. Subtle on purpose;
        # a device capture that drifts too much reads as a stock-video mockup.
        z = 1.0 + 0.045 * ease(p)
        dw = int(dev.width * z)
        dh = int(dev.height * z)
        d2 = dev.resize((dw, dh), Image.LANCZOS)

        f = bg.copy().convert("RGBA")
        if layout == "stack":
            dx = (w - dw) // 2
            dy = int(screen_y * scale) - (dh - dev.height) // 2
        else:
            dx = int(w * 0.10) - (dw - dev.width) // 2
            dy = (h - dh) // 2
        f.alpha_composite(d2, (dx, dy))

        # captions rise a few px and fade in over the first 0.4s
        ci = ease(min(1.0, p * (total / max(1.0, 0.4 * fps))))
        if ci > 0:
            c = cap
            if ci < 1.0:
                c = cap.copy()
                a = c.getchannel("A").point(lambda v: int(v * ci))
                c.putalpha(a)
            f.alpha_composite(c, (0, int((1 - ci) * 18 * scale)))
        return f.convert("RGB")

    prev_tail = []
    for si, (dev, cap, total) in enumerate(scenes):
        frames = [compose(dev, cap, k, total) for k in range(total)]
        if prev_tail:
            # cross-dissolve into this scene
            for j in range(xfade):
                a = (j + 1) / (xfade + 1)
                blend = Image.blend(prev_tail[j], frames[j], a)
                blend.save(os.path.join(tmp, "f%05d.png" % n)); n += 1
            body = frames[xfade:]
        else:
            body = frames
        keep = body[:-xfade] if len(body) > xfade else body
        for fr in keep:
            fr.save(os.path.join(tmp, "f%05d.png" % n)); n += 1
        prev_tail = body[-xfade:] if len(body) > xfade else []
        sys.stderr.write("  scene %d/%d -> %d frames\n" % (si + 1, len(scenes), n))
    for fr in prev_tail:
        fr.save(os.path.join(tmp, "f%05d.png" % n)); n += 1

    mp4 = os.path.join(outdir, "argus-demo-%s.mp4" % aspect)
    cmd = ["ffmpeg", "-y", "-framerate", str(fps),
           "-i", os.path.join(tmp, "f%05d.png"),
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
           "-movflags", "+faststart", mp4]
    subprocess.run(cmd, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    made = [mp4]
    if aspect == "wide":
        # README assets: a silent looping WebM plus a GIF fallback.
        webm = os.path.join(outdir, "argus-demo-loop.webm")
        subprocess.run(["ffmpeg", "-y", "-framerate", str(fps),
                        "-i", os.path.join(tmp, "f%05d.png"),
                        "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "36",
                        "-an", webm], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # GIF sizing is a real constraint, not a detail. GitHub will not play a
        # repo-relative <video> in a README, so the GIF is the only asset that
        # actually animates there - and it has to stay small enough to load.
        # 720px @ 12 fps measured 10.2 MB, which is too heavy; 480px @ 10 fps
        # with a 128-colour palette measures 3.8 MB for the same 27 s. Bump the
        # width back up only if you also shorten the loop.
        gif = os.path.join(outdir, "argus-demo-loop.gif")
        subprocess.run(["ffmpeg", "-y", "-i", mp4,
                        "-vf", "fps=10,scale=480:-1:flags=lanczos,"
                               "split[a][b];[a]palettegen=max_colors=128[p];"
                               "[b][p]paletteuse=dither=bayer:bayer_scale=3",
                        "-loop", "0", gif], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        made += [webm, gif]

    if not keep_frames:
        shutil.rmtree(tmp, ignore_errors=True)
    return made, n


def render_sim(aspect, framedir, outdir, keep_frames=False):
    """Reel from simulator frames: the screen ANIMATES inside the bezel.

    Runs at SIM_FPS because the source frames were dumped at that rate; asking
    for another rate here would resample real motion into something the device
    never did.
    """
    w, h, screen_w, screen_y, layout = ASPECTS[aspect]
    scale = h / 1920.0 if layout == "stack" else h / 1080.0 * 0.92
    fps = SIM_FPS
    bg = make_background(w, h)
    bezel, paste_xy, mask, size = build_bezel(screen_w)

    src = sorted(f for f in os.listdir(framedir) if f.endswith(".ppm"))
    if not src:
        raise SystemExit("no .ppm frames in %s - run `make frames` in sim/ first" % framedir)

    tmp = os.path.join(outdir, "_sim_%s" % aspect)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp, exist_ok=True)

    n = 0
    for first, count, headline, subline in SIM_STORYBOARD:
        if first + count > len(src):
            raise SystemExit("storyboard wants frame %d but only %d captured; "
                             "sim STORY[] and SIM_STORYBOARD are out of sync"
                             % (first + count, len(src)))
        cap = make_caption(w, h, headline, subline, layout, scale)
        for k in range(count):
            scr = Image.open(os.path.join(framedir, src[first + k])).convert("RGB")
            dev = paste_screen(bezel, paste_xy, mask, size, scr)

            f = bg.copy().convert("RGBA")
            if layout == "stack":
                dx, dy = (w - dev.width) // 2, int(screen_y * scale)
            else:
                dx, dy = int(w * 0.10), (h - dev.height) // 2
            f.alpha_composite(dev, (dx, dy))

            ci = ease(min(1.0, k / max(1.0, 0.4 * fps)))
            if ci > 0:
                c = cap
                if ci < 1.0:
                    c = cap.copy()
                    c.putalpha(c.getchannel("A").point(lambda v: int(v * ci)))
                f.alpha_composite(c, (0, int((1 - ci) * 18 * scale)))
            f.convert("RGB").save(os.path.join(tmp, "f%05d.png" % n)); n += 1
        sys.stderr.write("  %-22s %3d frames\n" % (headline[:22], count))

    mp4 = os.path.join(outdir, "argus-sim-%s.mp4" % aspect)
    subprocess.run(["ffmpeg", "-y", "-framerate", str(fps),
                    "-i", os.path.join(tmp, "f%05d.png"),
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
                    "-movflags", "+faststart", mp4], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    made = [mp4]
    if aspect == "wide":
        # README loop from the SIMULATOR frames, same budget as the device
        # path: GitHub only animates a GIF in a README, and 480 px @ 10 fps
        # with a 128-colour palette is what keeps a ~40 s loop near 4 MB.
        # Frames are dropped to 10 fps in the filter, not by re-timing.
        gif = os.path.join(outdir, "argus-sim-loop.gif")
        subprocess.run(["ffmpeg", "-y", "-framerate", str(fps),
                        "-i", os.path.join(tmp, "f%05d.png"),
                        "-vf", "fps=10,scale=480:-1:flags=lanczos,"
                               "split[a][b];[a]palettegen=max_colors=128[p];"
                               "[b][p]paletteuse=dither=bayer:bayer_scale=3",
                        "-loop", "0", gif], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        made.append(gif)
    if not keep_frames:
        shutil.rmtree(tmp, ignore_errors=True)
    return made, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--aspect", choices=list(ASPECTS) + ["all"], default="all")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--out", default=os.path.join(ROOT, "artifacts", "demo"))
    ap.add_argument("--keep-frames", action="store_true")
    ap.add_argument("--sim-frames", metavar="DIR",
                    help="build from SIMULATOR frames (sim/build/frames) instead "
                         "of the stills in img/argus. Current source, and the "
                         "screens animate.")
    a = ap.parse_args()

    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg not on PATH")
    os.makedirs(a.out, exist_ok=True)

    todo = list(ASPECTS) if a.aspect == "all" else [a.aspect]
    for asp in todo:
        sys.stderr.write("[%s]\n" % asp)
        if a.sim_frames:
            made, n = render_sim(asp, a.sim_frames, a.out, a.keep_frames)
        else:
            made, n = render(asp, a.fps, a.out, a.keep_frames)
        for m in made:
            sz = os.path.getsize(m) / 1e6
            sys.stderr.write("  %s  %.1f MB  (%d frames @ %d fps)\n"
                             % (os.path.basename(m), sz, n, a.fps))


if __name__ == "__main__":
    main()
