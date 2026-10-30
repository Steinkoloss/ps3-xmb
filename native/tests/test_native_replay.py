"""Native replay checks use synthetic inputs to unmodified original subroutines.

These exercise original instruction lowering, calls and byte order. They do not
assert that an original complete simulation frame has been reproduced.
Set MOYOU_NATIVE_BASELINE to the output directory of lift_simulation.py.
"""

import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


LS_OFFSET = 32 + 128 * 16


@unittest.skipUnless(os.environ.get("MOYOU_NATIVE_BASELINE"), "Set MOYOU_NATIVE_BASELINE to the native build")
class NativeReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(os.environ["MOYOU_NATIVE_BASELINE"]).resolve()
        cls.original = (cls.build / "spline/original-ls.bin").read_bytes()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def snapshot(self, entry, budget=1000):
        data = bytearray(struct.pack(">8sIIQII", b"MOYOUI01", entry, 0x2FF0, budget, 0, 0))
        data += bytes(128 * 16) + self.original
        self.register(data, 0, 0x2FF0)
        self.register(data, 1, 0x3F000)
        return data

    @staticmethod
    def register(data, number, *lanes):
        struct.pack_into(">4I", data, 32 + number * 16, *(list(lanes) + [0] * (4 - len(lanes))))

    def replay(self, data):
        source, output = self.root / "input.snapshot", self.root / "output.snapshot"
        source.write_bytes(data)
        result = subprocess.run([
            self.build / "moyou-spu-replay", "spline", self.build / "spline/original-ls.bin", source, output,
        ], capture_output=True, text=True, timeout=5)
        return result, output

    def test_original_vector_multiply_runs_as_native_code(self):
        data = self.snapshot(0x3370)
        self.register(data, 3, 0x10000)
        self.register(data, 4, 0x40000000)
        struct.pack_into(">4f", data, LS_OFFSET + 0x10000, 1, -1, 0.25, 0)
        result, output = self.replay(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = output.read_bytes()
        self.assertEqual(actual[:8], b"MOYOUO01")
        self.assertEqual(struct.unpack_from(">I", actual, 8)[0], 0x2FF0)
        self.assertEqual(struct.unpack_from(">4f", actual, 32 + 3 * 16), (2, -2, 0.5, 0))

    def test_original_nested_calls_write_all_matrix_lanes_in_original_order(self):
        data = self.snapshot(0x3390)
        self.register(data, 3, 0x11000)
        self.register(data, 4, 0x10000)
        self.register(data, 5, 0x3F000000)
        values = tuple(float(index - 8) for index in range(16))
        struct.pack_into(">16f", data, LS_OFFSET + 0x10000, *values)
        result, output = self.replay(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = output.read_bytes()
        self.assertEqual(struct.unpack_from(">16f", actual, LS_OFFSET + 0x11000), tuple(v * 0.5 for v in values))
        self.assertEqual(struct.unpack_from(">I", actual, 32 + 16)[0], 0x3F000)

    def test_changed_instruction_and_exhausted_budget_never_emit_checkpoints(self):
        data = self.snapshot(0x3370)
        data[LS_OFFSET + 0x3370] ^= 1
        result, output = self.replay(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("code changed", result.stderr)
        self.assertFalse(output.exists())
        data = self.snapshot(0x3370, budget=1)
        result, output = self.replay(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("budget exceeded", result.stderr)
        self.assertFalse(output.exists())

    def test_truncated_snapshot_and_untranslated_entry_are_rejected(self):
        result, output = self.replay(self.snapshot(0x3370)[:-1])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        result, output = self.replay(self.snapshot(0x2FF4))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no verified native translation", result.stderr)
        self.assertFalse(output.exists())

    def test_the_original_image_cannot_be_replaced_to_hide_changed_code(self):
        data = self.snapshot(0x3370)
        source, output = self.root / "input.snapshot", self.root / "output.snapshot"
        source.write_bytes(data)
        changed = bytearray(self.original)
        changed[0x3370] ^= 1
        image = self.root / "changed-ls.bin"
        image.write_bytes(changed)
        result = subprocess.run([
            self.build / "moyou-spu-replay", "spline", image, source, output,
        ], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the compiled program", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
