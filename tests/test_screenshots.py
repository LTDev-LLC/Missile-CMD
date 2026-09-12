# PNG encoding and generated-gallery integrity checks
import struct
import sys
import tempfile
from pathlib import Path
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from screenshots import png, save_screenshots


# Protect deterministic PNG encoding and safe maintenance of the curated image directory
class TestScreenshots(unittest.TestCase):
    # Decode every PNG pixel and check exact scaling, palette, chunk CRCs, and stable output
    def test_round_trip_preserves_pixels_palette_and_scale(self):
        pixels = bytes((x + y) % 2 for y in range(64) for x in range(128))
        data = png(pixels)
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        parts = {}
        offset = 8
        while offset < len(data):
            size, kind = struct.unpack_from(">I4s", data, offset)
            payload = data[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + payload))
            parts[kind] = payload
            offset += size + 12
        self.assertEqual(struct.unpack(">IIBBBBB", parts[b"IHDR"]), (512, 256, 1, 3, 0, 0, 0))
        self.assertEqual(parts[b"PLTE"], bytes((254, 138, 44, 0, 0, 0)))
        # Each enlarged row has one filter byte followed by sixty-four packed pixel bytes
        raw = zlib.decompress(parts[b"IDAT"])
        self.assertEqual(len(raw), 65 * 256)
        for y in range(256):
            self.assertEqual(raw[y * 65], 0)
            for x in range(512):
                actual = (raw[y * 65 + 1 + x // 8] >> (7 - x % 8)) & 1
                self.assertEqual(actual, pixels[y // 4 * 128 + x // 4])
        self.assertEqual(png(pixels), data)

    # Reject frames with the wrong size or values outside the monochrome palette
    def test_rejects_invalid_framebuffers(self):
        for data in (b"", bytes(8191), bytes(8193), bytes([2]) * 8192):
            with self.assertRaises(ValueError):
                png(data)

    # Verify stale-image cleanup, preservation of other files, and read-only screenshot checks
    def test_recording_removes_retired_scenes_and_check_does_not_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "screenshots"
            rendered = {"gameplay.png": png(bytes([0, 1]) * 4096)}
            with self.assertRaises(SystemExit):
                save_screenshots(destination, rendered, check=True)
            self.assertFalse(destination.exists())
            save_screenshots(destination, rendered)
            # Recording removes retired images while preserving unrelated notes
            (destination / "retired.png").write_bytes(b"old scene")
            (destination / "notes.txt").write_text("keep this")
            with self.assertRaisesRegex(SystemExit, "retired.png"):
                save_screenshots(destination, rendered, check=True)
            self.assertEqual((destination / "retired.png").read_bytes(), b"old scene")
            save_screenshots(destination, rendered)
            self.assertFalse((destination / "retired.png").exists())
            self.assertEqual((destination / "notes.txt").read_text(), "keep this")
            save_screenshots(destination, rendered, check=True)
            (destination / "gameplay.png").write_bytes(b"stale frame")
            with self.assertRaisesRegex(SystemExit, "gameplay.png"):
                save_screenshots(destination, rendered, check=True)
            self.assertEqual((destination / "gameplay.png").read_bytes(), b"stale frame")
