import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from cg import Program
from qrc import resources
from sce import inflate, region


def qrc(filename="lib/example.fpo", payload=b"original"):
    names = struct.pack(">I", 56) + filename.encode() + b"\0"
    tags = b"qrc\0file-table\0file\0src\0id\0"
    tree = struct.pack(">7i", 0, 0, -1, -1, -1, 28, 28)
    tree += struct.pack(">7i", 4, 0, 0, -1, -1, 56, 56)
    tree += struct.pack(">7i", 15, 2, 28, -1, -1, -1, -1)
    tree += struct.pack(">4I", 20, 6, 0, len(payload))
    tree += struct.pack(">4I", 24, 7, 0, 0)
    ids = 64 + len(tree)
    strings = ids + len(names)
    files = strings + len(tags)
    header = b"QRCF" + struct.pack(">15I", 0x110, 64, len(tree), ids, len(names), strings, len(tags),
                                    files, 0, files, 0, files, len(payload), 0, 0)
    return header + tree + names + tags + payload


def fragment():
    header = struct.pack(">8I", 7004, 6, 80, 0, 32, 32, 16, 64)
    domain = struct.pack(">6I", 1, 0, 0, 0, 0x00000101, 0)
    # Public ISA vector: MOV R3.yz, WPOS.xyzw; end. Operand predicates are true.
    code = bytes.fromhex("0c070100c801001d0000000000000000")
    return header + domain + bytes(8) + code


class QrcTests(unittest.TestCase):
    def test_original_resource_bytes_survive_compression_and_extraction(self):
        original = qrc(payload=bytes(range(256)))
        compressed = b"QRCC" + struct.pack(">I", len(original)) + zlib.compress(original)
        self.assertEqual(list(resources(compressed)), [("lib/example.fpo", bytes(range(256)))])

    def test_paths_cannot_escape_or_alias_the_output_tree(self):
        for name in ("../outside", "/absolute", "a/../outside", "a//b", "a/./b", "a/", "C:/outside", "a\\b", "."):
            with self.subTest(name=name), self.assertRaises(ValueError):
                list(resources(qrc(name)))

    def test_truncated_tables_and_resource_ranges_are_rejected(self):
        original = qrc()
        for length in (0, 8, 63, 64, len(original) - 1):
            with self.subTest(length=length), self.assertRaises(ValueError):
                list(resources(original[:length]))
        corrupt = bytearray(original)
        struct.pack_into(">I", corrupt, 64 + 56 + 28 + 8, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            list(resources(corrupt))

    def test_inflater_rejects_trailing_data_and_expansion_beyond_limit(self):
        compressed = zlib.compress(b"a" * 1024)
        with self.assertRaises(ValueError):
            inflate(compressed, 100)
        with self.assertRaises(ValueError):
            inflate(compressed + b"junk")
        with self.assertRaises(ValueError):
            region(b"data", -1, 1)


class CgTests(unittest.TestCase):
    def test_fragment_word_order_predicate_and_component_mask(self):
        program = Program(fragment())
        self.assertEqual(len(program.instructions), 1)
        instruction = program.instructions[0].to_dict()
        self.assertEqual(instruction["mnemonic"], "MOV")
        self.assertEqual(instruction["destination"], 3)
        self.assertEqual(instruction["write_mask"], 6)
        self.assertEqual(instruction["predicate"], 7)
        self.assertFalse(instruction["half"])
        self.assertTrue(instruction["end"])
        self.assertEqual(instruction["words"][1], 0x001DC801)

    def test_corrupt_programs_fail_instead_of_becoming_an_approximation(self):
        source = fragment()
        for length in (0, 31, 63, 79):
            with self.subTest(length=length), self.assertRaises(ValueError):
                Program(source[:length])
        for offset, value in ((0, 7005), (4, 7), (16, 0xFFFFFF00), (28, 65)):
            broken = bytearray(source)
            struct.pack_into(">I", broken, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                Program(broken)
        no_end = bytearray(source)
        no_end[65] &= 0xFE
        with self.assertRaisesRegex(ValueError, "no end"):
            Program(no_end)


if __name__ == "__main__":
    unittest.main()
