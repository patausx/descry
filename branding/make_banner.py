#!/usr/bin/env python3
# descry — github README banner, acid colorway
#   wordmark (from make_wordmark grid) + tagline set in the engine's own 6x8 font
# outputs: branding/final/banner.png (+ copy for docs/)
import importlib.util, os, re
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
def load(name):
    spec = importlib.util.spec_from_file_location(name, f"{HERE}/{name}.py")
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m

wm = load("make_wordmark")

BG   = (0x12,0x14,0x0A)
BASE = (0xC8,0xE0,0x30)
def lighten(c,f): return tuple(min(255,int(x+(255-x)*f)) for x in c)
def darken(c,f):  return tuple(int(x*(1-f)) for x in c)
HI  = lighten(BASE, 0.45)
SH  = darken(BASE, 0.30)
DIM = darken(BASE, 0.55)          # tagline: quieter than the mark

# ---- parse the engine's 6x8 font straight from core/ui/font.cpp ----
def load_font():
    src = open(f"{HERE}/../core/ui/font.cpp").read()
    src = src[src.index("font_6x8[96][8]"):]
    src = re.sub(r"R\(([01]),([01]),([01]),([01]),([01]),([01])\)",
                 lambda m: str(int("".join(m.groups()), 2)), src)
    blocks = re.findall(r"\{([^{}]*)\}", src[src.index("{")+1:])
    font = {}
    for i, b in enumerate(blocks[:96]):
        rows = [int(x) for x in re.findall(r"\d+", b)]
        rows += [0]*(8-len(rows))
        font[chr(0x20+i)] = rows
    return font

FONT = load_font()

def text_size(s): return len(s)*6, 8

def draw_text(px, s, ox, oy, color, scale=1):
    for ci, ch in enumerate(s.upper()):
        rows = FONT.get(ch, FONT[' '])
        for y in range(8):
            for x in range(6):
                if rows[y] & (1 << (5-x)):
                    X, Y = ox+(ci*6+x)*scale, oy+y*scale
                    for dy in range(scale):
                        for dx in range(scale):
                            px[X+dx, Y+dy] = color

# ---- compose at native cells, then integer-upscale ----
TAG = "A MUSIC TRACKER + SYNTHESIZER FOR THE NEW NINTENDO 3DS"

def make_banner(path, cellpx=6, scale_tag=2):
    gw, gh = wm.GW, wm.GH                       # wordmark cells
    mw, mh = gw*cellpx, gh*cellpx               # wordmark pixels
    tw, th = text_size(TAG)
    tw, th = tw*scale_tag, th*scale_tag
    pad_x, pad_top, gap, pad_bot = 48, 40, 22, 40
    W = max(mw, tw) + pad_x*2
    H = pad_top + mh + gap + th + gap + 2 + pad_bot

    im = Image.new("RGB", (W, H), BG)
    px = im.load()

    # wordmark, bevel-lit like the rest of the kit
    ox, oy = (W-mw)//2, pad_top
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

    # tagline in the tracker's own font
    ty = oy + mh + gap
    draw_text(px, TAG, (W-tw)//2, ty, DIM, scale_tag)

    # thin acid rule under the tagline
    ry = ty + th + gap
    for x in range(pad_x, W-pad_x):
        px[x, ry] = SH
        px[x, ry+1] = darken(BASE, 0.62)

    im.save(path)
    print(f"banner {W}x{H} -> {path}")
    return im

if __name__ == "__main__":
    out = f"{HERE}/final"
    os.makedirs(out, exist_ok=True)
    make_banner(f"{out}/banner.png")
