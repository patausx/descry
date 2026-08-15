#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import json
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

SR = 32000
MAGIC = 0x53335254  # 'TR3S'
VERSION = 3
MAX_CHOPS = 32
EMPTY = 0xFFFFFFFF

BMT_ZIP = Path('/home/filicide/Downloads/Blu Mar Ten - Blu Mar Ten - Jungle Jungle - 1989 to 1999 Samplepack.zip')

@dataclass
class Slot:
    slot: int
    name: str
    member_contains: str
    root: int = 60
    chops: int = 0


def safe_name(s: str) -> str:
    return re.sub(r'[^A-Za-z0-9._-]+', '_', s).strip('_')


def ffmpeg_pcm(src: Path) -> bytes:
    return subprocess.check_output([
        'ffmpeg', '-v', 'error', '-y', '-i', str(src),
        '-ac', '1', '-ar', str(SR), '-sample_fmt', 's16', '-f', 's16le', '-'
    ])


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


def slots() -> list[Slot]:
    # 31 add-on slots; sample slot 63 belongs to the shipped IRONLUNG demo.
    return [
        # breaks / amen variants
        Slot(32, 'bmt_let_there_break', 'Breaks/Let There Break.wav', 60, 16),
        Slot(33, 'bmt_shoes_break', 'Breaks/Shoes Break - 4A.wav', 60, 16),
        Slot(34, 'bmt_right_break', 'Breaks/Right Break - 6A.wav', 60, 16),
        Slot(35, 'bmt_jazz_note_break', 'Breaks/Jazz Note Break - 10A.wav', 60, 16),
        Slot(36, 'bmt_drumz_amen', 'Breaks/Drumz Amen', 60, 16),
        Slot(37, 'bmt_wheel_up_amen', 'Breaks/Wheel Up Amen', 60, 16),
        Slot(38, 'bmt_futureproof_amen', 'Breaks/Futureproof Amen', 60, 16),
        Slot(39, 'bmt_good_times_break', 'Breaks/Good Times Break', 60, 16),
        Slot(40, 'bmt_mystical_break', 'Breaks/Mystical Break', 60, 16),
        Slot(41, 'bmt_control_break', 'Breaks/Control Break', 60, 16),
        Slot(42, 'bmt_firin_line_break', 'Breaks/Firin Line Break', 60, 16),
        Slot(43, 'bmt_fairlight_break', 'Breaks/Fairlight Break', 60, 16),
        # bass / subs
        Slot(44, 'bmt_london_sub', 'Bass/London Sub - 1A.wav', 36, 0),
        Slot(45, 'bmt_wheel_up_sub', 'Bass/Wheel Up Sub.wav', 36, 0),
        Slot(46, 'bmt_what_ya_gonna_sub', 'Bass/What Ya Gonna Sub - 1A.wav', 36, 0),
        Slot(47, 'bmt_london_smooth_sub', 'Bass/London Smooth Sub.wav', 36, 0),
        Slot(48, 'bmt_worries_sub', 'Bass/Worries Sub - 5A.wav', 36, 0),
        Slot(49, 'bmt_bass_hit', 'Bass/', 36, 0),
        # stabs / hits
        Slot(50, 'bmt_far_out_stab', 'FX/Far Out Stab - 4A.wav', 60, 0),
        Slot(51, 'bmt_control_stab_2', 'Riffs, Arps & Hits/Control Stab 2 - 6B.wav', 60, 0),
        Slot(52, 'bmt_knives_stab', 'Riffs, Arps & Hits/Knives Stab - 10A.wav', 60, 0),
        Slot(53, 'bmt_tip_stab', 'Riffs, Arps & Hits/Tip Stab - 11B.wav', 60, 0),
        Slot(54, 'bmt_fire_stab', 'Riffs, Arps & Hits/Fire Stab - 2B.wav', 60, 0),
        Slot(55, 'bmt_darkness_stabs', 'Riffs, Arps & Hits/Darkness Stabs - 7A.wav', 60, 0),
        Slot(56, 'bmt_times_stab', 'Riffs, Arps & Hits/Times Stab - 5B.wav', 60, 0),
        Slot(57, 'bmt_technology_stab', 'Riffs, Arps & Hits/Technology Ana Stab - 5A.wav', 60, 0),
        # pads / atmos / texture
        Slot(58, 'bmt_dreams_pad', 'Riffs, Arps & Hits/Dreams Pad - 11A.wav', 60, 8),
        Slot(59, 'bmt_little_roller_pad', 'Riffs, Arps & Hits/Little Roller Sharp Pad - 3A.wav', 60, 8),
        Slot(60, 'bmt_night_train_pad', 'Pads/Night Train Pad - 9B.wav', 60, 8),
        Slot(61, 'bmt_soul_atmos', 'FX/Soul Atmos - 7A.wav', 60, 8),
        # Slot 63 is permanently reserved for the shipped IRONLUNG break.
        Slot(62, 'bmt_stranger_atmos', 'FX/Stranger Atmos - 7A.wav', 60, 8),
    ]


def load_inner_zip() -> zipfile.ZipFile:
    with zipfile.ZipFile(BMT_ZIP) as outer:
        inners = [n for n in outer.namelist() if n.lower().endswith('.zip')]
        if not inners:
            raise RuntimeError('no inner zip in Blu Mar Ten archive')
        data = outer.read(inners[0])
    return zipfile.ZipFile(io.BytesIO(data))


def find_member(z: zipfile.ZipFile, needle: str) -> str:
    needle_l = needle.lower()
    candidates = [n for n in z.namelist() if n.lower().endswith('.wav') and not n.startswith('__MACOSX/')]
    exact = [n for n in candidates if needle_l in n.lower()]
    if not exact:
        raise FileNotFoundError(f'no member containing {needle!r}')
    # Prefer non AppleDouble, shortest matching path for broad needles like Bass/.
    exact.sort(key=lambda n: (len(n), n))
    return exact[0]


def main() -> None:
    ap = argparse.ArgumentParser(description='Build descry BMT add-on slots 32-62 (slot 63 reserved for IRONLUNG)')
    ap.add_argument('-o', '--out', default='dist/3ds_sample_pack_bmt_32_62')
    args = ap.parse_args()
    out = Path(args.out)
    sd = out / '3ds/descry'
    preview = out / 'preview_wav'
    if out.exists():
        shutil.rmtree(out)
    sd.mkdir(parents=True)
    preview.mkdir(parents=True)

    manifest = []
    with load_inner_zip() as z, tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for s in slots():
            member = find_member(z, s.member_contains)
            tmp = tmpdir / (safe_name(Path(member).stem) + '.wav')
            tmp.write_bytes(z.read(member))
            pcm = ffmpeg_pcm(tmp)
            dst = sd / f'sample_{s.slot:02d}.s16'
            write_s16(dst, pcm, s.root, s.chops)
            prev = preview / f'{s.slot:02d}_{safe_name(s.name)}.wav'
            subprocess.check_call([
                'ffmpeg', '-v', 'error', '-y', '-i', str(tmp),
                '-ac', '1', '-ar', str(SR), '-sample_fmt', 's16', str(prev)
            ])
            manifest.append({
                'slot': s.slot,
                'name': s.name,
                'member': member,
                'root': s.root,
                'chops': s.chops,
                'frames': len(pcm) // 2,
                'seconds': round((len(pcm) // 2) / SR, 3),
                'bytes': dst.stat().st_size,
            })

    (out / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    lines = ['# Blu Mar Ten descry add-on slots 32-62', '', 'Slot `63` is reserved for the shipped IRONLUNG demo. Upload `sample_32.s16`..`sample_62.s16` only.', '', '| slot | name | chops | seconds | source |', '|---:|---|---:|---:|---|']
    for m in manifest:
        lines.append(f"| {m['slot']:02d} | `{m['name']}` | {m['chops']} | {m['seconds']:.2f} | `{m['member']}` |")
    (out / 'MANIFEST.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    total = sum(m['bytes'] for m in manifest)
    print(f'wrote slots 32-62 to {out} ({total/1024/1024:.2f} MiB); slot 63 reserved for IRONLUNG')

if __name__ == '__main__':
    main()
