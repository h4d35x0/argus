"""ARGUS — HD tool-icon sprite generator.

Mirrors gen_hexhound_sprites.py: draw at 4x supersample with shading + neon rim,
downscale, output transparent PNGs the watch loads from SD (/Icons/<tool>.png) via
lv_image_set_src("A:/Icons/<tool>.png"), with the procedural draw_*_icon() kept as a
fallback when the file is absent. ZERO flash cost (flash already at 88.2%).

ARGUS icon language: dark rounded-tile ground, top rim-light, thin outer neon
bloom, semantic accent (steel-blue = passive/recon, HADES-red = threat/detect,
amber = TX/offensive). A few icons intentionally break the palette to look like the
real object (AirTag white puck, pager yellow gadget, Tesla amber).

Per-icon spec (user direction 2026-07-20):
  wifi/analyzer/radar/aprs  approved as-is
  pager     yellow "pineapple" gadget like 13-37's
  tpms      clever: tyre + gauge + RF
  tesla     ARGUS-ified Tesla "T"
  airtag    HD realistic white puck
  skimmer   card + mag-stripe (red) + a WiFi scan symbol
  eviltwin  two APs, one made to look EVIL (red + horns)
  flock     bird-of-prey silhouette (surveillance vendor)
  pwn       evil chess pawn: dark waisted body, red neon rim, devil horns + glowing eyes
  mouse     ARGUS-ified computer mouse (BT HID)
  microsd   ARGUS-ified SD card
  flipper   KEEP existing flipper_logo_img (not generated here)
  pet       KEEP HexHound HD sprite (not generated here)

USAGE:  pip install Pillow ; python tools/gen_icon_sprites.py
Outputs tools/icon_out/asset_<tool>.png (200px transparent) + contact_sheet.png.
DEPLOY: copy asset_<tool>.png onto SD as /Icons/<tool>.png.
"""
import os, math
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon_out")
os.makedirs(OUT, exist_ok=True)

SS = 4
W = H = 200
CW = CH = W * SS

STEEL     = (155, 188, 214); STEEL_HI = (200, 222, 240); STEEL_DK = (91, 124, 150)
RED       = (219, 97, 90);   RED_HI   = (255, 150, 140); RED_DK   = (120, 40, 38)
AMBER     = (255, 176, 32);  AMBER_HI = (255, 214, 120); AMBER_DK = (150, 96, 8)
WHITE     = (238, 240, 244); WHITE_HI = (255, 255, 255); GREY_DK  = (150, 156, 166)
GREEN_LCD = (60, 220, 120)
TILE_DK   = (14, 20, 28);    TILE_LT  = (26, 36, 48)


def sc(v): return int(v * SS)
def new_canvas(): return Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
def A(c, a=255): return (c[0], c[1], c[2], a)


def tile_ground():
    grad = Image.new("RGBA", (CW, CH), (0, 0, 0, 0)); gd = ImageDraw.Draw(grad)
    for y in range(CH):
        t = y / CH
        col = tuple(int(TILE_LT[i] * (1 - t) + TILE_DK[i] * t) for i in range(3))
        gd.line([(0, y), (CW, y)], fill=col + (255,))
    mask = Image.new("L", (CW, CH), 0)
    ImageDraw.Draw(mask).rounded_rectangle([sc(6), sc(6), CW - sc(6), CH - sc(6)], radius=sc(40), fill=255)
    base = Image.new("RGBA", (CW, CH), (0, 0, 0, 0)); base.paste(grad, (0, 0), mask)
    return base, mask


DL = os.environ.get("ARGUS_ICON_SRC", "./icon-src")   # source art dir; override with ARGUS_ICON_SRC


def image_glyph(path, accent, hi, target=150):
    """Customize a provided PNG: use its alpha as the shape mask, crop, scale
    to fit, and refill with a top(hi)->bottom(accent) brand gradient."""
    src = Image.open(path).convert("RGBA")
    a = src.split()[3]
    bb = a.getbbox()
    if bb:
        a = a.crop(bb)
    nw, nh = a.size
    scale = sc(target) / max(nw, nh)
    nw, nh = max(1, int(nw * scale)), max(1, int(nh * scale))
    a = a.resize((nw, nh), Image.LANCZOS)
    grad = Image.new("RGBA", (nw, nh), (0, 0, 0, 0)); gd = ImageDraw.Draw(grad)
    for y in range(nh):
        t = y / max(1, nh - 1)
        col = tuple(int(hi[i] * (1 - t) + accent[i] * t) for i in range(3))
        gd.line([(0, y), (nw, y)], fill=col + (255,))
    small = Image.new("RGBA", (nw, nh), (0, 0, 0, 0)); small.paste(grad, (0, 0), a)
    lay = new_canvas(); lay.alpha_composite(small, ((CW - nw) // 2, (CH - nh) // 2))
    return lay


def glow(layer, color):
    a = layer.split()[3]
    g = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    ImageDraw.Draw(g).bitmap((0, 0), a, fill=color + (255,))
    return g.filter(ImageFilter.GaussianBlur(sc(9)))


# On-device sprites are TRANSPARENT glyph + neon glow only (no tile ground): they
# compose onto the LVGL tile the same way the HexHound pet / Flipper icons do, so
# the rounded-tile look lives once in make_tile() (LVGL), not baked per icon.
OUT_SIZE = 140   # px placed in each 180px tile's icon area, above the label


def finish(name, glyph, accent):
    bloom = glow(glyph, accent)
    out = Image.alpha_composite(Image.new("RGBA", (CW, CH), (0, 0, 0, 0)), bloom)
    out = Image.alpha_composite(out, glyph).resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)
    out.save(os.path.join(OUT, f"asset_{name}.png"))
    return out


# ---------------------------------------------------------------- glyphs -----

def g_wifi(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW // 2, sc(140)
    for i, r in enumerate((sc(88), sc(62), sc(36))):
        d.arc([cx-r, cy-r, cx+r, cy+r], 215, 325, fill=A(hi if i == 2 else a), width=sc(11))
    d.ellipse([cx-sc(11), cy-sc(11), cx+sc(11), cy+sc(11)], fill=A(hi))
    return l

def g_analyzer(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l)
    hs = [40, 78, 120, 150, 128, 92, 54]; bw, gap = sc(15), sc(7)
    x = (CW - (7*bw + 6*gap)) // 2; by = sc(158)
    for h in hs:
        hp = sc(h); d.rounded_rectangle([x, by-hp, x+bw, by], radius=sc(3),
                                        fill=A(hi if h == max(hs) else a)); x += bw+gap
    return l

def g_radar(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW//2, CH//2
    for r in (sc(38), sc(64), sc(90)): d.ellipse([cx-r, cy-r, cx+r, cy+r], outline=A(a), width=sc(4))
    d.pieslice([cx-sc(90), cy-sc(90), cx+sc(90), cy+sc(90)], -70, -25, fill=A(hi, 70))
    d.line([cx, cy, cx+sc(64), cy-sc(64)], fill=A(hi), width=sc(5))
    d.ellipse([cx+sc(30), cy-sc(46), cx+sc(46), cy-sc(30)], fill=A(RED))
    return l

def g_aprs(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l)
    d.line([sc(100), sc(60), sc(100), sc(150)], fill=A(a), width=sc(8))
    d.line([sc(100), sc(150), sc(70), sc(168)], fill=A(a), width=sc(6))
    d.line([sc(100), sc(150), sc(130), sc(168)], fill=A(a), width=sc(6))
    d.ellipse([sc(88), sc(48), sc(112), sc(72)], fill=A(hi))
    for r in (sc(34), sc(56)): d.arc([sc(100)-r, sc(60)-r, sc(100)+r, sc(60)+r], 200, 340, fill=A(hi), width=sc(5))
    return l

def g_pager(a, hi):
    # yellow handheld gadget w/ green LCD, like 13-37's ("pineapple pager")
    l = new_canvas(); d = ImageDraw.Draw(l)
    d.rounded_rectangle([sc(52), sc(30), sc(148), sc(172)], radius=sc(14), fill=A(AMBER),
                        outline=A(AMBER_DK), width=sc(3))
    d.rounded_rectangle([sc(66), sc(44), sc(134), sc(92)], radius=sc(6), fill=A(GREEN_LCD))  # LCD
    for i in range(3):
        d.line([sc(74), sc(56)+i*sc(11), sc(126), sc(56)+i*sc(11)], fill=A((0, 90, 40)), width=sc(3))
    for r in range(2):      # button grid
        for c in range(3):
            bx, by = sc(70)+c*sc(24), sc(108)+r*sc(26)
            d.rounded_rectangle([bx, by, bx+sc(16), by+sc(16)], radius=sc(3), fill=A((20, 20, 20)),
                                outline=A(AMBER_DK), width=sc(1))
    return l

def g_tpms(a, hi):
    # tyre (dark torus) + pressure gauge dial + RF ping = "clever" TPMS
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = sc(86), CH//2
    d.ellipse([cx-sc(74), cy-sc(74), cx+sc(74), cy+sc(74)], outline=A((40, 46, 56)), width=sc(26))  # tyre
    d.ellipse([cx-sc(74), cy-sc(74), cx+sc(74), cy+sc(74)], outline=A(a), width=sc(4))
    d.ellipse([cx-sc(40), cy-sc(40), cx+sc(40), cy+sc(40)], fill=A((18, 24, 32)), outline=A(STEEL_DK), width=sc(3))
    # gauge needle
    ang = math.radians(-45); d.line([cx, cy, cx+int(sc(30)*math.cos(ang)), cy+int(sc(30)*math.sin(ang))], fill=A(hi), width=sc(5))
    d.ellipse([cx-sc(7), cy-sc(7), cx+sc(7), cy+sc(7)], fill=A(hi))
    for r in (sc(20), sc(34)): d.arc([sc(150)-r, sc(70)-r, sc(150)+r, sc(70)+r], -55, 55, fill=A(hi), width=sc(5))  # RF ping
    return l

def g_tesla(a, hi):
    # Tesla "T" wordmark: wide top crossbar that droops at the tips, a small
    # triangular tooth on top-centre, and a tapering vertical stem below a gap.
    l = new_canvas(); d = ImageDraw.Draw(l); cx = CW//2; ty = sc(72)
    # wide, nearly-flat crossbar with the tips dipping only slightly
    d.line([cx-sc(82), ty+sc(12), cx-sc(24), ty, cx+sc(24), ty, cx+sc(82), ty+sc(12)],
           fill=A(a), width=sc(14), joint="curve")
    # small pointed cap on top-centre (short, not an arrow)
    d.polygon([(cx, sc(50)), (cx-sc(11), ty-sc(3)), (cx+sc(11), ty-sc(3))], fill=A(hi))
    # long thin tapering stem below a small gap
    d.polygon([(cx-sc(10), ty+sc(20)), (cx+sc(10), ty+sc(20)),
               (cx+sc(5), sc(176)), (cx-sc(5), sc(176))], fill=A(a))
    d.polygon([(cx-sc(10), ty+sc(20)), (cx+sc(10), ty+sc(20)),
               (cx+sc(8), ty+sc(42)), (cx-sc(8), ty+sc(42))], fill=A(hi))
    return l

def g_airtag(a, hi):
    # HD realistic white puck: radial-ish shading + rim + centre logo dot + shadow
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW//2, CH//2 + sc(4)
    d.ellipse([cx-sc(70), cy+sc(52), cx+sc(70), cy+sc(78)], fill=(0, 0, 0, 90))          # soft shadow
    for i in range(sc(76), 0, -sc(2)):                                                    # shaded body
        t = i / sc(76); col = tuple(int(WHITE_HI[j]*t + GREY_DK[j]*(1-t)) for j in range(3))
        d.ellipse([cx-i, cy-i, cx+i, cy+i], fill=col + (255,))
    d.ellipse([cx-sc(76), cy-sc(76), cx+sc(76), cy+sc(76)], outline=A((120, 126, 136)), width=sc(2))
    d.ellipse([cx-sc(52), cy-sc(66), cx+sc(30), cy-sc(20)], fill=(255, 255, 255, 130))    # glossy highlight
    d.ellipse([cx-sc(18), cy-sc(18), cx+sc(18), cy+sc(18)], outline=A((170, 176, 186)), width=sc(3))
    d.ellipse([cx-sc(6), cy-sc(6), cx+sc(6), cy+sc(6)], fill=A((150, 156, 166)))
    return l

def g_skimmer(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l)
    d.rounded_rectangle([sc(34), sc(70), sc(150), sc(150)], radius=sc(12), outline=A(a), width=sc(6))
    d.rectangle([sc(34), sc(88), sc(150), sc(104)], fill=A(a))                            # mag stripe
    d.rounded_rectangle([sc(50), sc(116), sc(78), sc(138)], radius=sc(4), fill=A(hi))     # chip
    cx, cy = sc(146), sc(60)                                                              # WiFi scan symbol
    for r in (sc(20), sc(34), sc(48)): d.arc([cx-r, cy-r, cx+r, cy+r], 30, 150, fill=A(hi), width=sc(5))
    d.ellipse([cx-sc(6), cy-sc(6), cx+sc(6), cy+sc(6)], fill=A(hi))
    return l

def g_eviltwin(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l)
    # good AP (steel) left, EVIL AP (red + horns) right
    for cx, col, evil in ((sc(72), STEEL, False), (sc(128), RED, True)):
        cy = sc(150)
        for r in (sc(66), sc(45), sc(24)): d.arc([cx-r, cy-r, cx+r, cy+r], 210, 330, fill=A(col), width=sc(8))
        d.ellipse([cx-sc(10), cy-sc(10), cx+sc(10), cy+sc(10)], fill=A(col))
        if evil:
            d.polygon([(cx-sc(14), cy-sc(6)), (cx-sc(24), cy-sc(24)), (cx-sc(6), cy-sc(12))], fill=A(RED_HI))  # horns
            d.polygon([(cx+sc(14), cy-sc(6)), (cx+sc(24), cy-sc(24)), (cx+sc(6), cy-sc(12))], fill=A(RED_HI))
            d.ellipse([cx-sc(6), cy-sc(5), cx-sc(1), cy], fill=A((0, 0, 0)))               # menacing eyes
            d.ellipse([cx+sc(1), cy-sc(5), cx+sc(6), cy], fill=A((0, 0, 0)))
    return l

def g_flock(a, hi):
    # hawk/falcon in flight, wings swept up in a shallow V (surveillance -> red)
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW//2, CH//2 + sc(6)
    # each wing: an upswept blade with a rounded leading edge and a tapered tip
    for s in (-1, 1):
        d.polygon([(cx, cy-sc(8)),
                   (cx+s*sc(30), cy-sc(30)),
                   (cx+s*sc(84), cy-sc(58)),          # swept tip up
                   (cx+s*sc(70), cy-sc(30)),
                   (cx+s*sc(78), cy-sc(6)),           # feather notch
                   (cx+s*sc(54), cy-sc(10)),
                   (cx+s*sc(30), cy+sc(6)),
                   (cx, cy+sc(10))], fill=A(a))
    d.ellipse([cx-sc(8), cy-sc(28), cx+sc(8), cy-sc(8)], fill=A(a))                        # head
    d.polygon([(cx, cy-sc(30)), (cx+sc(6), cy-sc(38)), (cx-sc(6), cy-sc(38))], fill=A(a))  # crown
    d.polygon([(cx, cy-sc(18)), (cx+sc(12), cy-sc(14)), (cx, cy-sc(8))], fill=A(AMBER))    # hooked beak
    d.polygon([(cx-sc(9), cy+sc(8)), (cx+sc(9), cy+sc(8)), (cx, cy+sc(58))], fill=A(a))    # tail fan
    return l

def g_pwn(a, hi):
    # neon cyber-skull clutching a handshake packet ("be creative")
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW//2, sc(84)
    d.rounded_rectangle([cx-sc(46), cy-sc(46), cx+sc(46), cy+sc(30)], radius=sc(30), fill=A((20, 28, 38)),
                        outline=A(a), width=sc(4))
    d.ellipse([cx-sc(30), cy-sc(20), cx-sc(8), cy+sc(6)], fill=A(RED))                     # eye sockets glow red
    d.ellipse([cx+sc(8), cy-sc(20), cx+sc(30), cy+sc(6)], fill=A(RED))
    d.polygon([(cx, cy+sc(4)), (cx-sc(7), cy+sc(18)), (cx+sc(7), cy+sc(18))], fill=A(a))   # nose
    d.rounded_rectangle([cx-sc(30), cy+sc(30), cx+sc(30), cy+sc(52)], radius=sc(6), fill=A((20, 28, 38)),
                        outline=A(a), width=sc(3))                                          # jaw
    for i in range(4): d.line([cx-sc(22)+i*sc(15), cy+sc(30), cx-sc(22)+i*sc(15), cy+sc(52)], fill=A(a), width=sc(2))
    # captured handshake packet below
    d.rounded_rectangle([cx-sc(40), sc(150), cx+sc(40), sc(178)], radius=sc(5), fill=A(hi, 40), outline=A(hi), width=sc(3))
    d.arc([cx-sc(10), sc(140), cx+sc(10), sc(166)], 180, 360, fill=A(hi), width=sc(4))     # lock shackle
    return l

def g_worldclock(a, hi):
    # wireframe globe (lat/long lines) doubling as a clock face — hands from centre.
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW // 2, CH // 2; R = sc(70)
    d.ellipse([cx-R, cy-R, cx+R, cy+R], outline=A(a), width=sc(5))
    for dy in (-sc(36), 0, sc(36)):                          # parallels
        rw = R if dy == 0 else int((R * R - dy * dy) ** 0.5)
        rh = sc(11)
        d.ellipse([cx-rw, cy+dy-rh, cx+rw, cy+dy+rh], outline=A(STEEL_DK), width=sc(2))
    for rw in (sc(26), sc(50)):                              # meridians
        d.ellipse([cx-rw, cy-R, cx+rw, cy+R], outline=A(STEEL_DK), width=sc(2))
    d.line([cx, cy-R, cx, cy+R], fill=A(STEEL_DK), width=sc(2))
    d.line([cx, cy, cx, cy-sc(42)], fill=A(hi), width=sc(7))       # minute hand
    d.line([cx, cy, cx+sc(30), cy+sc(16)], fill=A(hi), width=sc(7))  # hour hand
    d.ellipse([cx-sc(7), cy-sc(7), cx+sc(7), cy+sc(7)], fill=A(hi))
    return l

def g_sunmoon(a, hi):
    # amber sun with rays beside a cool crescent moon.
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW // 2, CH // 2
    sx, sy, sr = cx - sc(30), cy + sc(10), sc(40)
    for i in range(12):                                      # rays
        ang = math.radians(i * 30)
        x1 = sx + int((sr + sc(9)) * math.cos(ang)); y1 = sy + int((sr + sc(9)) * math.sin(ang))
        x2 = sx + int((sr + sc(24)) * math.cos(ang)); y2 = sy + int((sr + sc(24)) * math.sin(ang))
        d.line([x1, y1, x2, y2], fill=A(a), width=sc(5))
    d.ellipse([sx-sr, sy-sr, sx+sr, sy+sr], fill=A(a))
    d.ellipse([sx-sr, sy-sr, sx+sr, sy+sr], outline=A(AMBER_HI), width=sc(3))
    mx, my, mr = cx + sc(42), cy - sc(24), sc(40)            # crescent moon
    mask = Image.new("L", (CW, CH), 0); mk = ImageDraw.Draw(mask)
    mk.ellipse([mx-mr, my-mr, mx+mr, my+mr], fill=255)
    mk.ellipse([mx-mr+sc(24), my-mr-sc(3), mx+mr+sc(24), my+mr-sc(3)], fill=0)
    solid = Image.new("RGBA", (CW, CH), (STEEL_HI[0], STEEL_HI[1], STEEL_HI[2], 255))
    l.paste(solid, (0, 0), mask)
    return l

def g_evilpawn(a, hi):
    # menacing chess pawn ("pwn"): dark waisted body, neon-red rim + glow, devil
    # horns and glowing amber scowling eyes. A pawn that clearly means business,
    # so the pun lands without reading as "weak/expendable".
    l = new_canvas(); d = ImageDraw.Draw(l); cx = CW // 2
    body = (24, 30, 40); edge = a
    # base: two stacked tiers
    d.rounded_rectangle([cx-sc(52), sc(156), cx+sc(52), sc(178)], radius=sc(8),
                        fill=A(body), outline=A(edge), width=sc(4))
    d.rounded_rectangle([cx-sc(40), sc(140), cx+sc(40), sc(158)], radius=sc(6),
                        fill=A(body), outline=A(edge), width=sc(3))
    # waisted body (lower flare + upper flare meeting a narrow waist)
    d.polygon([(cx-sc(34), sc(146)), (cx+sc(34), sc(146)),
               (cx+sc(18), sc(114)), (cx-sc(18), sc(114))], fill=A(body))
    d.polygon([(cx-sc(18), sc(114)), (cx+sc(18), sc(114)),
               (cx+sc(30), sc(94)),  (cx-sc(30), sc(94))],  fill=A(body))
    d.line([(cx-sc(34), sc(146)), (cx-sc(18), sc(114)), (cx-sc(30), sc(94))], fill=A(edge), width=sc(4), joint="curve")
    d.line([(cx+sc(34), sc(146)), (cx+sc(18), sc(114)), (cx+sc(30), sc(94))], fill=A(edge), width=sc(4), joint="curve")
    # neck collar under head
    d.rounded_rectangle([cx-sc(34), sc(86), cx+sc(34), sc(102)], radius=sc(8),
                        fill=A(body), outline=A(edge), width=sc(3))
    # head
    hy = sc(58); hr = sc(30)
    d.ellipse([cx-hr, hy-hr, cx+hr, hy+hr], fill=A(body), outline=A(edge), width=sc(4))
    # horns (spikes up-and-out)
    d.polygon([(cx-sc(8), hy-sc(24)), (cx-sc(26), hy-sc(20)), (cx-sc(36), hy-sc(54))], fill=A(edge))
    d.polygon([(cx+sc(8), hy-sc(24)), (cx+sc(26), hy-sc(20)), (cx+sc(36), hy-sc(54))], fill=A(edge))
    # glowing angry eyes (angled inward-down)
    eye = AMBER_HI
    d.polygon([(cx-sc(21), hy-sc(3)), (cx-sc(5), hy+sc(4)),
               (cx-sc(7), hy+sc(12)), (cx-sc(23), hy+sc(6))], fill=A(eye))
    d.polygon([(cx+sc(21), hy-sc(3)), (cx+sc(5), hy+sc(4)),
               (cx+sc(7), hy+sc(12)), (cx+sc(23), hy+sc(6))], fill=A(eye))
    # brow ridge for a scowl
    d.line([(cx-sc(24), hy-sc(10)), (cx-sc(4), hy-sc(2))], fill=A(edge), width=sc(3))
    d.line([(cx+sc(24), hy-sc(10)), (cx+sc(4), hy-sc(2))], fill=A(edge), width=sc(3))
    return l

def g_mouse(a, hi):
    # ARGUS-ified computer mouse (BT HID) w/ a little BT rune
    l = new_canvas(); d = ImageDraw.Draw(l); cx = CW//2
    d.rounded_rectangle([cx-sc(46), sc(48), cx+sc(46), sc(168)], radius=sc(46), fill=A((20, 28, 38)),
                        outline=A(a), width=sc(5))
    d.line([cx, sc(52), cx, sc(104)], fill=A(a), width=sc(4))                              # button split
    d.rounded_rectangle([cx-sc(7), sc(60), cx+sc(7), sc(92)], radius=sc(6), fill=A(hi))    # scroll wheel
    # BT rune bottom
    by = sc(140)
    d.line([cx, by-sc(18), cx, by+sc(18)], fill=A(hi), width=sc(3))
    d.line([cx, by-sc(18), cx+sc(12), by-sc(8), cx-sc(12), by+sc(8), cx, by+sc(18)], fill=A(hi), width=sc(3), joint="curve")
    d.line([cx, by-sc(18), cx-sc(12), by-sc(8), cx+sc(12), by+sc(8), cx, by+sc(18)], fill=A(hi), width=sc(3), joint="curve")
    return l

def g_microsd(a, hi):
    # ARGUS-ified microSD card
    l = new_canvas(); d = ImageDraw.Draw(l); x0, y0, x1, y1 = sc(58), sc(40), sc(142), sc(168)
    d.polygon([(x0, y0+sc(30)), (x0+sc(30), y0), (x1, y0), (x1, y1), (x0, y1)], fill=A((24, 34, 46)),
              outline=A(a), width=sc(4))
    for i in range(5):                                                                     # gold contacts
        cxp = x0+sc(14)+i*sc(14); d.rounded_rectangle([cxp, y0+sc(44), cxp+sc(8), y0+sc(78)], radius=sc(2), fill=A(hi))
    d.line([x0+sc(6), y1-sc(10), x1-sc(6), y1-sc(10)], fill=A(a), width=sc(3))
    return l


def g_alarm(a, hi):
    # twin-bell alarm clock (Time screen)
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW//2, sc(112)
    for s in (-1, 1):                                                    # bells
        d.pieslice([cx+s*sc(44)-sc(20), sc(40), cx+s*sc(44)+sc(20), sc(80)], 180, 360, fill=A(a))
        d.line([cx+s*sc(30), sc(58), cx+s*sc(16), sc(74)], fill=A(a), width=sc(6))
    d.ellipse([cx-sc(48), cy-sc(48), cx+sc(48), cy+sc(48)], outline=A(a), width=sc(6))   # face
    d.ellipse([cx-sc(48), cy-sc(48), cx+sc(48), cy+sc(48)], fill=A((16, 24, 34)))
    d.ellipse([cx-sc(48), cy-sc(48), cx+sc(48), cy+sc(48)], outline=A(a), width=sc(6))
    d.line([cx, cy, cx, cy-sc(30)], fill=A(hi), width=sc(6))             # minute hand
    d.line([cx, cy, cx+sc(22), cy+sc(10)], fill=A(hi), width=sc(6))      # hour hand
    d.ellipse([cx-sc(6), cy-sc(6), cx+sc(6), cy+sc(6)], fill=A(hi))
    for s in (-1, 1): d.line([cx+s*sc(30), cy+sc(44), cx+s*sc(44), cy+sc(60)], fill=A(a), width=sc(6))  # legs
    return l

def g_stopwatch(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l); cx, cy = CW//2, sc(116)
    d.rounded_rectangle([cx-sc(10), sc(40), cx+sc(10), sc(58)], radius=sc(3), fill=A(a))  # top button
    d.line([cx, sc(58), cx, sc(70)], fill=A(a), width=sc(8))                              # stem
    d.ellipse([cx-sc(52), cy-sc(52), cx+sc(52), cy+sc(52)], fill=A((16, 24, 34)), outline=A(a), width=sc(6))
    for ang in range(0, 360, 30):                                                        # ticks
        r = math.radians(ang)
        d.line([cx+int(sc(46)*math.cos(r)), cy+int(sc(46)*math.sin(r)),
                cx+int(sc(52)*math.cos(r)), cy+int(sc(52)*math.sin(r))], fill=A(a), width=sc(3))
    d.line([cx, cy, cx+sc(24), cy-sc(30)], fill=A(RED), width=sc(5))                      # sweep hand
    d.ellipse([cx-sc(6), cy-sc(6), cx+sc(6), cy+sc(6)], fill=A(hi))
    return l

def g_timer(a, hi):
    # hourglass / sand-timer
    l = new_canvas(); d = ImageDraw.Draw(l); cx = CW//2
    d.line([cx-sc(48), sc(46), cx+sc(48), sc(46)], fill=A(a), width=sc(8))    # top cap
    d.line([cx-sc(48), sc(170), cx+sc(48), sc(170)], fill=A(a), width=sc(8))  # bottom cap
    d.polygon([(cx-sc(42), sc(50)), (cx+sc(42), sc(50)), (cx+sc(6), sc(108)), (cx-sc(6), sc(108))], outline=A(a), width=sc(6))
    d.polygon([(cx-sc(6), sc(108)), (cx+sc(6), sc(108)), (cx+sc(42), sc(166)), (cx-sc(42), sc(166))], outline=A(a), width=sc(6))
    d.polygon([(cx-sc(30), sc(60)), (cx+sc(30), sc(60)), (cx+sc(5), sc(100)), (cx-sc(5), sc(100))], fill=A(hi))  # top sand
    d.polygon([(cx-sc(24), sc(166)), (cx+sc(24), sc(166)), (cx, sc(140))], fill=A(hi))                           # bottom sand
    d.line([cx, sc(108), cx, sc(128)], fill=A(hi), width=sc(3))               # falling grain
    return l

def g_calendar(a, hi):
    l = new_canvas(); d = ImageDraw.Draw(l); x0, y0, x1, y1 = sc(46), sc(52), sc(154), sc(168)
    d.rounded_rectangle([x0, y0, x1, y1], radius=sc(10), fill=A((16, 24, 34)), outline=A(a), width=sc(5))
    d.rounded_rectangle([x0, y0, x1, y0+sc(26)], radius=sc(10), fill=A(a))    # header band
    d.rectangle([x0, y0+sc(16), x1, y0+sc(26)], fill=A(a))
    for s in (-1, 1): d.line([cx_ := CW//2 + s*sc(30), sc(40), cx_, sc(60)], fill=A(hi), width=sc(6))  # rings
    for r in range(3):                                                        # day dots
        for c in range(4):
            dx = x0+sc(16)+c*sc(28); dy = y0+sc(44)+r*sc(24)
            d.ellipse([dx, dy, dx+sc(10), dy+sc(10)], fill=A(hi if (r*4+c) == 5 else a))
    return l


ICONS = [
    ("alarm",     g_alarm,     STEEL, STEEL_HI),
    ("stopwatch", g_stopwatch, STEEL, STEEL_HI),
    ("timer",     g_timer,     STEEL, STEEL_HI),
    ("calendar",  g_calendar,  STEEL, STEEL_HI),
    ("wifi",     g_wifi,     STEEL, STEEL_HI),
    ("analyzer", g_analyzer, STEEL, STEEL_HI),
    ("radar",    g_radar,    STEEL, STEEL_HI),
    ("aprs",     g_aprs,     AMBER, AMBER_HI),
    ("tpms",     g_tpms,     STEEL, STEEL_HI),
    # tesla + flock customized from the user's provided art (Tesla logo -> amber,
    # flock-of-birds -> HADES red). pager is intentionally NOT here: it uses 13-37's
    # procedural icon (draw_pager_icon) per user direction.
    ("tesla",    lambda a, hi: image_glyph(DL + "/Tesla.png",         a, hi, target=140), AMBER, AMBER_HI),
    ("airtag",   g_airtag,   STEEL, STEEL_HI),
    ("skimmer",  g_skimmer,  RED,   RED_HI),
    ("eviltwin", g_eviltwin, STEEL, STEEL_HI),
    ("flock",    lambda a, hi: image_glyph(DL + "/flock safety 2.png", a, hi, target=150), RED, RED_HI),
    ("pwn",      g_evilpawn, RED,   RED_HI),   # evil chess pawn (was g_pwn cyber-skull)
    ("mouse",    g_mouse,    STEEL, STEEL_HI),
    ("microsd",  g_microsd,  STEEL, STEEL_HI),
    ("worldclock", g_worldclock, STEEL, STEEL_HI),
    ("sunmoon",    g_sunmoon,    AMBER, AMBER_HI),
]


def main():
    rendered = []
    for name, fn, accent, hi in ICONS:
        rendered.append((name, finish(name, fn(accent, hi), accent)))
        print("wrote asset_%s.png" % name)
    # Preview each transparent sprite on a realistic 180px LVGL-style tile
    # (dark vertical gradient + accent-dim rounded border + bottom label) so the
    # contact sheet shows the true on-device composition.
    TS = 180; cols = 4; rows = (len(rendered)+cols-1)//cols; pad = 18
    sheet = Image.new("RGBA", (cols*(TS+pad)+pad, rows*(TS+pad)+pad), (0, 0, 0, 255))
    for i, (name, img) in enumerate(rendered):
        r, c = divmod(i, cols); x = pad + c*(TS+pad); y = pad + r*(TS+pad)
        tile = Image.new("RGBA", (TS, TS), (0, 0, 0, 0)); td = ImageDraw.Draw(tile)
        for yy in range(TS):
            t = yy/TS; col = tuple(int((22, 30, 40)[k]*(1-t)+(13, 19, 27)[k]*t) for k in range(3))
            td.line([(0, yy), (TS, yy)], fill=col+(255,))
        m = Image.new("L", (TS, TS), 0); ImageDraw.Draw(m).rounded_rectangle([0, 0, TS-1, TS-1], radius=20, fill=255)
        tile.putalpha(m)
        ImageDraw.Draw(tile).rounded_rectangle([1, 1, TS-2, TS-2], radius=20, outline=STEEL_DK+(255,), width=2)
        tile.alpha_composite(img.resize((132, 132), Image.LANCZOS), ((TS-132)//2, 8))
        td2 = ImageDraw.Draw(tile); td2.text((TS//2-len(name)*3, TS-22), name, fill=(190, 200, 214, 255))
        sheet.alpha_composite(tile, (x, y))
    sheet.convert("RGB").save(os.path.join(OUT, "contact_sheet.png"))
    print("wrote contact_sheet.png")


if __name__ == "__main__":
    main()
