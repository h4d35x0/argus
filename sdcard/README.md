# SD card files

This directory mirrors the layout ARGUS expects on the microSD card. Copy its
contents to the **root** of a FAT32-formatted card, so that the card ends up
with a top-level `backgrounds/` directory:

```
<microSD root>/
    backgrounds/
        daily.rgb565
        defense.rgb565
        offense.rgb565
```

The `.preview.png` files are here only so the artwork is visible when browsing
this repo. Nothing on the watch reads them, and they do not need to be copied.

## backgrounds/

One wallpaper per mode, picked automatically when the mode changes:

| File | Mode | Artwork |
|------|------|---------|
| `daily.rgb565` | Daily (default) | HADES logo |
| `defense.rgb565` | Defense | Privacy is an Illusion |
| `offense.rgb565` | Offense | Circuit skull |

Turn **Wallpaper** on in Settings to see them. A missing mode file falls back to
`daily.rgb565`; a missing `daily.rgb565` means no wallpaper at all. Neither is
an error and neither can crash the watch.

The wallpaper renders faintly *behind* the clock and the Matrix rain, at an
opacity of 75 of 255 by default, so it reads as a backdrop rather than a
picture. That low opacity is the reason the rasters are tuned rather than
dropped in raw: artwork that is mostly near-black arrives at the panel as
nothing. `tools/gen_wallpapers.py` prints the luminance of each raster so the
three stay in the same band.

### Why raw RGB565 and not PNG

The watch does **not** decode these files. PNG and JPEG decoders buffer the
whole compressed file in scarce internal SRAM to inflate it, which OOMs the
board and boot-loops it; the BMP decoder re-reads and re-decodes from the card
on *every* render, which starves the main loop enough to break button polling.

So each wallpaper is a raw, panel-sized, little-endian RGB565 file: exactly
410 x 502 x 2 = 411,640 bytes, matching `LV_COLOR_DEPTH 16` with no byte swap.
It is read once into PSRAM at boot and blitted from there, so no decoder ever
runs. `tasks/WALLPAPER-SAGA.md` has the full history of how that was arrived at.

### Using your own artwork

Replace a source image in `tools/wallpapers/`, then regenerate:

```bash
pip install pillow
python tools/gen_wallpapers.py --stats
```

That rewrites the `.rgb565` files here from the sources. `--check` re-derives
them without writing and fails if a committed raster no longer matches its
source, so the two cannot silently drift apart.

A source that is already exactly 410 x 502 is used pixel for pixel. Anything
else is centred and scaled to fit. See the `SOURCES` table at the top of
`tools/gen_wallpapers.py` for the per-image settings.

Note that these three filenames are the only ones the watch looks for. Dropping
a `wallpaper.png` into `backgrounds/` does nothing: there is no directory scan
and no image decoder, by design.
