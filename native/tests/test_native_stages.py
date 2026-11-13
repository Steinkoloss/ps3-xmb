"""Native ports against the translated original, in the live scene (MOYOU_VERIFY_NATIVE)."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

REQUIRED = ("MOYOU_PREVIEW", "MOYOU_NATIVE_SHADERS", "MOYOU_REFERENCE", "MOYOU_CPU_CORE", "MOYOU_NATIVE_BASELINE")


@unittest.skipUnless(all(os.environ.get(name) for name in REQUIRED), "Set the live-scene build and original data")
class NativeStageTests(unittest.TestCase):
    def run_verified(self, frames, **extra):
        with tempfile.TemporaryDirectory() as temporary:
            environment = dict(os.environ, MOYOU_VERIFY_NATIVE="1", QT_QPA_PLATFORM="offscreen", **extra)
            result = subprocess.run([str(Path(os.environ["MOYOU_PREVIEW"]).resolve()),
                "--shaders", os.environ["MOYOU_NATIVE_SHADERS"], "--reference", os.environ["MOYOU_REFERENCE"],
                "--cpu", os.environ["MOYOU_CPU_CORE"],
                "--spline", str(Path(os.environ["MOYOU_NATIVE_BASELINE"]) / "spline/original-ls.bin"),
                "--preset", "day", "--frames", str(frames), "--width", "64", "--height", "36",
                "--output", str(Path(temporary) / "frame.png")],
                capture_output=True, text=True, timeout=600, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        return result

    def test_particle_frame_matches_the_original_under_controller_input(self):
        # Random motion and D-pad tilts reach every interaction handler branch.
        result = self.run_verified(900, MOYOU_EXERCISE_INPUT="1")
        self.assertNotIn("bytes differ", result.stderr)

    def test_native_particles_and_surface_match_the_original_every_update(self):
        result = self.run_verified(240)
        self.assertNotIn("bytes differ", result.stderr)
        differences = [float(value.replace(",", ".")) for value in
                       re.findall(r"largest relative difference ([0-9.,e+-]+)", result.stderr)]
        self.assertGreater(len(differences), 0)
        self.assertLessEqual(max(differences), 1e-4)


if __name__ == "__main__":
    unittest.main()
