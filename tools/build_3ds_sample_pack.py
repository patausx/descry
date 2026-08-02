#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

SR = 32000
MAGIC = 0x53335254  # 'TR3S'
VERSION = 3
MAX_CHOPS = 32
EMPTY = 0xFFFFFFFF

DL = Path('/home/filicide/Downloads')

@dataclass
class Slot:
    slot: int
    name: str
    src: Path
    root: int = 60
    chops: int = 0


def ffmpeg_pcm(src: Path) -> bytes:
    cmd = [
        'ffmpeg', '-v', 'error', '-y', '-i', str(src),
        '-ac', '1', '-ar', str(SR), '-sample_fmt', 's16', '-f', 's16le', '-'
    ]
    return subprocess.check_output(cmd)


def write_s16(dst: Path, pcm: bytes, root: int, chops_count: int) -> None:
    frames = len(pcm) // 2
    chops = [EMPTY] * MAX_CHOPS
    if chops_count > 0 and frames > 0:
        n = max(1, min(MAX_CHOPS, chops_count))
        for i in range(n):
            chops[i] = (i * frames) // n
    header = struct.pack('<IBBBBII', MAGIC, VERSION, 1, root & 0xFF, 0, 0, 0)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(header + struct.pack('<' + 'I' * MAX_CHOPS, *chops) + struct.pack('<I', 0) + pcm)


def safe_name(s: str) -> str:
    return re.sub(r'[^A-Za-z0-9._-]+', '_', s).strip('_')


def build_slots() -> list[Slot]:
    jungle = DL / 'drumkits/_wav/jungle'
    drum808 = DL / 'drumkits/_wav/808'
    amen = DL / 'rhythm-lab.com_amen_vol.1/WAV'
    slots: list[Slot] = []
    jungle_names = [
        'amen-brother.wav', 'think.wav', 'apache.wav', 'hot-pants.wav',
        'funky-drummer.wav', 'cold-sweat.wav', 'scorpio.wav', 'soul-pride.wav',
        'assembly-line.wav', 'sing-a-simple-song.wav', 'give-it-up-or-turnit-a-loose.wav', 'kick-back.wav',
    ]
    for i, fn in enumerate(jungle_names):
        slots.append(Slot(i, Path(fn).stem, jungle / fn, 60, 16))

    kit_names = [
        ('808-kick.wav', 36), ('808-snare.wav', 60), ('808-clap.wav', 60), ('808-rimshot.wav', 60),
        ('808-hat-closed.wav', 60), ('808-hat-open.wav', 60), ('808-cymbal.wav', 60), ('808-cowbell.wav', 60),
        ('808-clave.wav', 60), ('808-maraca.wav', 60), ('808-tom-lo.wav', 43), ('808-tom-mid.wav', 48),
        ('808-tom-hi.wav', 53), ('808-conga-lo.wav', 43), ('808-conga-mid.wav', 48), ('808-conga-hi.wav', 53),
    ]
    for j, (fn, root) in enumerate(kit_names, start=12):
        slots.append(Slot(j, Path(fn).stem, drum808 / fn, root, 0))

    amen_names = ['cw_amen01_175.wav', 'cw_amen02_165.wav', 'cw_amen05_158.wav', 'cw_amen10_135.wav']
    for j, fn in enumerate(amen_names, start=28):
        slots.append(Slot(j, Path(fn).stem, amen / fn, 60, 16))
    return slots


def main() -> None:
    ap = argparse.ArgumentParser(description='Build a 3DS descry sample slot pack from curated Downloads WAVs')
    ap.add_argument('-o', '--out', default='dist/3ds_sample_pack', help='output dir')
    args = ap.parse_args()
    out = Path(args.out)
    sd = out / '3ds/descry'
    wav = out / 'preview_wav'
    if out.exists():
        shutil.rmtree(out)
    sd.mkdir(parents=True)
    wav.mkdir(parents=True)

    manifest = []
    for s in build_slots():
        if not s.src.exists():
            raise FileNotFoundError(s.src)
        pcm = ffmpeg_pcm(s.src)
        dst = sd / f'sample_{s.slot:02d}.s16'
        write_s16(dst, pcm, s.root, s.chops)
        preview = wav / f'{s.slot:02d}_{safe_name(s.name)}.wav'
        subprocess.check_call([
            'ffmpeg', '-v', 'error', '-y', '-i', str(s.src),
            '-ac', '1', '-ar', str(SR), '-sample_fmt', 's16', str(preview)
        ])
        manifest.append({
            'slot': s.slot, 'name': s.name, 'source': str(s.src), 'root': s.root,
            'chops': s.chops, 'frames': len(pcm)//2,
            's16': str(dst.relative_to(out)), 'bytes': dst.stat().st_size,
        })

    (out / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    lines = ['# descry 3ds sample pack', '', 'Copy `3ds/descry/sample_XX.s16` to `sdmc:/3ds/descry/`.', '', '| slot | name | root | chops | seconds | source |', '|---:|---|---:|---:|---:|---|']
    for m in manifest:
        lines.append(f"| {m['slot']:02d} | `{m['name']}` | {m['root']} | {m['chops']} | {m['frames']/SR:.2f} | `{Path(m['source']).name}` |")
    (out / 'MANIFEST.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    total = sum((sd / f'sample_{i.slot:02d}.s16').stat().st_size for i in build_slots())
    print(f'wrote {len(manifest)} slots to {out} ({total/1024/1024:.2f} MiB)')

if __name__ == '__main__':
    main()
