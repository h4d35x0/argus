"""HD remaster of all 5 HexHound stages - same silhouettes/palette as the original
HexHound sprites.h, redrawn with shading + detail + neon bloom. Supersampled then
downscaled. This is the GENERATOR for the on-watch pet art.

USAGE:
  pip install Pillow
  python tools/gen_hexhound_sprites.py
  python tools/gen_hexhound_sprites.py --asset-size 400
Outputs into tools/hexhound_out/:
  - hd_<stage>.png       opaque previews (gallery)
  - asset_<stage>.png    200px TRANSPARENT sprites for the watch
  - asset_<stage>_<N>.png  a non-default --asset-size, kept beside the 200px set

--asset-size only changes the FINAL downsample. Everything is drawn on the same
960x960 supersampled canvas, so 400 is real detail, not an upscale of the 200.
The default run is byte-identical to before this option existed: the size is only
appended to the filename when it is not 200, and the Tools-tile pup icon (which is
derived from the 200px asset) is written only on a default run.
Deploy: copy the five asset_<stage>.png onto the SD card as
  /HexHound/egg.png pup.png beast.png gremlin.png sentinel.png
(pet_screen.cpp loads A:/HexHound/<stage>.png per evolution stage). The Tools-tile
Packet Pup icon (pup_icon.png) is the alpha-cropped asset_pup.png (see this file's
tail / the tools/ note). Tweak a stage by editing its function below and re-running.
"""
import os, base64, argparse
from PIL import Image, ImageDraw, ImageFilter, ImageChops

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hexhound_out")
os.makedirs(OUT, exist_ok=True)

BG = (6, 11, 17)
SS = 4
W = H = 240
CW = CH = W * SS

_ap = argparse.ArgumentParser(description="Render the HexHound HD stage set.")
_ap.add_argument("--asset-size", type=int, default=200,
                 help="edge length of the transparent asset_<stage> PNGs "
                      "(default 200; the supersampled canvas is CW either way)")
ASSET_SIZE = _ap.parse_args().asset_size
if not 8 <= ASSET_SIZE <= CW:
    raise SystemExit(f"--asset-size must be between 8 and {CW}, the canvas edge; "
                     "anything larger would be an upscale, not more detail")

def asset_path(name):
    # The default keeps its historic name so a plain re-run overwrites the same
    # committed files; any other size lands beside it rather than on top of it.
    stem = f"asset_{name}" if ASSET_SIZE == 200 else f"asset_{name}_{ASSET_SIZE}"
    return os.path.join(OUT, f"{stem}.png")

# palette
ARM_D=(16,18,23); ARM_M=(40,45,54); ARM_L=(84,92,107); RIM=(150,162,182)
FUR_D=(20,22,27); FUR_M=(46,50,60); FUR_L=(92,100,116)
VIS=(0,170,255); VIS_CORE=(190,240,255); CY=(0,229,255)
GRN=(60,255,110); GRN_D=(20,120,50); AMB=(255,176,32); AMB_D=(150,90,0)
RED=(255,66,58); RED_D=(120,26,24); DB=(30,60,150)

def sc(*v): return tuple(int(x*SS) for x in v)
def new_canvas(): return Image.new("RGBA",(CW,CH),(0,0,0,0))
def ellipse_mask(box):
    m=Image.new("L",(CW,CH),0); ImageDraw.Draw(m).ellipse(box,fill=255); return m
def rrect_mask(box,r):
    m=Image.new("L",(CW,CH),0); ImageDraw.Draw(m).rounded_rectangle(box,radius=r,fill=255); return m
def poly_mask(pts):
    m=Image.new("L",(CW,CH),0); ImageDraw.Draw(m).polygon(pts,fill=255); return m
def vgrad(y0,y1,top,bot):
    g=Image.new("RGBA",(CW,CH),(0,0,0,0)); px=g.load(); h=max(1,y1-y0)
    for y in range(max(0,y0),min(CH,y1)):
        t=(y-y0)/h
        c=(int(top[0]+(bot[0]-top[0])*t),int(top[1]+(bot[1]-top[1])*t),int(top[2]+(bot[2]-top[2])*t),255)
        for x in range(CW): px[x,y]=c
    return g
def shaded(mask, y0,y1, top, bot):
    g=vgrad(sc(y0)[0] if False else y0*SS, y1*SS, top, bot)
    out=new_canvas(); out.paste(g,(0,0),mask); return out
def fill(mask,color):
    out=new_canvas(); out.paste(Image.new("RGBA",(CW,CH),color+(255,)),(0,0),mask); return out
def rim_light(mask, color, offset=3):
    # thin bright edge along the top of a mask via shifting
    edge=ImageChops.subtract(mask, ImageChops.offset(mask,0,offset*SS))
    return fill(edge,color)

def finalize(name, canvas, glow_list):
    glow=new_canvas()
    for mask,color in glow_list:
        glow=ImageChops.lighter(glow, fill(mask,color))
    bloom=glow.filter(ImageFilter.GaussianBlur(radius=9*SS))
    # opaque render for the preview gallery
    out=Image.new("RGBA",(CW,CH),BG+(255,))
    out.alpha_composite(bloom); out.alpha_composite(bloom); out.alpha_composite(canvas)
    out=out.convert("RGB").resize((W,H),Image.LANCZOS)
    out.save(os.path.join(OUT,f"hd_{name}.png"))
    # TRANSPARENT watch asset (creature + glow over transparency, so it floats
    # over the sonar rings). ASSET_SIZE px, palette-quantized PNG to keep it tiny.
    trn=new_canvas(); trn.alpha_composite(bloom); trn.alpha_composite(canvas)
    trn=trn.resize((ASSET_SIZE,ASSET_SIZE),Image.LANCZOS)
    trn.save(asset_path(name))
    return out

# ---------------- EGG ----------------
def egg():
    c=new_canvas()
    body=ellipse_mask(sc(120-52,120-64,120+52,120+72))
    c.alpha_composite(shaded(body,56,192,ARM_L,ARM_D))
    c.alpha_composite(rim_light(body,RIM,4))
    # circuit traces (dim electric blue)
    tr=Image.new("L",(CW,CH),0); d=ImageDraw.Draw(tr)
    pts=[(120,70),(120,100),(96,120),(96,150),(120,100),(144,124),(144,158)]
    d.line(sc(*sum(([p[0],p[1]] for p in pts),[])),fill=255,width=2*SS,joint="curve")
    for nx,ny in [(96,150),(144,158),(120,70),(96,120)]:
        d.ellipse(sc(nx-4,ny-4,nx+4,ny+4),fill=255)
    trace=new_canvas(); trace.paste(Image.new("RGBA",(CW,CH),DB+(255,)),(0,0),tr)
    c.alpha_composite(trace)
    # hairline crack with cyan leak near top
    cr=Image.new("L",(CW,CH),0); dc=ImageDraw.Draw(cr)
    dc.line(sc(112,78,120,92,114,104,124,116),fill=255,width=2*SS,joint="curve")
    crack=new_canvas(); crack.paste(Image.new("RGBA",(CW,CH),CY+(255,)),(0,0),cr); c.alpha_composite(crack)
    return finalize("egg",c,[(tr,DB),(cr,CY)])

# ---------------- PACKET PUP (sitting wolf) ----------------
def pup():
    c=new_canvas(); glow=[]
    # tail (behind)
    tail=poly_mask(sc(150,150, 178,140, 186,176, 156,178))
    c.alpha_composite(shaded(tail,138,180,FUR_M,FUR_D))
    # body (sitting haunch)
    body=poly_mask(sc(92,120, 148,120, 158,196, 82,196))
    c.alpha_composite(shaded(body,116,198,FUR_L,FUR_D))
    # chest lighter patch
    chest=ellipse_mask(sc(104,150,136,196)); c.alpha_composite(shaded(chest,150,196,FUR_L,FUR_M))
    # front legs + paws
    for cx in (104,136):
        leg=rrect_mask(sc(cx-11,168,cx+11,204),8*SS); c.alpha_composite(shaded(leg,166,206,FUR_M,FUR_D))
    # ears (pointed, behind head)
    for pts in [sc(84,60,100,44,108,84), sc(156,60,140,44,132,84)]:
        e=poly_mask(pts); c.alpha_composite(shaded(e,44,86,FUR_M,FUR_D))
    # head
    head=ellipse_mask(sc(92,58,148,116)); c.alpha_composite(shaded(head,56,118,FUR_L,FUR_M))
    c.alpha_composite(rim_light(head,RIM,3))
    # snout
    sn=rrect_mask(sc(108,96,132,120),9*SS); c.alpha_composite(shaded(sn,94,120,FUR_M,FUR_D))
    nose=ellipse_mask(sc(113,110,127,122)); c.alpha_composite(fill(nose,(8,10,14)))
    # cyan eyes
    for cx in (108,132):
        ey=ellipse_mask(sc(cx-8,80,cx+8,96)); c.alpha_composite(fill(ey,CY)); glow.append((ey,CY))
        p=ellipse_mask(sc(cx-3,84,cx+3,94)); c.alpha_composite(fill(p,(6,10,20)))
    return finalize("pup",c,glow)

# ---------------- BEACON BEAST (horned, antenna, blue eyes) ----------------
def beast():
    c=new_canvas(); glow=[]
    # antenna stalk + glowing tip
    stalk=rrect_mask(sc(116,40,124,74),4*SS); c.alpha_composite(shaded(stalk,40,74,ARM_L,ARM_M))
    tip=ellipse_mask(sc(108,26,132,50)); c.alpha_composite(fill(tip,CY)); glow.append((tip,CY))
    # horns
    for pts in [sc(86,84,74,52,102,76), sc(154,84,166,52,138,76)]:
        h=poly_mask(pts); c.alpha_composite(shaded(h,50,86,ARM_L,ARM_D))
    # bulky body (wide angular)
    body=poly_mask(sc(74,110, 166,110, 150,200, 90,200))
    c.alpha_composite(shaded(body,106,202,ARM_L,ARM_D)); c.alpha_composite(rim_light(body,RIM,4))
    # head plate
    head=poly_mask(sc(88,70,152,70,146,120,94,120)); c.alpha_composite(shaded(head,68,122,ARM_L,ARM_M))
    # legs
    for cx in (104,136):
        leg=rrect_mask(sc(cx-15,186,cx+15,214),8*SS); c.alpha_composite(shaded(leg,184,216,ARM_M,ARM_D))
    # amber accent vents (stage color)
    for y in (150,164,178):
        v=rrect_mask(sc(104,y,136,y+5),3*SS); c.alpha_composite(fill(v,AMB)); glow.append((v,AMB))
    # electric-blue angular eyes
    for pts_ in [sc(98,88,120,92,116,104,98,100), sc(142,88,120,92,124,104,142,100)]:
        ey=poly_mask(pts_); c.alpha_composite(fill(ey,VIS)); glow.append((ey,VIS))
    return finalize("beast",c,glow)

# ---------------- GREMLIN (big head imp, amber eyes, green grin) ----------------
def gremlin():
    c=new_canvas(); glow=[]
    # thin body + legs (slight lean = shifty stance)
    body=poly_mask(sc(106,132,138,132,146,196,100,196)); c.alpha_composite(shaded(body,130,198,FUR_M,FUR_D))
    for cx in (112,132):
        l=rrect_mask(sc(cx-9,184,cx+9,214),6*SS); c.alpha_composite(shaded(l,182,216,FUR_M,FUR_D))
    # left arm low/relaxed
    la=rrect_mask(sc(86,142,101,186),6*SS); c.alpha_composite(shaded(la,140,188,FUR_M,FUR_D))
    # RIGHT arm raised + bent, clawed hand up by the grin (scheming)
    upper=poly_mask(sc(140,144,158,140,154,120,138,126)); c.alpha_composite(shaded(upper,118,146,FUR_M,FUR_D))
    fore =poly_mask(sc(138,126,154,120,150,102,138,108)); c.alpha_composite(shaded(fore,100,128,FUR_M,FUR_D))
    for fx,fy in ((140,100),(146,102),(151,106)):   # three thin claw fingers
        cl=poly_mask(sc(fx-2,fy, fx+2,fy, fx,fy-12)); c.alpha_composite(shaded(cl,fy-12,fy,FUR_L,FUR_M))
    # big pointy ears (one flicked back, jagged)
    for pts in [sc(80,116,48,48,112,94), sc(160,114,196,64,128,96)]:
        e=poly_mask(pts); c.alpha_composite(shaded(e,48,118,FUR_L,FUR_D)); c.alpha_composite(rim_light(e,RIM,3))
    # big head (tilted a hair)
    head=ellipse_mask(sc(78,54,162,138)); c.alpha_composite(shaded(head,52,140,FUR_L,FUR_M))
    c.alpha_composite(rim_light(head,RIM,4))
    # slanted amber eyes (almond, angled inward-down = sly), asymmetric
    left_eye = poly_mask(sc(92,94, 118,86, 120,96, 96,104))
    right_eye= poly_mask(sc(150,84, 124,90, 122,101, 148,97))
    for ey in (left_eye,right_eye): c.alpha_composite(fill(ey,AMB)); glow.append((ey,AMB))
    for cx,cy in ((110,95),(134,93)):               # side-eyeing pupils + glint
        c.alpha_composite(fill(ellipse_mask(sc(cx-4,cy-3,cx+4,cy+5)),(30,16,0)))
        c.alpha_composite(fill(ellipse_mask(sc(cx,cy-3,cx+3,cy)),(255,244,210)))
    # angled dark brows (V toward center = scheming)
    br=Image.new("L",(CW,CH),0); db=ImageDraw.Draw(br)
    db.line(sc(88,82,120,92),fill=255,width=6*SS); db.line(sc(152,80,124,92),fill=255,width=6*SS)
    c.alpha_composite(fill(br,FUR_D))
    # lopsided green smirk (rises on the right)
    gr=Image.new("L",(CW,CH),0); ImageDraw.Draw(gr).line(
        sc(100,116, 114,126, 130,127, 146,114),fill=255,width=5*SS,joint="curve")
    grin=new_canvas(); grin.paste(Image.new("RGBA",(CW,CH),GRN+(255,)),(0,0),gr)
    c.alpha_composite(grin); glow.append((gr,GRN))
    # sharp fangs poking down over the smirk
    for fx,fy in ((112,122),(122,126),(132,125),(140,120)):
        c.alpha_composite(fill(poly_mask(sc(fx-3,fy, fx+3,fy, fx,fy+9)),(235,255,238)))
    return finalize("gremlin",c,glow)

# ---------------- SENTINEL (reuse) ----------------
def sentinel():
    c=new_canvas(); glow=[]
    for sgn in (-1,1):
        cx=120+sgn*30
        leg=rrect_mask(sc(cx-16,150,cx+16,214),10*SS); c.alpha_composite(shaded(leg,150,214,ARM_M,ARM_D))
        foot=rrect_mask(sc(cx-22,205,cx+20,220),6*SS); c.alpha_composite(shaded(foot,205,220,ARM_L,ARM_M))
        kn=rrect_mask(sc(cx-13,176,cx+13,186),5*SS); c.alpha_composite(fill(kn,RED)); glow.append((kn,RED))
    for sgn in (-1,1):
        pts=sc(120+sgn*26,70,120+sgn*78,74,120+sgn*70,116,120+sgn*30,110)
        pm=poly_mask(pts); c.alpha_composite(shaded(pm,66,120,RIM,ARM_D)); c.alpha_composite(rim_light(pm,RIM,3))
    for sgn in (-1,1):
        cx=120+sgn*60; arm=rrect_mask(sc(cx-14,108,cx+14,158),9*SS); c.alpha_composite(shaded(arm,108,158,ARM_M,ARM_D))
    torso=poly_mask(sc(120-46,96,120+46,96,120+40,168,120-40,168)); c.alpha_composite(shaded(torso,92,170,ARM_L,ARM_D))
    seams=Image.new("L",(CW,CH),0); d=ImageDraw.Draw(seams)
    d.line(sc(120,100,120,150),fill=255,width=2*SS); d.line(sc(90,120,150,120),fill=255,width=2*SS)
    c.alpha_composite(fill(seams,ARM_D))
    chev=Image.new("L",(CW,CH),0); ImageDraw.Draw(chev).line(sc(96,158,120,150,144,158),fill=255,width=3*SS,joint="curve")
    c.alpha_composite(fill(chev,RED)); glow.append((chev,RED))
    core=rrect_mask(sc(111,126,129,140),4*SS); c.alpha_composite(fill(core,CY)); glow.append((core,CY))
    head=poly_mask(sc(90,48,150,48,146,96,94,96)); c.alpha_composite(shaded(head,44,98,ARM_L,ARM_M))
    crest=rrect_mask(sc(116,40,124,54),3*SS); c.alpha_composite(shaded(crest,40,54,RIM,ARM_M))
    visor=rrect_mask(sc(98,66,142,78),6*SS); c.alpha_composite(fill(visor,VIS)); glow.append((visor,VIS))
    vc=rrect_mask(sc(100,69,140,75),4*SS); c.alpha_composite(fill(vc,VIS_CORE)); glow.append((vc,VIS_CORE))
    return finalize("sentinel",c,glow)

imgs={"egg":egg(),"pup":pup(),"beast":beast(),"gremlin":gremlin(),"sentinel":sentinel()}
print("rendered:", ", ".join(imgs))

# gallery
def b64(p): return base64.b64encode(open(os.path.join(OUT,p),'rb').read()).decode()
STAGES=[("egg","Egg","#8a8a8a","01"),("pup","Packet Pup","#00e5ff","02"),
        ("beast","Beacon Beast","#ffb020","03"),("gremlin","Gremlin Mode","#3dff6e","04"),
        ("sentinel","Sentinel","#ff423a","05")]
cells="\n".join(f'''<figure style="--a:{a}"><img src="data:image/png;base64,{b64(f"hd_{k}.png")}" alt="{lab}">
  <figcaption><span class="n">{n}</span> {lab}</figcaption></figure>''' for k,lab,a,n in STAGES)
html=f'''<title>HexHound - HD remaster set</title>
<style>
 :root{{--bg:#05090f;--line:#15304a;--ink:#d6ecff;--muted:#6f93b0;--cyan:#00e5ff;--mono:ui-monospace,Menlo,Consolas,monospace}}
 *{{box-sizing:border-box}} body{{margin:0;font-family:var(--mono);color:var(--ink);background:radial-gradient(1200px 600px at 50% -10%,#0a1826,var(--bg) 60%)}}
 .wrap{{max-width:900px;margin:0 auto;padding:40px 24px 64px}}
 .eyebrow{{color:var(--cyan);letter-spacing:.32em;text-transform:uppercase;font-size:.72rem;margin:0 0 10px}}
 h1{{margin:0;font-size:clamp(1.7rem,4vw,2.3rem)}} .lede{{color:var(--muted);max-width:62ch;line-height:1.6;margin:14px 0 26px}}
 .grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:18px}}
 figure{{margin:0;text-align:center}} figure img{{width:100%;max-width:200px;aspect-ratio:1;border-radius:12px;background:#060B11;border:1px solid var(--line)}}
 figcaption{{margin-top:9px;font-size:.82rem;color:var(--ink)}} .n{{color:var(--a);margin-right:6px}}
 figure img{{border-color:color-mix(in srgb,var(--a) 45%,var(--line))}}
 footer{{border-top:1px solid var(--line);margin-top:30px;padding-top:20px;color:var(--muted);font-size:.84rem;line-height:1.7}} footer b{{color:var(--ink)}}
</style>
<div class="wrap">
 <p class="eyebrow">ARGUS &middot; HexHound</p>
 <h1>HD remaster &mdash; full evolution set</h1>
 <p class="lede">All five stages remastered from your <code>sprites.h</code> designs: same silhouettes and
 stage-accent colors, now shaded with panel/fur detail and neon bloom. These render per evolution
 stage on the watch (SD-card assets), replacing the single procedural hound.</p>
 <div class="grid">{cells}</div>
 <footer><b>Next:</b> wire these into <code>pet_screen.cpp</code> so the pet evolves through them,
 keeping the idle bob + sonar rings, then flash. Flag any stage you want tweaked.</footer>
</div>'''
# newline="\n" so a run on Windows does not rewrite the committed file with CRLF.
# Without it every Windows run left this repo dirty with a 31-line diff that was
# pure carriage returns and no content change. The PNG writers were already safe;
# this was the one text output that was not.
open(os.path.join(OUT,"hexhound_hd_set.html"),"w",encoding="utf-8",
     newline="\n").write(html)
print("wrote hexhound_hd_set.html")

# Tools-tile Packet Pup icon: alpha-crop the pup asset to its content and pad to a
# square that clears the tile label. Deploy to the SD as /HexHound/pup_icon.png
# (loaded by draw_pet_icon() in tools_screen.cpp).
# It is derived from the 200px asset, so a non-default --asset-size skips it
# rather than rewriting a committed file from a source it was not cut from.
if ASSET_SIZE == 200:
    pup=Image.open(asset_path("pup")).convert("RGBA")
    crop=pup.crop(pup.getbbox()); crop.thumbnail((132,132),Image.LANCZOS)
    icon=Image.new("RGBA",(142,142),(0,0,0,0))
    icon.paste(crop,((142-crop.width)//2,(142-crop.height)//2),crop)
    icon.save(os.path.join(OUT,"pup_icon.png"))
    print("wrote pup_icon.png (Tools tile)")
else:
    print(f"asset size {ASSET_SIZE}: skipped pup_icon.png (it is cut from the 200px asset)")
