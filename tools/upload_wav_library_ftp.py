#!/usr/bin/env python3
"""Upload the built descry WAV library to 3DS ftpd over one direct connection.

The library lives outside the repo; override the location with DESCRY_DIST.
"""
from __future__ import annotations

import argparse
import ftplib
import os
import time
from pathlib import Path

DIST = Path(os.environ.get('DESCRY_DIST', Path.home() / 'descry-sidecars/dist'))
ROOT = DIST / 'descry_wav_library/3ds/descry/wav'
REMOTE_ROOT = '/3ds/descry/wav'


def connect(host: str, port: int, retries: int = 20) -> ftplib.FTP:
    last: Exception | None = None
    for attempt in range(1, retries + 1):
        ftp = ftplib.FTP()
        try:
            ftp.connect(host, port, timeout=12)
            ftp.login()
            ftp.set_pasv(True)
            print(f'connected directly to {host}:{port}')
            return ftp
        except Exception as e:
            last = e
            try:
                ftp.close()
            except Exception:
                pass
            print(f'waiting for ftpd ({attempt}/{retries}): {e}')
            time.sleep(2)
    raise RuntimeError(f'could not connect to ftpd: {last}')


def ensure_dir(ftp: ftplib.FTP, path: str) -> None:
    ftp.cwd('/')
    for part in [p for p in path.split('/') if p]:
        try:
            ftp.cwd(part)
        except ftplib.error_perm:
            ftp.mkd(part)
            ftp.cwd(part)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('host', nargs='?', default='192.168.1.106')
    ap.add_argument('port', nargs='?', type=int, default=5000)
    args = ap.parse_args()

    files = sorted(ROOT.glob('*/*.wav'))
    if not files:
        raise SystemExit(f'no WAV files under {ROOT}')

    ftp = connect(args.host, args.port)
    uploaded = 0
    try:
        for folder in sorted({p.parent.name for p in files}):
            ensure_dir(ftp, f'{REMOTE_ROOT}/{folder}')
            group = [p for p in files if p.parent.name == folder]
            for p in group:
                uploaded += 1
                print(f'[{uploaded:02d}/{len(files)}] {folder}/{p.name} ({p.stat().st_size // 1024} KiB)')
                with p.open('rb') as f:
                    ftp.storbinary(f'STOR {p.name}', f, blocksize=64 * 1024)

        # Verify directory counts over the same control connection.
        total = 0
        print('remote verification:')
        for folder in sorted({p.parent.name for p in files}):
            ensure_dir(ftp, f'{REMOTE_ROOT}/{folder}')
            names = ftp.nlst()
            count = sum(1 for n in names if n.lower().endswith('.wav'))
            total += count
            print(f'  {folder}: {count} WAV')
        print(f'remote total: {total} WAV')
        if total != len(files):
            raise RuntimeError(f'verification mismatch: local={len(files)}, remote={total}')
    finally:
        try:
            ftp.quit()
        except Exception:
            ftp.close()


if __name__ == '__main__':
    main()
