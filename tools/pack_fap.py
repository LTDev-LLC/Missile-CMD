#!/usr/bin/env python3
"""Verify and normalize fast relocations, discard duplicate ELF relocation data.

Section indices MUST remain unchanged: fast relocations refer to them directly.
Official, Unleashed, and Momentum support fast-only loading. Keep empty
symbol/string tables so missing-import diagnostics have initialized, bounded tables.
Reference: https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/lib/flipper_application/elf/elf_file.c
Also checked against the same loader in Unleashed unlshd-092 and Momentum mntm-012.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import struct
import tempfile

# Parse fixed little-endian ELF32 layouts explicitly rather than relying on host word sizes
HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
SECTION = struct.Struct("<10I")
SYMBOL = struct.Struct("<IIIBBH")
# Project packaging ceiling: 100 KB in decimal bytes.
LIMIT = 100_000


# Reject malformed input or a violated packaging invariant with a useful failure message
def check(condition, message):
    if not condition:
        raise ValueError(message)


# Read a bounded null-terminated ASCII string from an ELF string table
def cstring(data, offset):
    check(0 <= offset < len(data), "String offset out of bounds")
    end = data.find(b"\0", offset)
    check(end >= 0, "Unterminated ELF string")
    return data[offset:end].decode("ascii")


# Validate the ARM ELF layout and retain every section index while extracting its contents
def parse(data):
    check(len(data) >= HEADER.size, "Truncated ELF header")
    header = list(HEADER.unpack_from(data))
    check(header[0][:7] == b"\x7fELF\x01\x01\x01" and header[1] == 1 and header[2] == 40,
          "Expected little-endian ARM relocatable ELF")
    check(header[10] == 0 and header[11] == SECTION.size, "Unexpected ELF layout")
    check(header[12] > 0 and header[13] < header[12], "Invalid section table")
    check(header[6] + header[12] * SECTION.size <= len(data), "Truncated section table")
    sections = [list(SECTION.unpack_from(data, header[6] + i * SECTION.size)) for i in range(header[12])]
    bodies = []
    for section in sections:
        check(section[1] == 8 or section[4] + section[5] <= len(data), "Truncated section")
        # NOBITS sections reserve loaded memory but carry no file payload to copy
        bodies.append(b"" if section[1] == 8 else data[section[4]:section[4] + section[5]])
    names = [cstring(bodies[header[13]], s[0]) for s in sections]
    return header, sections, bodies, names


# Compute the firmware import hash with the loader's unsigned 32-bit wrap
def symbol_hash(name):
    result = 5381
    for char in name.encode("ascii"):
        result = (result * 33 + char) & 0xFFFFFFFF
    return result


# Decode bounded fast relocation groups into comparable per-location records
def fast_records(data, sections, target):
    check(len(data) >= 5 and data[0] == 1, "Unsupported fast relocation version")
    count = struct.unpack_from("<I", data, 1)[0]
    offset, records = 5, []
    for _ in range(count):
        check(offset + 5 <= len(data), "Truncated fast relocation")
        tag, value = struct.unpack_from("<BI", data, offset)
        offset += 5
        # The high tag bit selects a section reference; external references carry import hashes
        internal = bool(tag & 128)
        symbol_value = 0
        if internal:
            check(offset + 4 <= len(data), "Truncated fast target")
            symbol_value = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            check(0 < value < len(sections) and sections[value][2] & 2, "Bad fast section index")
            check(symbol_value <= sections[value][5] + 1, "Bad fast symbol offset")
        check(offset + 4 <= len(data), "Truncated fast offsets")
        offsets_count = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        check(offset + offsets_count * 3 <= len(data), "Truncated fast offsets")
        for _ in range(offsets_count):
            # Three-byte offsets identify locations where the loader patches full words
            location = int.from_bytes(data[offset:offset + 3], "little")
            check(location + 4 <= sections[target][5], "Relocation outside target")
            records.append((tag & 127, internal, value, symbol_value, location))
            offset += 3
    check(offset == len(data), "Trailing fast relocation bytes")
    return Counter(records)


def normalize_absolute_relocations(records, payload):
    """Fold internal ABS32 symbol offsets into addends, then share section-base groups.

    Firmware 1.4.3 adds (section base + symbol offset) to each ABS32 word.
    Moving the offset into that word preserves its value for every load address,
    including Thumb pointer bits and 32-bit wrap. Other relocation types/imports
    remain unchanged. Standard/fast equivalence is checked BEFORE this transform.
    """
    adjusted = bytearray(payload)
    normalized = Counter()
    occupied = set()
    for record, repetitions in records.items():
        kind, internal, section, value, location = record
        check(repetitions == 1, "Duplicate relocation")
        # Reject overlapping words before normalization can alter another relocation
        positions = set(range(location, location + 4))
        check(not positions & occupied, "Overlapping relocations")
        occupied.update(positions)
        if kind == 2 and internal:
            addend = struct.unpack_from("<I", adjusted, location)[0]
            struct.pack_into("<I", adjusted, location, (addend + value) & 0xFFFFFFFF)
            normalized[(kind, internal, section, 0, location)] += 1
        else:
            normalized[record] += 1
    groups = {}
    for kind, internal, section, value, location in normalized:
        groups.setdefault((kind, internal, section, value), []).append(location)
    encoded = bytearray(struct.pack("<BI", 1, len(groups)))
    for (kind, internal, section, value), locations in groups.items():
        encoded.extend(struct.pack("<BI", kind | (128 if internal else 0), section))
        if internal:
            encoded.extend(struct.pack("<I", value))
        encoded.extend(struct.pack("<I", len(locations)))
        for location in locations:
            encoded.extend(location.to_bytes(3, "little"))
    # Independently compare affine relocation results before returning changed bytes
    check(relocation_values(records, payload) == relocation_values(normalized, adjusted),
          "Relocation normalization changed loaded values")
    return normalized, bytes(adjusted), bytes(encoded)


# Compare effective absolute addends independently of relocation group encoding
def relocation_values(records, payload):
    result = Counter()
    for (kind, internal, section, value, location), count in records.items():
        if kind == 2 and internal:
            value = (value + struct.unpack_from("<I", payload, location)[0]) & 0xFFFFFFFF
        result[(kind, internal, section, value, location)] += count
    return result


# Verify relocation equivalence before removing duplicate data and repacking aligned sections
def compact(data):
    header, sections, bodies, names = parse(data)
    check(".text" in names and ".fapmeta" in names, "Missing app metadata or code")
    check((header[4] & ~1) < sections[names.index(".text")][5], "Entry outside code")
    fast = {}
    for i, name in enumerate(names):
        if name.startswith(".fast.rel"):
            target_name = name[len(".fast.rel"):]
            check(target_name in names, "Missing fast target")
            fast[target_name] = fast_records(bodies[i], sections, names.index(target_name))
    check(fast, "Missing fast relocations; refusing to strip")
    for i, section in enumerate(sections):
        if section[1] != 9:
            continue
        check(names[i].startswith(".rel"), "Unknown relocation section")
        check(section[7] < len(names) and section[6] < len(names), "Bad relocation link")
        target_name = names[section[7]]
        check(target_name in fast, "No equivalent fast relocations")
        symtab = sections[section[6]]
        check(symtab[6] < len(bodies), "Bad symbol string table")
        strings = bodies[symtab[6]]
        expected = []
        check(len(bodies[i]) % 8 == 0, "Bad relocation length")
        for location, info in struct.iter_unpack("<II", bodies[i]):
            sym_offset = (info >> 8) * SYMBOL.size
            check(sym_offset + SYMBOL.size <= len(bodies[section[6]]), "Bad symbol index")
            name, value, _, _, _, index = SYMBOL.unpack_from(bodies[section[6]], sym_offset)
            expected.append((info & 255, index != 0, index if index else symbol_hash(cstring(strings, name)), value if index else 0, location))
        check(Counter(expected) == fast[target_name], "Fast/standard relocation mismatch")
        # Keep an empty section header so stripping relocation data cannot shift indices
        sections[i] = [0] * 10
        bodies[i] = b""
    # Merge equivalent internal absolute relocations only after SDK verification
    for i, name in enumerate(names):
        if name.startswith(".fast.rel"):
            target_name = name[len(".fast.rel"):]
            target = names.index(target_name)
            fast[target_name], bodies[target], bodies[i] = normalize_absolute_relocations(
                fast[target_name], bodies[target])
            sections[i][5] = len(bodies[i])
    # All relocations were checked before discarding debug-only symbols
    for i, name in enumerate(names):
        if name == ".symtab":
            bodies[i] = bytes(SYMBOL.size)
            sections[i][5] = SYMBOL.size
            sections[i][7] = 1
        elif name == ".strtab":
            bodies[i] = b"\0"
            sections[i][5] = 1
    packed = bytearray(HEADER.size)
    for i, section in enumerate(sections):
        if section[1] == 0:
            continue
        alignment = section[8] or 1
        check(alignment <= 4096 and alignment & (alignment - 1) == 0, "Unexpected section alignment")
        # Pad to the next power-of-two section boundary without changing its alignment requirement
        packed.extend(bytes((-len(packed)) % alignment))
        section[4] = len(packed)
        if section[1] != 8:
            packed.extend(bodies[i])
    packed.extend(bytes((-len(packed)) % 4))
    header[6] = len(packed)
    for section in sections:
        packed.extend(SECTION.pack(*section))
    HEADER.pack_into(packed, 0, *header)
    # Re-parse and validate compact relocation bounds and unchanged section identities
    _, checked, payloads, _ = parse(packed)
    for i, name in enumerate(names):
        if name.startswith(".fast.rel"):
            check(fast_records(payloads[i], checked, names.index(name[len(".fast.rel"):])) == fast[name[len(".fast.rel"):]], "Changed relocations")
        if sections[i][2] & 2 or name == ".fapmeta":
            check(payloads[i] == bodies[i], "Changed application payload")
    return bytes(packed)


# Pack the selected FAP, record its size, and publish it only after the size check passes
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fap", type=Path, nargs="?", default=Path("dist/missile_cmd.fap"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--metadata-dir", type=Path, default=Path("build_metadata"))
    args = parser.parse_args()
    raw = args.fap.read_bytes()
    packed = compact(raw)
    output = args.output or args.fap
    metadata = args.metadata_dir
    metadata.mkdir(parents=True, exist_ok=True)
    report_path = metadata / "size.json"
    report = dict(raw_bytes=len(raw), packed_bytes=len(packed), hard_limit=LIMIT,
                  reserve_bytes=LIMIT - len(packed))
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    text = f"FAP: {len(packed):,}/{LIMIT:,} bytes; headroom {LIMIT-len(packed):,}"
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as stream:
            stream.write(text + "\n")
    check(len(packed) <= LIMIT, f"FAP must be at most {LIMIT:,} bytes")
    output.parent.mkdir(parents=True, exist_ok=True)
    # Publish by a same-directory rename so a partial write cannot replace the verified output
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(packed)
    temporary.replace(output)


if __name__ == "__main__":
    main()
