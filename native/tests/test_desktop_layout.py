"""Multi-monitor wallpaper layout (plasma/DesktopLayout.h). Set MOYOU_DESKTOP_LAYOUT_TEST to
the built moyou-desktop-layout-test."""
import os
import subprocess
import unittest


@unittest.skipUnless(os.environ.get("MOYOU_DESKTOP_LAYOUT_TEST"), "MOYOU_DESKTOP_LAYOUT_TEST not set")
class DesktopLayoutTest(unittest.TestCase):
    def test_physical_layout(self):
        result = subprocess.run([os.environ["MOYOU_DESKTOP_LAYOUT_TEST"]], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
