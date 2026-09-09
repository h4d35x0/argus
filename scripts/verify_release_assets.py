"""Gate the release asset set before it is published.

ARGUS ships FOUR component binaries flashed at four offsets. It deliberately
does not ship a single merged image, because one was shipped on 2026-09-09 and
bricked a watch: `esptool merge_bin --flash_mode qio` rewrote the bootloader's
flash-mode byte at 0x2 from the DIO that PlatformIO builds for this board to
QIO, so the ROM configured SPI for quad reads, could not read flash, and never
booted. CI built that image and could not boot it, so nothing caught it.

This gate enforces the two invariants that would have caught it:

  1. No merged image is present in the release set.
  2. The bootloader's declared flash mode is the one proven to boot on this
     hardware. If it ever changes, that is not automatically wrong, but it MUST
     be booted on a real watch before shipping. This fails so a human looks.

Usage: python scripts/verify_release_assets.py <release-dir>
"""

import sys
from pathlib import Path

REQUIRED = ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin")

# Flash mode byte lives at offset 0x2 of an ESP32 image header.
FLASH_MODE_OFFSET = 0x2
FLASH_MODES = {0: "QIO", 1: "QOUT", 2: "DIO", 3: "DOUT"}
# What PlatformIO builds for lilygo-t-watch-ultra, and what has actually booted.
PROVEN_FLASH_MODE = 2  # DIO


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: python scripts/verify_release_assets.py <release-dir>")
        return 2
    release_dir = Path(argv[1])
    ok = True

    for name in REQUIRED:
        path = release_dir / name
        if path.is_file() and path.stat().st_size > 0:
            print(f"  ok    {name:<16} {path.stat().st_size} B")
        else:
            print(f"  FAIL  {name:<16} missing or empty")
            ok = False

    merged = sorted(
        p.name
        for p in release_dir.glob("argus-*.bin")
        if not p.name.endswith("-sdcard.zip")
    )
    if merged:
        ok = False
        print(f"\n  FAIL  merged image present: {', '.join(merged)}")
        print("        ARGUS does not ship a merged image. A merge step rewrites")
        print("        the bootloader's flash-mode byte and bricked a watch on")
        print("        2026-09-09. Ship the four parts at their own offsets.")

    bootloader = release_dir / "bootloader.bin"
    if bootloader.is_file():
        mode = bootloader.read_bytes()[FLASH_MODE_OFFSET]
        name = FLASH_MODES.get(mode, hex(mode))
        if mode == PROVEN_FLASH_MODE:
            print(f"  ok    bootloader flash mode {name}")
        else:
            ok = False
            print(f"\n  FAIL  bootloader flash mode is {name}, expected "
                  f"{FLASH_MODES[PROVEN_FLASH_MODE]}")
            print("        This may be legitimate (board or toolchain change),")
            print("        but a different flash mode has never been booted on")
            print("        this hardware. Flash it to a real watch and confirm it")
            print("        boots before updating PROVEN_FLASH_MODE here.")

    if not ok:
        print("\nrelease asset set rejected")
        return 1
    print("\nrelease asset set ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
