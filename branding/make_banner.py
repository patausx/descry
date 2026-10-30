#!/usr/bin/env python3
"""Render the README-only wordmark; leave application branding untouched.

The canonical acid wordmark keeps its original pixel geometry and ink colours.
Only its background changes; the README has no tagline or decorative rule.
Requires Pillow. Run with --check to verify docs/banner.png without writing it.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "branding/final/wordmark_acid.png"
OUTPUT = ROOT / "docs/banner.png"
BACKGROUND = (0x21, 0x28, 0x30)
SIZE = (744, 332)


def render() -> Image.Image:
    with Image.open(SOURCE) as image:
        source = image.convert("RGB")
    old_background = Image.new("RGB", source.size, source.getpixel((0, 0)))
    mask = ImageChops.difference(source, old_background).convert("L")
    mask = mask.point(lambda value: 255 if value else 0)
    bounds = mask.getbbox()
    if bounds is None:
        raise ValueError("canonical wordmark is empty")
    ink = source.crop(bounds)
    if ink.width > SIZE[0] or ink.height > SIZE[1]:
        raise ValueError("canonical wordmark does not fit the README canvas")
    offset = ((SIZE[0] - ink.width) // 2, (SIZE[1] - ink.height) // 2)
    result = Image.new("RGB", SIZE, BACKGROUND)
    result.paste(ink, offset, mask.crop(bounds))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = render()
    if args.check:
        if not OUTPUT.is_file():
            parser.exit(1, "missing README banner; run branding/make_banner.py\n")
        with Image.open(OUTPUT) as image:
            actual = image.convert("RGB")
        if actual.size != expected.size or actual.tobytes() != expected.tobytes():
            parser.exit(1, "README banner differs; run branding/make_banner.py\n")
        print("PASS: logo-only README banner, original ink, background #212830")
    else:
        expected.save(OUTPUT, optimize=True)
        print(f"README banner {expected.width}x{expected.height} -> {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
