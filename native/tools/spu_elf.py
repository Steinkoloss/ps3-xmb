"""Load an original SPU ELF without treating resource data as instructions."""

from dataclasses import dataclass

from sce import region, unpack


@dataclass(frozen=True)
class Image:
    entry: int
    memory: bytes
    executable: tuple


def load(data):
    header = unpack(data, 0, ">16sHHIIIIIHHHHHH")
    identity, kind, machine, version, entry, programs, sections = header[:7]
    if identity[:7] != b"\x7fELF\x01\x02\x01" or machine != 23 or version != 1 or kind != 2:
        raise ValueError("Expected an original big-endian SPU executable")
    _, header_size, program_size, program_count, section_size, section_count, _ = header[7:]
    if (header_size != 52 or program_size != 32 or section_size != 40
            or program_count > 32 or section_count > 4096):
        raise ValueError("Unsupported SPU ELF tables")
    memory = bytearray(256 * 1024)
    loaded = []
    for index in range(program_count):
        kind, offset, address, _, size, capacity, _, _ = unpack(data, programs + index * program_size, ">8I")
        if kind != 1:
            continue
        if size > capacity or address + capacity > len(memory):
            raise ValueError("SPU segment exceeds local store")
        if any(address < end and start < address + capacity for start, end in loaded):
            raise ValueError("Overlapping SPU load segments")
        memory[address:address + size] = region(data, offset, size)
        loaded.append((address, address + capacity))
    executable = []
    for index in range(section_count):
        _, kind, flags, address, offset, size, _, _, _, _ = unpack(
            data, sections + index * section_size, ">10I")
        if flags & 4:
            if kind != 1 or address % 4 or size % 4 or not size:
                raise ValueError("Invalid executable SPU section")
            if not any(start <= address and address + size <= end for start, end in loaded):
                raise ValueError("Executable SPU section is not loaded")
            if bytes(memory[address:address + size]) != region(data, offset, size):
                raise ValueError("Executable SPU section differs from its loaded segment")
            executable.append((address, address + size))
    executable.sort()
    if (not executable or entry % 4 or not any(start <= entry < end for start, end in executable)
            or any(a[1] > b[0] for a, b in zip(executable, executable[1:]))):
        raise ValueError("Invalid executable SPU section ranges or entry")
    return Image(entry, bytes(memory), tuple(executable))
