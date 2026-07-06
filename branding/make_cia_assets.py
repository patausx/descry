#!/usr/bin/env python3
# descry — CIA banner assets (home menu)
#   banner_256x128.png : acid wordmark, cellpx=2, centered
#   banner_audio.wav   : ~2.5s acid pluck arp jingle, 32000 Hz mono s16
import importlib.util, os, math, struct
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

# ---- banner image 256x128 ----
def make_banner_img(path, W=256, H=128, cellpx=2):
    im = Image.new("RGB", (W, H), BG)
    px = im.load()
    gw, gh = wm.GW, wm.GH
    ox, oy = (W-gw*cellpx)//2, (H-gh*cellpx)//2
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
    print("banner img ->", path)

# ---- banner audio: short acid arp, saw + 1-pole LP sweep + exp decay ----
SR = 32000
def note(freq, t0, dur, total, cutoff0=0.35, vel=1.0):
    n = int(dur*SR)
    out = [0.0]*int(total*SR)
    phase, lp = 0.0, 0.0
    for i in range(n):
        t = i/SR
        phase += freq/SR
        s = 2.0*(phase - math.floor(phase+0.5))          # saw
        env = math.exp(-t*7.0)
        a = cutoff0*math.exp(-t*4.0) + 0.02              # closing filter
        lp += a*(s-lp)
        j = int(t0*SR)+i
        if j < len(out): out[j] = lp*env*vel
    return out

def make_audio(path):
    total = 2.5
    # D lydian-ish arp: D3 F#3 A3 C#4 E4, quick 16ths then let ring
    seq = [(146.83,0.00),(185.00,0.11),(220.00,0.22),(277.18,0.33),(329.63,0.44)]
    mix = [0.0]*int(total*SR)
    for f,t0 in seq:
        for i,v in enumerate(note(f,t0,total-t0,total,vel=0.55)):
            mix[i]+=v
    # low D pad under it
    for i,v in enumerate(note(73.42,0.0,total,total,cutoff0=0.12,vel=0.5)):
        mix[i]+=v
    # gentle fade-out tail
    N=len(mix); fade=int(0.6*SR)
    for i in range(fade):
        mix[N-1-i]*= i/fade
    pk = max(abs(x) for x in mix)
    data = b"".join(struct.pack("<h", int(max(-1,min(1,x/pk*0.85))*32767)) for x in mix)
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36+len(data), b"WAVE",
                      b"fmt ", 16, 1, 1, SR, SR*2, 2, 16, b"data", len(data))
    open(path,"wb").write(hdr+data)
    print("banner audio ->", path)

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_banner_img(f"{OUT}/banner_256x128.png")
    make_audio(f"{OUT}/banner_audio.wav")
