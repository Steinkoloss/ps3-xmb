"""Check the native SPU API, surface layout and complete patch coverage."""
import math
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(os.environ.get("MOYOU_SURFACE_CHECKPOINT") and os.environ.get("MOYOU_NATIVE_BASELINE"),
                     "Set the native surface executable and original SPU build")
class SurfaceTests(unittest.TestCase):
    def render(self, points):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "controls.f32").write_bytes(b"".join(struct.pack("<4f", *p) for p in points))
            result = subprocess.run([str(Path(os.environ["MOYOU_SURFACE_CHECKPOINT"]).resolve()),
                "--original", str(Path(os.environ["MOYOU_NATIVE_BASELINE"]).resolve() / "spline/original-ls.bin"),
                "--input", str(root / "controls.f32"), "--output", str(root / "surface.f32")],
                capture_output=True, text=True, timeout=10)
            output = root / "surface.f32"
            return result, output.read_bytes() if output.exists() else None

    def test_original_surface_covers_every_patch_with_correct_vector_layout_and_normal_orientation(self):
        result, data = self.render((x, y, 0, 1) for y in range(19) for x in range(19))
        self.assertEqual(result.returncode, 0, result.stderr)
        vertices = list(struct.iter_unpack("<8f", data))
        self.assertEqual(len(vertices), 16384)
        # The original kernel uses 16/127 inside each eight-sample patch.
        for y in range(128):
            for x in range(128):
                p = vertices[y * 128 + x]
                self.assertAlmostEqual(p[0], 1 + x // 8 + (x % 8) * 16 / 127, delta=0.0001)
                self.assertAlmostEqual(p[1], 1 + y // 8 + (y % 8) * 16 / 127, delta=0.0001)
                for actual, expected in zip(p[2:], (0,1,0,0,-1,0)):
                    self.assertAlmostEqual(actual, expected, delta=0.0001)
        self.assertTrue(all(math.isfinite(value) for vertex in vertices for value in vertex))

    def test_nonfinite_or_truncated_control_grids_cannot_become_surface_checkpoints(self):
        result, output = self.render([(0,0,0,1)] * 360)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(output)
        result, output = self.render([(float("nan"),0,0,1)] * 361)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(output)


if __name__ == "__main__":
    unittest.main()
