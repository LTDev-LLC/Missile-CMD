#!/usr/bin/env python3
"""Record curated pages and gameplay using the real renderer and a host C compiler."""
import argparse
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import zlib

from help_assets import generate as generate_help
from sources import sources as project_sources

ROOT = Path(__file__).resolve().parent.parent
WIDTH, HEIGHT, SCALE = 128, 64, 4
PALETTE = bytes((254, 138, 44, 0, 0, 0))


# Frame a PNG chunk with its big-endian length and CRC over the type and payload
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(pixels):
    """Write a two-color PNG with exact 4x pixel scaling and no timestamps."""
    if len(pixels) != WIDTH * HEIGHT or any(value > 1 for value in pixels):
        raise ValueError("Expected a 128x64 monochrome framebuffer")
    rows = bytearray()
    for y in range(HEIGHT):
        # Each scanline starts with PNG filter zero and stores eight enlarged pixels per byte
        row = bytearray(1 + WIDTH * SCALE // 8)
        for x in range(WIDTH * SCALE):
            row[1 + x // 8] |= pixels[y * WIDTH + x // SCALE] << (7 - x % 8)
        # Repeat the enlarged row to apply the same integer scaling vertically
        rows.extend(row * SCALE)
    # Uncompressed DEFLATE keeps committed PNG bytes stable across zlib versions
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH * SCALE, HEIGHT * SCALE, 1, 3, 0, 0, 0))
            + chunk(b"PLTE", PALETTE)
            + chunk(b"IDAT", zlib.compress(bytes(rows), level=0))
            + chunk(b"IEND", b""))


def save_screenshots(destination, rendered, check=False):
    """Update generated PNGs or verify the exact curated set without writing."""
    # This directory holds generated PNGs; removed scenes must not leave stale images behind
    obsolete = {path.name for path in destination.glob("*.png")} - rendered.keys()
    if check:
        stale = {name for name, data in rendered.items()
                 if not (destination / name).exists() or (destination / name).read_bytes() != data}
        if stale or obsolete:
            raise SystemExit("Screenshots need updating: " + ", ".join(sorted(stale | obsolete))
                             + ". Run make screenshots.")
        return
    destination.mkdir(exist_ok=True)
    for name, data in rendered.items():
        (destination / name).write_bytes(data)
    # Remove retired PNGs only after every current scene has been written successfully
    for name in obsolete:
        (destination / name).unlink()


# Compile the host renderer, validate its frames, and update or check the curated gallery
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail if recorded screenshots are missing, changed or obsolete")
    args = parser.parse_args()
    generate_help(ROOT)
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    binary = build / "screenshots"
    sources = ["tools/screenshots.c", "tests/host_platform.c", "tests/host_canvas.c"]
    # Compile separate modules; the device unity-build source would duplicate definitions
    sources += project_sources("core", "ui", "app")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    command = compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                          "-Isrc/include", "-Ibuild/generated", "-Itests/stubs", "-Itests"]
    command += shlex.split(os.environ.get("SCREENSHOT_CFLAGS", ""))
    # Keep capture assertions enabled even if caller-supplied compiler flags define NDEBUG
    subprocess.run(command + ["-UNDEBUG"] + sources + ["-o", str(binary)], cwd=ROOT, check=True)
    destination = ROOT / "screenshots"
    with tempfile.TemporaryDirectory(prefix="screenshots_", dir=build) as temporary:
        result = subprocess.run([str(binary), temporary], cwd=ROOT, check=True, capture_output=True, text=True)
        # The renderer emits a private scene list; reject duplicate or unsafe output basenames
        names = result.stdout.splitlines()
        if not names or len(names) != len(set(names)) or any(not re.fullmatch(r"[a-z_]+", name) for name in names):
            raise ValueError("Screenshot renderer returned an invalid scene list")
        rendered = {}
        for name in names:
            frame = (Path(temporary) / f"{name}.pgm").read_bytes()
            header = b"P5\n128 64\n1\n"
            if not frame.startswith(header):
                raise ValueError(f"Invalid framebuffer: {name}")
            pixels = frame[len(header):]
            if not any(pixels) or all(pixels):
                raise ValueError(f"Empty or solid screenshot: {name}")
            rendered[f"{name}.png"] = png(pixels)
        save_screenshots(destination, rendered, args.check)
        print("Screenshots are up to date." if args.check else "Screenshots updated.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.stderr or f"Screenshot command failed: {error}") from error
