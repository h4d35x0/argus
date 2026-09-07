#!/usr/bin/env python3
"""Generate the three ARGUS mode wallpapers as raw RGB565 rasters.

src/background.cpp does not run an image DECODER. PNG/JPEG inflate the whole
compressed file in scarce internal SRAM and OOM the watch; the BMP decoder
re-decodes from the SD on every render and starves the main loop. So each mode
wallpaper ships as a raw, panel-sized little-endian RGB565 file that is read
once into PSRAM and blitted. See tasks/WALLPAPER-SAGA.md for the full history.

This script is the single definition of how those rasters are produced:

    tools/wallpapers/<source image>  ->  sdcard/backgrounds/<mode>.rgb565

Outputs are committed so a fresh clone ships all three wallpapers ready to copy
onto a microSD card. --check re-derives them and compares, so a committed
raster can never silently drift from its source.

Usage:
    python tools/gen_wallpapers.py            # regenerate sdcard/backgrounds/
    python tools/gen_wallpapers.py --check    # verify committed rasters match
    python tools/gen_wallpapers.py --stats    # also print luminance stats

Each raster is written alongside a <mode>.preview.png, purely so the artwork is
visible when browsing the repo; nothing reads it and only the .rgb565 files
belong on the card. Previews are not covered by --check, because PNG encoders
differ across Pillow versions and a re-encode is not drift.

Requires Pillow only (pip install pillow).
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - dependency message, not logic
    sys.exit("Pillow is required: pip install pillow")

REPO = Path(__file__).resolve().parent.parent
SRC_DIR = REPO / "tools" / "wallpapers"
OUT_DIR = REPO / "sdcard" / "backgrounds"

# Panel resolution. background.cpp requires an exact WP_W * WP_H * 2 byte file
# and does no scaling, so these must stay in step with WP_W / WP_H there.
W, H = 410, 502
RAW_BYTES = W * H * 2


class Source:
    """One mode wallpaper: where it comes from and how it is fitted and lifted.

    fit
        "exact"   the source is already 410x502; use its pixels unchanged, so
                  the output is byte-identical to the field-proven raster.
        "contain" scale the source's non-black content to fit inside the panel
                  at `margin` of each axis, centred on black.

    value_gamma
        Hue-preserving brightness lift applied after fitting; below 1.0
        brightens. The wallpaper renders at a low opacity (background.cpp
        bg_opa defaults to 75 of 255), so a source that is almost entirely
        near-black reaches the panel as nothing at all. The lift scales each
        pixel's VALUE (its largest channel) by v -> 255 * (v/255) ** gamma and
        scales R, G and B by that same factor, which keeps hue and saturation
        exactly and never clips a channel. 1.0 is a no-op.
    """

    def __init__(self, mode, filename, fit, value_gamma=1.0, margin=0.94, note=""):
        self.mode = mode
        self.filename = filename
        self.fit = fit
        self.value_gamma = value_gamma
        self.margin = margin
        self.note = note

    @property
    def path(self):
        return SRC_DIR / self.filename

    @property
    def out_path(self):
        return OUT_DIR / (self.mode + ".rgb565")


# Indexed by ArgusMode in src/argus_mode.h: Daily, Defense, Offense.
SOURCES = [
    Source(
        "daily", "daily-hades.png", fit="contain", value_gamma=0.55,
        note="HADES logo. Square brand export, so it is contain-fitted to the "
             "panel. The artwork is near-black armour on black with red eyes; "
             "measured against the other two rasters it needed a lift to be "
             "visible at the default wallpaper opacity.",
    ),
    Source(
        "defense", "defense-privacy.png", fit="exact",
        note="Privacy is an Illusion, in the blue Defense accent. Already "
             "panel-sized, passed through unchanged.",
    ),
    Source(
        "offense", "offense.jpg", fit="exact",
        note="Red circuit skull. Already panel-sized, passed through unchanged.",
    ),
]


def content_bbox(im):
    """Bounding box of the non-black content, or the whole image if all black."""
    mask = im.convert("L").point(lambda v: 255 if v > 4 else 0)
    return mask.getbbox() or (0, 0, im.width, im.height)


def flatten(path):
    """Load an image onto black. Transparent logo exports must not become white."""
    im = Image.open(path)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        out = Image.new("RGB", im.size, (0, 0, 0))
        out.paste(im, (0, 0), im)
        return out
    return im.convert("RGB")


def fit_contain(im, margin):
    crop = im.crop(content_bbox(im))
    ratio = min(W * margin / crop.width, H * margin / crop.height)
    scaled = crop.resize(
        (max(1, round(crop.width * ratio)), max(1, round(crop.height * ratio))),
        Image.LANCZOS,
    )
    out = Image.new("RGB", (W, H), (0, 0, 0))
    out.paste(scaled, ((W - scaled.width) // 2, (H - scaled.height) // 2))
    return out


def lift_value(im, gamma):
    """Brighten hue-preservingly. See Source.value_gamma."""
    if gamma == 1.0:
        return im
    # 256-entry map from a pixel's current value to its lifted value.
    lut = [0] + [min(255, round(255.0 * (v / 255.0) ** gamma)) for v in range(1, 256)]
    src = im.tobytes()               # RGB, 3 bytes per pixel
    out = bytearray(len(src))
    for i in range(0, len(src), 3):
        r, g, b = src[i], src[i + 1], src[i + 2]
        v = r if r >= g and r >= b else (g if g >= b else b)
        target = lut[v]
        if v == 0 or target <= v:
            out[i:i + 3] = (r, g, b)
            continue
        scale = target / v
        out[i] = min(255, round(r * scale))
        out[i + 1] = min(255, round(g * scale))
        out[i + 2] = min(255, round(b * scale))
    return Image.frombytes("RGB", im.size, bytes(out))


def render(source):
    """Source image -> the exact 410x502 RGB frame that becomes the raster."""
    im = flatten(source.path)
    if source.fit == "exact":
        if im.size != (W, H):
            raise SystemExit(
                "%s: fit=exact needs a %dx%d source, got %dx%d"
                % (source.filename, W, H, im.width, im.height)
            )
    elif source.fit == "contain":
        im = fit_contain(im, source.margin)
    else:
        raise SystemExit("%s: unknown fit %r" % (source.filename, source.fit))
    return lift_value(im, source.value_gamma)


def to_rgb565(im):
    """Pack to little-endian RGB565, matching LV_COLOR_DEPTH 16 with no swap."""
    src = im.tobytes()               # RGB, 3 bytes per pixel
    out = bytearray(RAW_BYTES)
    for i in range(0, RAW_BYTES // 2):
        j = i * 3
        struct.pack_into(
            "<H", out, i * 2,
            ((src[j] >> 3) << 11) | ((src[j + 1] >> 2) << 5) | (src[j + 2] >> 3))
    return bytes(out)


def luminance_stats(im):
    """mean / p95 / p99 / max of BT.601 luma, for the readability band."""
    lum = sorted(im.convert("L").tobytes())
    n = len(lum)
    return {
        "mean": sum(lum) / n,
        "p95": lum[int(n * 0.95)],
        "p99": lum[int(n * 0.99)],
        "max": lum[-1],
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="verify committed rasters match their sources; write nothing")
    ap.add_argument("--stats", action="store_true",
                    help="print luminance stats for each raster")
    args = ap.parse_args()

    if not args.check:
        OUT_DIR.mkdir(parents=True, exist_ok=True)

    failed = False
    for source in SOURCES:
        if not source.path.exists():
            print("MISSING SOURCE %s" % source.path)
            failed = True
            continue

        frame = render(source)
        raw = to_rgb565(frame)
        assert len(raw) == RAW_BYTES

        if args.check:
            if not source.out_path.exists():
                print("MISSING  %s" % source.out_path.relative_to(REPO))
                failed = True
            elif source.out_path.read_bytes() != raw:
                print("DRIFTED  %s does not match %s"
                      % (source.out_path.relative_to(REPO), source.filename))
                failed = True
            else:
                print("ok       %s" % source.out_path.relative_to(REPO))
        else:
            source.out_path.write_bytes(raw)
            print("wrote    %s  (%d bytes, from %s)"
                  % (source.out_path.relative_to(REPO), len(raw), source.filename))
            frame.save(source.out_path.with_suffix(".preview.png"))

        if args.stats:
            s = luminance_stats(frame)
            print("         luma mean=%6.2f  p95=%3d  p99=%3d  max=%3d"
                  % (s["mean"], s["p95"], s["p99"], s["max"]))

    if failed:
        if args.check:
            print("")
            print("Regenerate with: python tools/gen_wallpapers.py")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
