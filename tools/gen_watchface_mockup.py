"""ARGUS — custom watchface MOCKUP (design sign-off only, not firmware).

Renders the proposed watchface at the T-Watch Ultra's 410x502 in both brand states
so we can lock the layout before touching src/time_screen.cpp:
  - RESTING  : calm steel-blue accent (#9BBCD6)
  - THREAT   : HADES red (#DB615A) when Threat Radar / detect pipeline flags a tail
The accent (brand ring, wordmark, time glow, status pill) flips as one.
Outputs tools/icon_out/watchface_mockup.png (side-by-side).
"""
import os, math
from PIL import Image, ImageDraw, ImageFont, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon_out")
os.makedirs(OUT, exist_ok=True)
W, H = 410, 502

STEEL = (155, 188, 214); STEEL_HI = (206, 226, 242); STEEL_DK = (91, 124, 150)
RED = (219, 97, 90); RED_HI = (255, 150, 140); RED_DK = (120, 40, 38)
INK = (232, 238, 245); DIMTXT = (120, 140, 158)

def F(path, sz): return ImageFont.truetype(path, sz)
BAHN = "C:/Windows/Fonts/bahnschrift.ttf"; ARIALB = "C:/Windows/Fonts/arialbd.ttf"


def ctext(d, xy, s, font, fill, anchor="mm"):
    d.text(xy, s, font=font, fill=fill, anchor=anchor)


def render(accent, accent_hi, accent_dk, threat):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    # radial vignette background
    cx, cy = W // 2, 250
    for r in range(360, 0, -4):
        t = r / 360
        col = tuple(int((10, 14, 20)[k] * t + (4, 6, 9)[k] * (1 - t)) for k in range(3))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col + (255,))

    # --- status bar ---
    ctext(d, (26, 30), "ARGUS", F(BAHN, 30), accent_hi, anchor="lm")
    # radio state dots (WiFi / BT / GPS) + battery
    labels = [("WIFI", True), ("BT", True), ("GPS", False)]
    x = W - 150
    for name, on in labels:
        col = accent if on else (70, 78, 88)
        d.ellipse([x, 24, x + 12, 36], fill=col + (255,))
        ctext(d, (x + 18, 30), name, F(BAHN, 16), INK if on else DIMTXT, anchor="lm")
        x += 46
    ctext(d, (W - 26, 30), "82%", F(BAHN, 20), INK, anchor="rm")

    # --- brand ring (threat-aware), thin with a bright seconds arc ---
    R = 168
    ring = Image.new("RGBA", (W, H), (0, 0, 0, 0)); rd = ImageDraw.Draw(ring)
    rd.ellipse([cx - R, cy - R, cx + R, cy + R], outline=accent_dk + (255,), width=3)
    rd.arc([cx - R, cy - R, cx + R, cy + R], -90, 150, fill=accent + (255,), width=6)   # seconds
    ring = ring.filter(ImageFilter.GaussianBlur(0))
    glowring = ring.filter(ImageFilter.GaussianBlur(6))
    img.alpha_composite(glowring); img.alpha_composite(ring)
    # tick marks
    for a in range(0, 360, 30):
        ang = math.radians(a)
        x1 = cx + int((R - 14) * math.cos(ang)); y1 = cy + int((R - 14) * math.sin(ang))
        x2 = cx + int((R - 4) * math.cos(ang)); y2 = cy + int((R - 4) * math.sin(ang))
        d.line([x1, y1, x2, y2], fill=STEEL_DK + (180,), width=2)

    # --- time (big) + small AM span ---
    tglow = Image.new("RGBA", (W, H), (0, 0, 0, 0)); td = ImageDraw.Draw(tglow)
    td.text((cx, cy - 18), "10:42", font=F(ARIALB, 118), fill=accent + (255,), anchor="mm")
    img.alpha_composite(tglow.filter(ImageFilter.GaussianBlur(10)))
    ctext(d, (cx, cy - 18), "10:42", F(ARIALB, 118), INK)
    ctext(d, (cx + 150, cy - 58), "AM", F(BAHN, 30), accent_hi, anchor="mm")
    # date
    ctext(d, (cx, cy + 60), "SUN  ·  JUL 20", F(BAHN, 30), DIMTXT)

    # --- status pill (threat-aware) ---
    py = 452
    pill_w = 300 if threat else 210
    x0 = cx - pill_w // 2
    d.rounded_rectangle([x0, py - 30, x0 + pill_w, py + 30], radius=30,
                        fill=(accent_dk[0], accent_dk[1], accent_dk[2], 60),
                        outline=accent + (255,), width=3)
    label = "THREAT DETECTED" if threat else "SECURE"
    # small glyph: shield-check (secure) / alert-eye (threat)
    gx = x0 + 34
    if threat:
        d.ellipse([gx - 14, py - 12, gx + 14, py + 12], outline=accent + (255,), width=3)
        d.ellipse([gx - 4, py - 4, gx + 4, py + 4], fill=accent + (255,))
    else:
        d.line([gx - 12, py, gx - 3, py + 10, gx + 13, py - 12], fill=accent + (255,), width=4, joint="curve")
    ctext(d, (x0 + pill_w // 2 + 16, py), label, F(BAHN, 30), accent_hi)
    if threat:
        ctext(d, (cx, py + 52), "1 tail  ·  co-moving 4 min", F(BAHN, 20), RED_HI, anchor="mm")

    return img


def main():
    resting = render(STEEL, STEEL_HI, STEEL_DK, threat=False)
    threat = render(RED, RED_HI, RED_DK, threat=True)
    pad = 24
    sheet = Image.new("RGBA", (W * 2 + pad * 3, H + 80), (18, 18, 20, 255))
    sheet.alpha_composite(resting, (pad, 60)); sheet.alpha_composite(threat, (pad * 2 + W, 60))
    d = ImageDraw.Draw(sheet)
    d.text((pad + W // 2, 30), "RESTING (steel-blue)", font=F(BAHN, 26), fill=(200, 210, 220, 255), anchor="mm")
    d.text((pad * 2 + W + W // 2, 30), "THREAT (HADES red)", font=F(BAHN, 26), fill=(230, 160, 150, 255), anchor="mm")
    sheet.convert("RGB").save(os.path.join(OUT, "watchface_mockup.png"))
    print("wrote watchface_mockup.png")


if __name__ == "__main__":
    main()
