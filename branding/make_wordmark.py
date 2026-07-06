#!/usr/bin/env python3
# descry — full wordmark: user's "de" ligature (exact shape, untouched)
# + hand-pixeled s c r y in the same style (stroke 4/3, chamfered corners).
# baseline-aligned, bevel lighting, colorways from make_ed_final.
from PIL import Image

# ---- the ligature, scanned 1:1 from the user's sketch (21 x 34) ----
LIG = """
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

# ---- letters: x-height 18 rows, same stroke/chamfer language ----
S = """
..##########..
.############.
####......####
####......####
####..........
####..........
#####.........
.#########....
..###########.
.....#########
..........####
..........####
..........####
####......####
####......####
####......####
.############.
..##########..
""".strip().split("\n")

C = """
..##########..
.############.
####......####
####......####
####..........
####..........
####..........
####..........
####..........
####..........
####..........
####..........
####..........
####..........
####......####
####......####
.############.
..##########..
""".strip().split("\n")

R = """
####.#######.
#############
#####....####
.........####
.........####
.........####
####.........
####.........
####.........
####.........
####.........
####.........
####.........
####.........
####.........
####.........
####.........
####.........
""".strip().split("\n")

Y = """
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
####......####
.#############
..############
...###########
..........####
..........####
..........####
..........####
..........####
....##########
..############
..###########.
""".strip().split("\n")

XH_ROW  = 16          # ligature row where x-height starts
BASE_ROW = 33         # ligature baseline (last body row)
DESC = 8              # y descender rows below baseline
GAP = 3               # letter spacing (cells)

def glyph_wh(g): return len(g[0]), len(g)

def compose():
    letters = [(LIG, 0), (S, XH_ROW), (C, XH_ROW), (R, XH_ROW), (Y, XH_ROW)]
    W = sum(glyph_wh(g)[0] for g, _ in letters) + GAP * (len(letters) - 1)
    H = BASE_ROW + 1 + DESC
    grid = [[0]*W for _ in range(H)]
    x = 0
    for g, oy in letters:
        gw, gh = glyph_wh(g)
        for gy in range(gh):
            for gx in range(gw):
                if g[gy][gx] == "#":
                    grid[oy+gy][x+gx] = 1
        x += gw + GAP
    return grid, W, H

GRID, GW, GH = compose()

def cell(x, y):
    return 0 <= x < GW and 0 <= y < GH and GRID[y][x]

COLORWAYS = [
    ((0x31,0x34,0x32), (0xBC,0xB7,0xA5), (0xE8,0xE2,0xCC), (0x8a,0x86,0x77), "cream"),
    ((0xF2,0xEF,0xE6), (0x14,0x4d,0x52), (0x2e,0x6e,0x74), (0x0a,0x30,0x34), "teal"),
    ((0x14,0x16,0x15), (0x8c,0xa0,0x57), (0xc4,0xd8,0x8a), (0x55,0x66,0x33), "scope"),
]

def render(bg, base, hi, sh, cellpx=6, margin=4, out=None):
    w = (GW + 2*margin) * cellpx
    h = (GH + 2*margin) * cellpx
    im = Image.new("RGB", (w, h), bg)
    px = im.load()
    for gy in range(GH):
        for gx in range(GW):
            if not cell(gx, gy): continue
            lit    = not cell(gx, gy-1) or not cell(gx-1, gy)
            shaded = not cell(gx, gy+1) or not cell(gx+1, gy)
            c = hi if (lit and not shaded) else sh if (shaded and not lit) else base
            X, Y0 = (margin+gx)*cellpx, (margin+gy)*cellpx
            for dy in range(cellpx):
                for dx in range(cellpx):
                    px[X+dx, Y0+dy] = c
    if out: im.save(out)
    return im

OUT = "/home/filicide/descry/branding"
imgs = [render(*cw[:4], out=f"{OUT}/wordmark_{cw[4]}.png") for cw in COLORWAYS]

pad = 16
w = max(i.width for i in imgs) + 2*pad
h = pad + sum(i.height + pad for i in imgs)
sheet = Image.new("RGB", (w, h), (0x20,0x22,0x21))
y = pad
for im in imgs:
    sheet.paste(im, (pad, y)); y += im.height + pad
sheet.save(f"{OUT}/wordmark_sheet.png")
print("done:", GW, "x", GH, "cells")
