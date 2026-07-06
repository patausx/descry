#!/usr/bin/env python3
# descry — CIA banner assets (home menu)
#   banner_256x128.png        : acid wordmark on olive-black (opaque, classic)
#   banner_256x128_alpha.png  : transparent bg + dark outline (floats on home menu)
#   banner_preview_menu.png   : alpha variant composited on a home-menu-ish backdrop
#   jingles/jingle_*.wav      : ~2.5s candidates for the banner sound
import importlib.util, os, math, struct, random
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
HI = lighten(BASE, 0.45)
SH = darken(BASE, 0.30)
OUT = f"{HERE}/final"

# ---------------------------------------------------------------- banner imgs
def glyph_color(gx, gy):
    lit    = not wm.cell(gx, gy-1) or not wm.cell(gx-1, gy)
    shaded = not wm.cell(gx, gy+1) or not wm.cell(gx+1, gy)
    return HI if (lit and not shaded) else SH if (shaded and not lit) else BASE

def make_banner_img(path, W=256, H=128, cellpx=2):
    im = Image.new("RGB", (W, H), BG)
    px = im.load()
    gw, gh = wm.GW, wm.GH
    ox, oy = (W-gw*cellpx)//2, (H-gh*cellpx)//2
    for gy in range(gh):
        for gx in range(gw):
            if not wm.cell(gx, gy): continue
            c = glyph_color(gx, gy)
            X, Y = ox+gx*cellpx, oy+gy*cellpx
            for dy in range(cellpx):
                for dx in range(cellpx):
                    px[X+dx, Y+dy] = c
    im.save(path)
    print("banner (opaque) ->", path)

def make_banner_alpha(path, W=256, H=128, cellpx=2):
    im = Image.new("RGBA", (W, H), (0,0,0,0))
    px = im.load()
    gw, gh = wm.GW, wm.GH
    ox, oy = (W-gw*cellpx)//2, (H-gh*cellpx)//2
    outline = (*BG, 255)                      # olive-black rim, reads on white
    def put(gx, gy, c):
        X, Y = ox+gx*cellpx, oy+gy*cellpx
        if X < 0 or Y < 0 or X+cellpx > W or Y+cellpx > H: return
        for dy in range(cellpx):
            for dx in range(cellpx):
                px[X+dx, Y+dy] = c
    # pass 1: outline = any empty cell orthogonally adjacent to a filled one
    for gy in range(-1, gh+1):
        for gx in range(-1, gw+1):
            if wm.cell(gx, gy): continue
            if (wm.cell(gx+1,gy) or wm.cell(gx-1,gy) or
                wm.cell(gx,gy+1) or wm.cell(gx,gy-1)):
                put(gx, gy, outline)
    # pass 2: glyphs with bevel
    for gy in range(gh):
        for gx in range(gw):
            if wm.cell(gx, gy):
                put(gx, gy, (*glyph_color(gx, gy), 255))
    im.save(path)
    print("banner (alpha)  ->", path)
    return im

def make_menu_preview(banner, path):
    # home menu upper screen is a light grey gradient — sanity-check readability
    W, H = banner.size
    bgim = Image.new("RGBA", (W, H))
    p = bgim.load()
    for y in range(H):
        g = 208 + int(28*y/H)                 # #d0.. -> #ec..
        for x in range(W):
            p[x, y] = (g, g+2, g+6, 255)
    bgim.alpha_composite(banner)
    bgim.convert("RGB").save(path)
    print("menu preview    ->", path)

# ---------------------------------------------------------------- audio bits
SR = 32000
def render(total): return [0.0]*int(total*SR)

def add(mix, sig, t0=0.0, vel=1.0):
    o = int(t0*SR)
    for i, v in enumerate(sig):
        j = o+i
        if j < len(mix): mix[j] += v*vel

def fade_tail(mix, sec=0.5):
    n = int(sec*SR); N = len(mix)
    for i in range(n):
        mix[N-1-i] *= i/n

def write_wav(path, mix, gain=0.85):
    pk = max(1e-9, max(abs(x) for x in mix))
    data = b"".join(struct.pack("<h", int(max(-1,min(1,x/pk*gain))*32767)) for x in mix)
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36+len(data), b"WAVE",
                      b"fmt ", 16, 1, 1, SR, SR*2, 2, 16, b"data", len(data))
    open(path,"wb").write(hdr+data)
    print("jingle ->", path)

# --- voices ---
def saw_pluck(freq, dur, cutoff0=0.35, decay=7.0, fdecay=4.0):
    n = int(dur*SR); out = [0.0]*n
    phase, lp = 0.0, 0.0
    for i in range(n):
        t = i/SR
        phase += freq/SR
        s = 2.0*(phase - math.floor(phase+0.5))
        a = cutoff0*math.exp(-t*fdecay) + 0.02
        lp += a*(s-lp)
        out[i] = lp*math.exp(-t*decay)
    return out

def acid_note(f0, f1, dur, q=2.2, cut0=900.0, cutdec=6.0, acc=1.0):
    # 303-ish: square osc, chamberlin SVF lowpass w/ resonance, pitch slide
    n = int(dur*SR); out = [0.0]*n
    phase, low, band = 0.0, 0.0, 0.0
    for i in range(n):
        t = i/SR
        freq = f0*(f1/f0)**(min(1.0, t/max(dur,1e-6)))
        phase = (phase + freq/SR) % 1.0
        s = 1.0 if phase < 0.48 else -1.0
        fc = 60.0 + cut0*acc*math.exp(-t*cutdec)
        f = 2.0*math.sin(math.pi*min(0.45*SR, fc)/SR)
        low  += f*band
        high  = s - low - (1.0/q)*band
        band += f*high
        out[i] = low*math.exp(-t*3.0)
    return out

def fm_bell(freq, dur, ratio=3.5, index=2.2, idec=5.0, adec=2.2):
    n = int(dur*SR); out = [0.0]*n
    for i in range(n):
        t = i/SR
        mod = math.sin(2*math.pi*freq*ratio*t) * index*math.exp(-t*idec)
        out[i] = math.sin(2*math.pi*freq*t + mod) * math.exp(-t*adec)
    return out

def ks_pluck(freq, dur, damp=0.995):
    # karplus-strong
    n = int(dur*SR); N = max(2, int(SR/freq))
    buf = [random.uniform(-1,1) for _ in range(N)]
    out = [0.0]*n
    idx = 0
    for i in range(n):
        nxt = (idx+1) % N
        buf[idx] = damp*0.5*(buf[idx]+buf[nxt])
        out[i] = buf[idx]
        idx = nxt
    return out

def sub_riser(f0, f1, dur, decay=1.2):
    n = int(dur*SR); out = [0.0]*n
    phase = 0.0
    for i in range(n):
        t = i/SR
        freq = f0 + (f1-f0)*(t/dur)**0.5
        phase += freq/SR
        out[i] = math.sin(2*math.pi*phase)*math.exp(-t*decay)
    return out

def noise_sweep(dur, cut0=0.5, cutdec=6.0):
    n = int(dur*SR); out = [0.0]*n
    lp = 0.0
    rng = random.Random(7)
    for i in range(n):
        t = i/SR
        a = cut0*math.exp(-t*cutdec) + 0.005
        lp += a*(rng.uniform(-1,1)-lp)
        out[i] = lp*math.exp(-t*4.0)
    return out

# --- jingles (all rooted in D, ~2.5s, in descending order of "extra") ---
D = {"D2":73.42,"A2":110.0,"D3":146.83,"F#3":185.0,"A3":220.0,"C#4":277.18,
     "D4":293.66,"E4":329.63,"F#4":369.99,"A4":440.0,"C#5":554.37,"D5":587.33,
     "E5":659.26,"A5":880.0}

def j_arp(path):        # v1 — the current one: lydian saw arp
    mix = render(2.5)
    for i,(nm) in enumerate(["D3","F#3","A3","C#4","E4"]):
        add(mix, saw_pluck(D[nm], 2.5-0.11*i), 0.11*i, 0.55)
    add(mix, saw_pluck(D["D2"], 2.5, cutoff0=0.12, decay=1.2), 0, 0.5)
    fade_tail(mix, 0.6); write_wav(path, mix)

def j_acid(path):       # v2 — 303 slide, resonant, cheeky
    mix = render(2.5)
    add(mix, acid_note(D["D2"], D["D2"], 0.16, acc=1.4), 0.00, 0.9)
    add(mix, acid_note(D["D2"], D["D3"], 0.30, acc=1.0), 0.18, 0.8)  # slide up
    add(mix, acid_note(D["A2"], D["A2"], 0.14, acc=0.7), 0.52, 0.7)
    add(mix, acid_note(D["D3"], D["D2"], 0.9,  q=2.8, acc=1.2), 0.70, 0.9)  # long fall
    add(mix, fm_bell(D["D5"], 1.4, ratio=2.0, index=1.2), 0.70, 0.25)       # sparkle on top
    fade_tail(mix, 0.5); write_wav(path, mix)

def j_bell(path):       # v3 — fm bells, dreamy fifth
    mix = render(2.5)
    add(mix, fm_bell(D["D4"], 2.4), 0.00, 0.7)
    add(mix, fm_bell(D["A4"], 2.2), 0.22, 0.55)
    add(mix, fm_bell(D["D5"], 2.0, ratio=3.0, index=1.6), 0.44, 0.45)
    add(mix, sub_riser(D["D2"], D["D2"], 2.2, decay=1.0), 0.0, 0.35)
    fade_tail(mix, 0.6); write_wav(path, mix)

def j_strum(path):      # v4 — karplus Dmaj9 strum, warm guitar-ish
    random.seed(3)
    mix = render(2.5)
    for i,nm in enumerate(["D2","A2","D3","F#3","C#4","E4"]):
        add(mix, ks_pluck(D[nm], 2.5-0.045*i), 0.045*i, 0.55)
    fade_tail(mix, 0.6); write_wav(path, mix)

def j_boot(path):       # v5 — console boot: sub bloom + noise whoosh + high blip
    mix = render(2.5)
    add(mix, sub_riser(D["D2"]*0.5, D["D2"], 1.8, decay=1.4), 0.0, 0.9)
    add(mix, noise_sweep(1.2), 0.0, 0.30)
    add(mix, fm_bell(D["D5"], 1.5, ratio=2.0, index=1.0), 0.55, 0.4)
    add(mix, fm_bell(D["A5"], 1.3, ratio=2.0, index=0.8), 0.75, 0.3)
    fade_tail(mix, 0.5); write_wav(path, mix)

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_banner_img(f"{OUT}/banner_256x128.png")
    bn = make_banner_alpha(f"{OUT}/banner_256x128_alpha.png")
    make_menu_preview(bn, f"{OUT}/banner_preview_menu.png")
    jd = f"{OUT}/jingles"; os.makedirs(jd, exist_ok=True)
    j_arp  (f"{jd}/jingle_1_arp.wav")
    j_acid (f"{jd}/jingle_2_acid.wav")
    j_bell (f"{jd}/jingle_3_bell.wav")
    j_strum(f"{jd}/jingle_4_strum.wav")
    j_boot (f"{jd}/jingle_5_boot.wav")
    # keep the legacy name pointing at the currently chosen jingle
    import shutil
    shutil.copy(f"{jd}/jingle_5_boot.wav", f"{OUT}/banner_audio.wav")
