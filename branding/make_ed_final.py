#!/usr/bin/env python3
# descry — user's "ed" ligature, EXACT shape (scanned from his pixel sketch),
# rendered alive: square tile, edge-light bevel (top-left highlight /
# bottom-right shade), several colorways. shape is NOT altered.
from PIL import Image

# ---- the glyph, scanned 1:1 from the sketch (21 x 34 cells) ----
GLYPH = """
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.................####
.....##########..####
...##################
..#####......########
.####..........######
#####...........#####
#####............####
#####............####
#####............####
#####################
#####################
#####................
#####................
#####............####
#####............####
.####............####
..#####........######
...##################
....#################
......#########..####
""".strip().split("\n")

GH = len(GLYPH); GW = len(GLYPH[0])

def cell(x, y):
    if 0 <= x < GW and 0 <= y < GH:
        return GLYPH[y][x] == "#"
    return False

# ---- colorways: (bg, base, highlight, shadow, name) ----
COLORWAYS = [
    # descry cretaceous: cream glyph on dark slate
    ((0x31,0x34,0x32), (0xBC,0xB7,0xA5), (0xE8,0xE2,0xCC), (0x8a,0x86,0x77), "cream"),
    # user's teal, but on warm paper w/ depth
    ((0xF2,0xEF,0xE6), (0x14,0x4d,0x52), (0x2e,0x6e,0x74), (0x0a,0x30,0x34), "teal"),
    # scope phosphor: green glow on near-black
    ((0x14,0x16,0x15), (0x8c,0xa0,0x57), (0xc4,0xd8,0x8a), (0x55,0x66,0x33), "scope"),
    # dusk: rose glyph on deep slate
    ((0x26,0x28,0x27), (0xAC,0x90,0x86), (0xd4,0xb8,0xac), (0x6e,0x59,0x52), "rose"),
]

def render(bg, base, hi, sh, cellpx=8, out=None):
    # square canvas: side = max(GW,GH) + margins, glyph centered
    side = GH + 6                        # 40 cells
    ox = (side - GW) // 2                # center horizontally
    oy = (side - GH) // 2
    S = side * cellpx
    im = Image.new("RGB", (S, S), bg)
    px = im.load()
    for gy in range(GH):
        for gx in range(GW):
            if not cell(gx, gy): continue
            # bevel: lit if open above or left; shaded if open below or right
            lit    = not cell(gx, gy-1) or not cell(gx-1, gy)
            shaded = not cell(gx, gy+1) or not cell(gx+1, gy)
            c = hi if (lit and not shaded) else sh if (shaded and not lit) else base
            X, Y = (ox+gx)*cellpx, (oy+gy)*cellpx
            for dy in range(cellpx):
                for dx in range(cellpx):
                    px[X+dx, Y+dy] = c
    if out: im.save(out)
    return im

OUT = "/home/filicide/descry/branding"
imgs = []
for bg, base, hi, sh, name in COLORWAYS:
    imgs.append(render(bg, base, hi, sh, out=f"{OUT}/edfinal_{name}.png"))

# flat version too (no bevel) in descry palette, for reference
flat = render((0x31,0x34,0x32), (0xBC,0xB7,0xA5), (0xBC,0xB7,0xA5), (0xBC,0xB7,0xA5),
              out=f"{OUT}/edfinal_flat.png")

# contact sheet
pad = 16
w = pad + (len(imgs)+1) * (imgs[0].width + pad)
h = imgs[0].height + 2*pad
sheet = Image.new("RGB", (w, h), (0x20,0x22,0x21))
x = pad
for im in imgs + [flat]:
    sheet.paste(im, (x, pad)); x += im.width + pad
sheet.save(f"{OUT}/edfinal_sheet.png")
print("done:", ", ".join(c[4] for c in COLORWAYS), "+ flat")
