#!/usr/bin/env python3
"""Build the actual descry user WAV library for sdmc:/3ds/descry/wav/.

All outputs are ordinary PCM16 mono 32 kHz WAV files. Directories stay below the
3DS browser's MAX_WAV_FILES=64 limit and filenames stay below its 39-char buffer.
"""
from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

# Sample packs and built libraries live outside the repo (third-party audio).
# Override with DESCRY_DIST / DESCRY_DOWNLOADS if your layout differs.
DIST = Path(os.environ.get('DESCRY_DIST', Path.home() / 'descry-sidecars/dist'))
DL = Path(os.environ.get('DESCRY_DOWNLOADS', Path.home() / 'Downloads'))
OUT = DIST / 'descry_wav_library'
WAV_ROOT = OUT / '3ds/descry/wav'
BMT_OUTER = DL / 'Blu Mar Ten - Blu Mar Ten - Jungle Jungle - 1989 to 1999 Samplepack.zip'


def short_name(name: str) -> str:
    name = re.sub(r'[^A-Za-z0-9._-]+', '_', name).strip('_')
    if not name.lower().endswith('.wav'):
        name += '.wav'
    if len(name) > 39:
        name = name[:35].rstrip('._-') + '.wav'
    return name


def convert(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([
        'ffmpeg', '-v', 'error', '-y', '-i', str(src),
        '-vn', '-ac', '1', '-ar', '32000', '-c:a', 'pcm_s16le', str(dst),
    ])


def unique_dst(folder: Path, wanted: str) -> Path:
    wanted = short_name(wanted)
    p = folder / wanted
    n = 2
    while p.exists():
        stem = Path(wanted).stem
        suffix = f'_{n}.wav'
        p = folder / (stem[:39-len(suffix)] + suffix)
        n += 1
    return p


def add_file(manifest: list[dict], src: Path, folder: str, name: str | None = None) -> None:
    dst = unique_dst(WAV_ROOT / folder, name or src.name)
    convert(src, dst)
    manifest.append({'folder': folder, 'name': dst.name, 'source': str(src), 'bytes': dst.stat().st_size})


def bmt_inner() -> zipfile.ZipFile:
    with zipfile.ZipFile(BMT_OUTER) as outer:
        inner = next(n for n in outer.namelist() if n.lower().endswith('.zip'))
        data = outer.read(inner)
    return zipfile.ZipFile(io.BytesIO(data))


def bmt_find(z: zipfile.ZipFile, needle: str) -> str:
    needle = needle.lower()
    names = [n for n in z.namelist() if n.lower().endswith('.wav') and not n.startswith('__MACOSX/')]
    hits = [n for n in names if needle in n.lower()]
    if not hits:
        raise FileNotFoundError(needle)
    return sorted(hits, key=lambda n: (len(n), n))[0]


def add_bmt(manifest: list[dict], z: zipfile.ZipFile, td: Path, folder: str, needle: str, name: str) -> None:
    member = bmt_find(z, needle)
    src = td / short_name(name)
    src.write_bytes(z.read(member))
    dst = unique_dst(WAV_ROOT / folder, name)
    convert(src, dst)
    manifest.append({'folder': folder, 'name': dst.name, 'source': member, 'bytes': dst.stat().st_size})


def main() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    WAV_ROOT.mkdir(parents=True)
    manifest: list[dict] = []

    # Existing curated classic breaks.
    for src in sorted((DL / 'drumkits/_wav/jungle').glob('*.wav')):
        add_file(manifest, src, '01_Jungle_Classics')

    # Rhythm Lab amen variations (20 WAVs, BPM retained in names).
    for src in sorted((DL / 'rhythm-lab.com_amen_vol.1/WAV').glob('*.wav')):
        add_file(manifest, src, '02_Amen_Variations')

    # Compact complete 808 mini kit.
    for src in sorted((DL / 'drumkits/_wav/808').glob('*.wav')):
        add_file(manifest, src, '03_808_Kit')

    selected = {
        '04_BMT_Breaks': [
            ('Breaks/Let There Break.wav', 'let_there_break.wav'),
            ('Breaks/Shoes Break - 4A.wav', 'shoes_break.wav'),
            ('Breaks/Right Break - 6A.wav', 'right_break.wav'),
            ('Breaks/Jazz Note Break - 10A.wav', 'jazz_note_break.wav'),
            ('Breaks/Drumz Amen', 'drumz_amen.wav'),
            ('Breaks/Wheel Up Amen', 'wheel_up_amen.wav'),
            ('Breaks/Futureproof Amen', 'futureproof_amen.wav'),
            ('Breaks/Good Times Break', 'good_times_break.wav'),
            ('Breaks/Mystical Break', 'mystical_break.wav'),
            ('Breaks/Control Break', 'control_break.wav'),
            ('Breaks/Firin Line Break', 'firin_line_break.wav'),
            ('Breaks/Fairlight Break', 'fairlight_break.wav'),
            ('Breaks/Sound Control Break', 'sound_control_break.wav'),
            ('Breaks/Searchin Break', 'searchin_break.wav'),
            ('Breaks/Seatown Break', 'seatown_break.wav'),
            ('Breaks/Blue Break', 'blue_break.wav'),
            ('Breaks/Centuries Break', 'centuries_break.wav'),
            ('Breaks/Voyage Break', 'voyage_break.wav'),
            ('Breaks/Rock Amen 2', 'rock_amen_2.wav'),
            ('Breaks/Stone Break', 'stone_break.wav'),
        ],
        '05_BMT_Bass': [
            ('Bass/London Sub - 1A.wav', 'london_sub.wav'),
            ('Bass/Wheel Up Sub.wav', 'wheel_up_sub.wav'),
            ('Bass/What Ya Gonna Sub', 'what_ya_gonna_sub.wav'),
            ('Bass/London Smooth Sub', 'london_smooth_sub.wav'),
            ('Bass/Worries Sub', 'worries_sub.wav'),
            ('Bass/Gang Sub', 'gang_sub.wav'),
        ],
        '06_BMT_Stabs': [
            ('FX/Far Out Stab', 'far_out_stab.wav'),
            ('Control Stab 2', 'control_stab_2.wav'),
            ('Knives Stab', 'knives_stab.wav'),
            ('Tip Stab', 'tip_stab.wav'),
            ('Fire Stab', 'fire_stab.wav'),
            ('Darkness Stabs', 'darkness_stabs.wav'),
            ('Times Stab', 'times_stab.wav'),
            ('Technology Ana Stab', 'technology_stab.wav'),
        ],
        '07_BMT_Pads_Atmos': [
            ('Dreams Pad', 'dreams_pad.wav'),
            ('Little Roller Sharp Pad', 'little_roller_pad.wav'),
            ('Pads/Night Train Pad', 'night_train_pad.wav'),
            ('Pads/Myriad Pad', 'myriad_pad.wav'),
            ('Pads/Freedom Pad', 'freedom_pad.wav'),
            ('Pads/Pesh Pad', 'pesh_pad.wav'),
            ('Pads/Recharge Pad', 'recharge_pad.wav'),
            ('FX/Soul Atmos', 'soul_atmos.wav'),
            ('FX/Stranger Atmos', 'stranger_atmos.wav'),
            ('God Chord Vox Pads', 'god_chord_vox_pad.wav'),
        ],
    }

    with bmt_inner() as z, tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        for folder, entries in selected.items():
            for needle, name in entries:
                add_bmt(manifest, z, td, folder, needle, name)

    counts: dict[str, int] = {}
    for m in manifest:
        counts[m['folder']] = counts.get(m['folder'], 0) + 1
    if any(n > 63 for n in counts.values()):
        raise RuntimeError(f'folder exceeds browser capacity: {counts}')
    if any(len(m['name']) > 39 for m in manifest):
        raise RuntimeError('filename exceeds 39 chars')

    (OUT / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    lines = ['# descry WAV library', '', 'Upload `3ds/descry/wav/` to `sdmc:/3ds/descry/wav/`.', '', 'All files: PCM16 mono 32000 Hz.', '']
    for folder in sorted(counts):
        lines.append(f'- `{folder}/` — {counts[folder]} WAV files')
    lines += ['', f'Total: **{len(manifest)} WAV files**.']
    (OUT / 'MANIFEST.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    total = sum(m['bytes'] for m in manifest)
    print(f'wrote {len(manifest)} WAV files in {len(counts)} folders ({total/1024/1024:.2f} MiB)')
    for folder in sorted(counts):
        print(f'  {folder}: {counts[folder]}')

if __name__ == '__main__':
    main()
