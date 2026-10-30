"""Decode the original QRCF file table; do not reinterpret its resource data.

Format: https://www.psdevwiki.com/ps3/CXML_Containers
"""

from pathlib import PurePosixPath

from sce import inflate, region, unpack


def cstring(data, offset, maximum):
    raw = region(data, offset, maximum)
    end = raw.find(b"\0")
    if end < 0:
        raise ValueError("Unterminated QRC string")
    return raw[:end].decode("utf-8")


def resources(data):
    if data[:4] == b"QRCC":
        expected, = unpack(data, 4, ">I")
        data = inflate(region(data, 8, len(data) - 8))
        if len(data) != expected:
            raise ValueError("QRC decompressed size does not match its header")
    if data[:4] != b"QRCF":
        raise ValueError("Expected an original QRCF resource container")
    version, = unpack(data, 4, ">I")
    if version != 0x110:
        raise ValueError("Unsupported QRCF revision")
    tables = [unpack(data, 8 + index * 8, ">II") for index in range(6)]
    for offset, length in tables:
        region(data, offset, length)
    tree, tree_size = tables[0]
    names, names_size = tables[1]
    strings, strings_size = tables[2]
    file_data, file_size = tables[5]
    offset = 0
    seen = set()
    while offset < tree_size:
        node = unpack(region(data, tree, tree_size), offset, ">7i")
        count = node[1]
        if count < 0 or count > 64:
            raise ValueError("Invalid QRC attribute count")
        size = 28 + count * 16
        region(data, tree + offset, size)
        if offset + size > tree_size:
            raise ValueError("QRC attributes extend beyond the tree table")
        name = cstring(region(data, strings, strings_size), node[0], strings_size - node[0])
        attrs = [unpack(data, tree + offset + 28 + index * 16, ">4I") for index in range(count)]
        offset += size
        if name != "file":
            if count:
                raise ValueError("Unsupported QRC element attributes")
            continue
        source = next((attr for attr in attrs if attr[1] == 6), None)
        identity = next((attr for attr in attrs if attr[1] == 7), None)
        if count != 2 or source is None or identity is None:
            raise ValueError("QRC file entry is missing its data or name")
        relative = identity[2] + 4
        filename = cstring(data, names + relative, names_size - relative)
        path = PurePosixPath(filename)
        if (not filename or path.is_absolute() or ".." in path.parts or "\\" in filename
                or ":" in filename or str(path) != filename or str(path) == "."):
            raise ValueError("Unsafe QRC resource path")
        if filename in seen:
            raise ValueError("Duplicate QRC resource path")
        seen.add(filename)
        content = region(region(data, file_data, file_size), source[2], source[3])
        yield filename, content
