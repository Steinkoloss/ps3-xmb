#!/usr/bin/env python3
"""Build the renderer image the native code reads: the decrypted custom_render_plugin ELF
loaded at 0x02000000 with its PRX relocations applied (types 1, 4 and 6; 57 is ignored).

  python3 tools/renderer_image.py custom_render_plugin.elf --output renderer-image.bin

The output is firmware data and stays local.
"""
import argparse
import hashlib
import struct
from pathlib import Path

BASE = 0x02000000
IMAGE_SHA256 = "31b3c6fe7b315f94ecd7927028c5e1bac5e6d1c599b2cd8a5a62660ee13e38e4"
LOAD, PRX_RELOCATIONS = 1, 0x700000A4


def renderer_image(elf):
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 2:
        raise ValueError("Not a 64-bit big-endian ELF")
    phoff, = struct.unpack_from(">Q", elf, 0x20)
    entry_size, count = struct.unpack_from(">HH", elf, 0x36)
    loads, relocations = [], []
    for i in range(count):
        kind, _, offset, address, _, file_size, memory_size, _ = struct.unpack_from(">IIQQQQQQ", elf, phoff + i * entry_size)
        if kind == LOAD:
            loads.append((offset, address, file_size, memory_size))
        elif kind == PRX_RELOCATIONS:
            relocations.append(elf[offset:offset + file_size])
    if len(loads) != 2 or len(relocations) != 1:
        raise ValueError("Unexpected renderer segments")
    image = bytearray(max(address + memory_size for _, address, _, memory_size in loads))
    for offset, address, file_size, _ in loads:
        image[address:address + file_size] = elf[offset:offset + file_size]
    segments = [BASE + address for _, address, _, _ in loads]
    table = relocations[0]
    if len(table) % 24:
        raise ValueError("Truncated relocation table")
    for at in range(0, len(table), 24):
        offset, _, value_segment, address_segment, kind, pointer = struct.unpack_from(">QHBBIQ", table, at)
        if kind == 57:
            continue
        target = segments[address_segment] + offset - BASE
        value = (segments[value_segment] if value_segment != 0xFF else 0) + pointer
        if kind == 1:
            struct.pack_into(">I", image, target, value & 0xFFFFFFFF)
        elif kind == 4:
            struct.pack_into(">H", image, target, value & 0xFFFF)
        elif kind == 6:
            struct.pack_into(">H", image, target, ((value + 0x8000) >> 16) & 0xFFFF)
        else:
            raise ValueError(f"Unadapted relocation type {kind}")
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError("Relocated renderer does not match the verified image")
    return bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_bytes(renderer_image(args.elf.read_bytes()))


if __name__ == "__main__":
    main()
