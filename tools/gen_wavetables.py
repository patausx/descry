#!/usr/bin/env python3
# generate a starter pack of single-cycle wavetables for descry
# (16-bit mono wav, 600 frames each - AKWF-compatible format).
# focus on timbres NOT covered by the builtin shapes: organ, formant, metal.
import math, struct, os

OUT = "wavetables"
N = 600  # frames per cycle (AKWF standard-ish)

def write_wav(name, samples):
    path = os.path.join(OUT, name + ".wav")
    data = b"".join(struct.pack("<h", max(-32768, min(32767, int(s * 32767)))) for s in samples)
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(data), b"WAVE", b"fmt ", 16,
        1, 1, 32000, 64000, 2, 16, b"data", len(data))
    with open(path, "wb") as f:
        f.write(hdr + data)
    print(f"  {name}.wav")

def norm(s):
    peak = max(abs(min(s)), abs(max(s))) or 1.0
    return [v / peak * 0.98 for v in s]

os.makedirs(OUT, exist_ok=True)
T = [i / N for i in range(N)]  # 0..1 phase

# 01: organ - additive drawbar (1 + 2 + 3 + 4 harmonics, hammond-ish)
write_wav("01_organ", norm([
    math.sin(2*math.pi*t) + 0.6*math.sin(4*math.pi*t) +
    0.4*math.sin(6*math.pi*t) + 0.5*math.sin(8*math.pi*t) for t in T]))

# 02: bell partials (inharmonic-ish via detuned high partials folded into cycle)
write_wav("02_bell", norm([
    math.sin(2*math.pi*t) + 0.5*math.sin(2*math.pi*5.02*t if False else 10*math.pi*t) +
    0.35*math.sin(14*math.pi*t) + 0.2*math.sin(22*math.pi*t) for t in T]))

# 03: formant "ah" - resonant peaks around harmonics 3 and 8
write_wav("03_vox_ah", norm([
    sum(math.sin(2*math.pi*(k+1)*t) * (1.2 if k in (2,3) else (0.8 if k in (7,8) else 0.25/(k+1)))
        for k in range(16)) for t in T]))

# 04: formant "oo" - dark vowel, peaks at 2 and 5
write_wav("04_vox_oo", norm([
    sum(math.sin(2*math.pi*(k+1)*t) * (1.2 if k == 1 else (0.7 if k == 4 else 0.15/(k+1)))
        for k in range(12)) for t in T]))

# 05: metallic - odd harmonics with sharp rolloff + ring
write_wav("05_metal", norm([
    sum(math.sin(2*math.pi*(2*k+1)*t) / math.sqrt(2*k+1) * (1 if k < 9 else 0)
        for k in range(9)) + 0.3*math.sin(34*math.pi*t) for t in T]))

# 06: pulse 25% (a classic the builtins lack: only 50% there)
write_wav("06_pulse25", norm([1.0 if t < 0.25 else -1.0 for t in T]))

# 07: pulse 12.5% (nes-style narrow)
write_wav("07_pulse12", norm([1.0 if t < 0.125 else -1.0 for t in T]))

# 08: half sine (rectified) - warm hollow
write_wav("08_halfsin", norm([math.sin(2*math.pi*t) if t < 0.5 else 0.0 for t in T]))

# 09: saw+oct - saw with strong octave-up layer
write_wav("09_sawoct", norm([
    (2*t - 1) + 0.6*(2*((2*t) % 1) - 1) for t in T]))

# 10: fm-ish 2-op bright (carrier + mod folded in)
write_wav("10_fmbright", norm([
    math.sin(2*math.pi*t + 2.2*math.sin(6*math.pi*t)) for t in T]))

# 11: fm-ish growl (low ratio, heavy index)
write_wav("11_fmgrowl", norm([
    math.sin(2*math.pi*t + 3.5*math.sin(2*math.pi*t)) for t in T]))

# 12: soft triangle-fold (west-coast flavor)
write_wav("12_fold", norm([
    math.sin(math.pi * 1.8 * math.sin(2*math.pi*t)) for t in T]))

print("done ->", OUT)
