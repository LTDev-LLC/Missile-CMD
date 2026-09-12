# Core integrity checks for the production FAP packer
import struct
import sys
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pack_fap import HEADER, SECTION, SYMBOL, compact, parse, normalize_absolute_relocations
import pack_fap


# Build a small ARM ELF with equivalent standard and fast relocation records
def fixture():
    names = ["", ".text", ".rodata", ".rel.text", ".symtab", ".strtab", ".fast.rel.text", ".fapmeta", ".shstrtab"]
    name_data = b""
    offsets = []
    for name in names:
        offsets.append(len(name_data))
        name_data += name.encode() + b"\0"
    # Both relocation encodings point the same text word at the rodata section
    payloads = [b"", b"\0" * 8, b"hello\0\0\0", struct.pack("<II", 0, (1 << 8) | 2),
                bytes(SYMBOL.size) + SYMBOL.pack(1, 0, 0, 0, 0, 2), b"\0local\0",
                struct.pack("<BIBIII", 1, 1, 130, 2, 0, 1) + bytes(3), b"meta", name_data]
    sections = [[offsets[i], 1, 0, 0, 0, len(payload), 0, 0, 1, 0] for i, payload in enumerate(payloads)]
    sections[0] = [0] * 10
    sections[1][2] = 6
    sections[2][2] = 2
    sections[3][1], sections[3][2], sections[3][6], sections[3][7], sections[3][9] = 9, 64, 4, 1, 8
    sections[4][1], sections[4][6], sections[4][7], sections[4][9] = 2, 5, 2, 16
    sections[5][1] = sections[8][1] = 3
    data = bytearray(HEADER.size)
    for i, payload in enumerate(payloads):
        if i:
            sections[i][4] = len(data)
            data.extend(payload)
    shoff = len(data)
    for section in sections:
        data.extend(SECTION.pack(*section))
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    HEADER.pack_into(data, 0, ident, 1, 40, 1, 1, 0, shoff, 0, HEADER.size, 0, 0, SECTION.size, len(sections), 8)
    return bytes(data)


# Protect loader compatibility and rejection paths in the production FAP packer
class TestPacking(unittest.TestCase):
    def test_100_kb_maximum_at_publication_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / 'raw.fap', root / 'packed.fap'
            source.write_bytes(fixture())
            args = ['pack_fap', str(source), '--output', str(output), '--metadata-dir', str(root)]
            for size in (65536, 100000, 100001):
                output.write_bytes(b'previous verified artifact')
                with patch.object(sys, 'argv', args), patch.object(pack_fap, 'compact', return_value=bytes(size)):
                    if size <= 100000:
                        pack_fap.main()
                        self.assertEqual(output.stat().st_size, size)
                    else:
                        with self.assertRaisesRegex(ValueError, 'at most 100,000 bytes'):
                            pack_fap.main()
                        self.assertEqual(output.read_bytes(), b'previous verified artifact')

    # Verify compaction preserves identities and payloads and is stable when repeated
    def test_preserves_indices_code_and_metadata(self):
        raw = fixture()
        packed = compact(raw)
        before, after = parse(raw), parse(packed)
        self.assertLess(len(packed), len(raw))
        self.assertEqual(before[0][4], after[0][4])
        self.assertEqual(before[0][12:], after[0][12:])
        for index in (1, 2, 6, 7):
            self.assertEqual(before[2][index], after[2][index])
        self.assertEqual(after[1][3], [0] * 10)
        self.assertEqual(compact(packed), packed)

    # Compare normalized relocations at varied load addresses, including unsigned wrap
    def test_absolute_normalization_matches_loader(self):
        from collections import Counter
        import random
        # Use a fixed seed so a relocation arithmetic failure is reproducible
        rng = random.Random(824)
        for _ in range(100):
            words = [rng.getrandbits(32) for _ in range(12)]
            payload = struct.pack("<12I", *words)
            records = Counter((2, True, 2 + i % 2, rng.randrange(1024), i * 4) for i in range(10))
            records[(2, False, 0x12345678, 0, 40)] = 1
            records[(3, True, 2, 99, 44)] = 1
            normalized, data, encoded = normalize_absolute_relocations(records, payload)
            self.assertLessEqual(struct.unpack_from("<I", encoded, 1)[0], 4)
            # Compare patched words at ordinary addresses and across unsigned wraparound
            for base in (0, 0x20008000, 0xFFFFFFFF, rng.getrandbits(32)):
                original, updated = list(words), list(struct.unpack("<12I", data))
                for (kind, internal, _, value, offset) in records:
                    original[offset // 4] = (original[offset // 4] + base + value) & 0xFFFFFFFF
                for (kind, internal, _, value, offset) in normalized:
                    updated[offset // 4] = (updated[offset // 4] + base + value) & 0xFFFFFFFF
                self.assertEqual(original, updated)
            self.assertEqual(normalize_absolute_relocations(normalized, data)[1:], (data, encoded))
        with self.assertRaises(ValueError):
            normalize_absolute_relocations(Counter({(2, True, 2, 0, 0): 2}), bytes(4))
        with self.assertRaises(ValueError):
            normalize_absolute_relocations(Counter({(2, True, 2, 0, 0): 1, (2, False, 3, 0, 1): 1}), bytes(8))

    # Reject disagreeing relocation encodings before stripping the symbol data needed to check them
    def test_rejects_mismatch_before_discarding_symbols(self):
        raw = bytearray(fixture())
        _, sections, _, _ = parse(raw)
        raw[sections[6][4] + 6] = 1  # Fast target now disagrees with standard relocation
        with self.assertRaises(ValueError):
            compact(raw)

    # Reject truncated ELF data and unsupported fast relocation versions
    def test_rejects_truncation_and_unsupported_versions(self):
        raw = fixture()
        for length in (0, 16, 51, len(raw) - 1):
            with self.assertRaises(ValueError):
                compact(raw[:length])
        bad = bytearray(raw)
        _, sections, _, _ = parse(raw)
        bad[sections[6][4]] = 2
        with self.assertRaises(ValueError):
            compact(bad)


if __name__ == "__main__":
    unittest.main()
