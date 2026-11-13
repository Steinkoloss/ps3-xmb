"""Original CPU frame invariants; these do not certify complete rendered fidelity."""
import math
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(all(os.environ.get(k) for k in ("MOYOU_CPU_CORE", "MOYOU_ROPE_CHECKPOINT", "MOYOU_REFERENCE")),
                     "Set the local CPU translation, checkpoint executable and original reference")
class RopeTests(unittest.TestCase):
    def setUp(self):
        self.core = Path(os.environ["MOYOU_CPU_CORE"]).resolve()
        self.reference = Path(os.environ["MOYOU_REFERENCE"]).resolve()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def frame(self, number, renderer=None, settings=None):
        output = self.root / f"frame-{number}.f32"
        result = subprocess.run([str(Path(os.environ["MOYOU_ROPE_CHECKPOINT"]).resolve()),
            "--renderer", str(renderer or self.core / "renderer-image.bin"),
            "--math", str(self.core / "math-image.bin"),
            "--settings", str(settings or self.reference / "lines/LINE1.mnu"),
            "--frames", str(number), "--output", str(output)], capture_output=True, text=True, timeout=20)
        return result, output.read_bytes() if output.exists() else None

    def test_starting_grid_is_the_shipped_original_state_including_homogeneous_lanes(self):
        result, actual = self.frame(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        image = (self.core / "renderer-image.bin").read_bytes()
        original = struct.iter_unpack(">3f", image[0x97c18:0x97c18 + 361 * 12])
        expected = b"".join(struct.pack("<4f", *point, 1) for point in original)
        self.assertEqual(actual, expected)

    def test_original_accumulator_and_rng_advance_at_their_original_boundaries(self):
        result, first = self.frame(1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("random state 0", result.stdout)
        result, second = self.frame(2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("random state 1083", result.stdout)
        self.assertNotEqual(first, second)
        result, later = self.frame(62)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"random state {61 * 361 * 3}", result.stdout)
        points = list(struct.iter_unpack("<4f", later))
        self.assertEqual(len(points), 361)
        self.assertTrue(all(math.isfinite(v) for point in points for v in point))
        self.assertTrue(all(point[3] == 1 for point in points))

    def test_native_rope_matches_the_original_for_varying_intervals_and_the_clock_wrap(self):
        for extra in (["--jitter"], ["--jitter", "--start-time", "9.999"]):
            output = self.root / "native.bin"
            result = subprocess.run([str(Path(os.environ["MOYOU_ROPE_CHECKPOINT"]).resolve()),
                "--renderer", str(self.core / "renderer-image.bin"), "--math", str(self.core / "math-image.bin"),
                "--settings", str(self.reference / "lines/LINE1.mnu"),
                "--settings", str(self.reference / "lines/override/day/LINE1.mnu"),
                "--frames", "900", "--output", str(output), *extra],
                capture_output=True, text=True, timeout=600, env=dict(os.environ, MOYOU_VERIFY_NATIVE="1"))
            self.assertEqual(result.returncode, 0, result.stderr[-2000:])
            self.assertNotIn("bytes differ", result.stderr)

    def test_changed_original_code_and_missing_required_menu_data_fail_before_output(self):
        image = bytearray((self.core / "renderer-image.bin").read_bytes())
        image[0x4bed0] ^= 1
        changed = self.root / "changed.bin"
        changed.write_bytes(image)
        result, actual = self.frame(2, renderer=changed)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("integrity check", result.stderr)
        self.assertIsNone(actual)
        settings = self.root / "incomplete.mnu"
        settings.write_text("STATE:int:1\nTIMESTEP:float:4\n")
        result, actual = self.frame(2, settings=settings)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing required", result.stderr)
        self.assertIsNone(actual)


if __name__ == "__main__":
    unittest.main()
