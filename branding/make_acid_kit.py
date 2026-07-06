#!/usr/bin/env python3
# descry — FINAL brand kit, canonical colorway: ACID
#   acid yellow-green (0xC8E030) on olive-black (0x12140A)
# outputs:
#   branding/final/logo_acid.png       — square mark, big
#   branding/final/logo_acid_{n}px    — 96/48 tiles
#   branding/final/wordmark_acid.png   — full wordmark
#   branding/final/splash_400x240.png  — 3ds top-screen splash
#   assets/icon.png                    — 48x48 smdh icon (native 1px cells)
from PIL import Image
import importlib.util, os

HERE = os.path.dirname(os.path.abspath(__file__))
def load(name):
    spec = importlib.util.spec_from_file_location(name, f"{HERE}/{name}.py")
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m

ed = load("make_ed_final")     # ligature glyph + square render
wm = load("make_wordmark")     # composed wordmark grid + render

BG   = (0x12,0x14,0x0A)
BASE = (0xC8,0xE0,0x30)
def lighten(c,f): return tuple(min(255,int(x+(255-x)*f)) for x in c)
def darken(c,f):  return tuple(int(x*(1-f)) for x in c)
HI = lighten(BASE, 0.45)
SH = darken(BASE, 0.30)

OUT = f"{HERE}/final"
os.makedirs(OUT, exist_ok=True)

# ---- logo tiles ----
ed.render(BG, BASE, HI, SH, cellpx=8, out=f"{OUT}/logo_acid.png")       # 320px
ed.render(BG, BASE, HI, SH, cellpx=2, out=f"{OUT}/logo_acid_80px.png")  # 80px

# ---- wordmark ----
wm.render(BG, BASE, HI, SH, cellpx=6, margin=4, out=f"{OUT}/wordmark_acid.png")

# ---- 48x48 smdh icon: glyph at native 1px cells, centered ----
def make_icon(path, size=48):
    im = Image.new("RGB", (size, size), BG)
    px = im.load()
    gw, gh = ed.GW, ed.GH                       # 21 x 34
    ox, oy = (size - gw)//2, (size - gh)//2
    for gy in range(gh):
        for gx in range(gw):
            if not ed.cell(gx, gy): continue
            lit    = not ed.cell(gx, gy-1) or not ed.cell(gx-1, gy)
            shaded = not ed.cell(gx, gy+1) or not ed.cell(gx+1, gy)
            c = HI if (lit and not shaded) else SH if (shaded and not lit) else BASE
            px[ox+gx, oy+gy] = c
    im.save(path)
    return im

make_icon(f"{OUT}/icon_48.png")
make_icon(f"{HERE}/../assets/icon.png")

# ---- top-screen splash 400x240: wordmark centered, cellpx=4 ----
def make_splash(path, W=400, H=240, cellpx=4):
    im = Image.new("RGB", (W, H), BG)
    px = im.load()
    gw, gh = wm.GW, wm.GH                       # 88 x 42 cells
    ox = (W - gw*cellpx)//2
    oy = (H - gh*cellpx)//2
    for gy in range(gh):
        for gx in range(gw):
            if not wm.cell(gx, gy): continue
            lit    = not wm.cell(gx, gy-1) or not wm.cell(gx-1, gy)
            shaded = not wm.cell(gx, gy+1) or not wm.cell(gx+1, gy)
            c = HI if (lit and not shaded) else SH if (shaded and not lit) else BASE
            X, Y = ox+gx*cellpx, oy+gy*cellpx
            for dy in range(cellpx):
                for dx in range(cellpx):
                    px[X+dx, Y+dy] = c
    im.save(path)

make_splash(f"{OUT}/splash_400x240.png")
print("acid kit done ->", OUT, "+ assets/icon.png")
