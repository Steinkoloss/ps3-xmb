"""Optional integration checks against local, unmodified firmware inputs.

MOYOU_REFERENCE=/path/to/import python3 -m unittest discover -s tests -v
No original firmware bytes or shader translations are stored in the test tree.
"""

import collections
import hashlib
import json
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from cg import Program
from import_firmware import PARTICLES_SHA256, PUP_SHA256, RENDERER_SHA256, SPLINE_SHA256
from qrc import resources
from spu_elf import load


@unittest.skipUnless(os.environ.get("MOYOU_REFERENCE"), "Set MOYOU_REFERENCE to the local firmware import")
class OriginalInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(os.environ["MOYOU_REFERENCE"])
        cls.manifest = json.loads((cls.root / "manifest.json").read_text())

    def test_full_inventory_and_simulation_images_match_verified_firmware(self):
        self.assertEqual(self.manifest["schema"], "moyou.original-inputs.v1")
        self.assertEqual(self.manifest["firmware_sha256"], PUP_SHA256)
        files = self.manifest["files"]
        self.assertEqual(len(files), 162)
        for name, expected in files.items():
            with self.subTest(name=name):
                data = (self.root / name).read_bytes()
                self.assertEqual(len(data), expected["size"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), expected["sha256"])
        for name, digest in {
            "custom_render_plugin.elf": RENDERER_SHA256,
            "lines/spurs/moyou/spline/spline.elf": SPLINE_SHA256,
            "lines/spurs/particles/particles/particles.elf": PARTICLES_SHA256,
        }.items():
            self.assertEqual(files[name]["sha256"], digest)

    def test_container_resources_are_preserved_without_conversion(self):
        original = dict(resources((self.root / "original/lines.qrc").read_bytes()))
        self.assertEqual(len(original), 159)
        for name, data in original.items():
            with self.subTest(name=name):
                self.assertEqual(data, (self.root / "lines" / name).read_bytes())

    def test_all_original_shaders_decode_with_parameter_patches_intact(self):
        inventory = collections.Counter()
        for name in self.manifest["files"]:
            if not name.endswith((".fpo", ".vpo")):
                continue
            with self.subTest(name=name):
                program = Program((self.root / name).read_bytes())
                inventory[program.profile] += 1
                self.assertTrue(program.instructions)
                self.assertEqual(program.domain["instruction_slots"], len(program.code) // 16)
                for parameter in program.parameters:
                    for offset in parameter.constant_offsets:
                        self.assertTrue(any(i.offset + 16 == offset and i.constant_bits
                                            for i in program.instructions))
        self.assertEqual(inventory, {7003: 11, 7004: 47})

    def test_simulation_sections_keep_original_code_separate_from_resource_data(self):
        for name, end in (("moyou/spline/spline", 0x8758), ("particles/particles/particles", 0xA9E8)):
            with self.subTest(name=name):
                data = (self.root / f"lines/spurs/{name}.elf").read_bytes()
                image = load(data)
                self.assertEqual(image.entry, 0x3070)
                self.assertEqual(len(image.memory), 256 * 1024)
                self.assertEqual(image.executable[-1][1], end)
                for length in (0, 51, len(data) - 1):
                    with self.subTest(length=length), self.assertRaises(ValueError):
                        load(data[:length])


if __name__ == "__main__":
    unittest.main()
