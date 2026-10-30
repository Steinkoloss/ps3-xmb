import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_checkpoint import compare


def checkpoint(root, payload=b"\x00\x7f\x80\xff", **changes):
    root.mkdir(exist_ok=True)
    (root / "frame.rgba").write_bytes(payload)
    document = {
        "schema": "moyou.checkpoint.v1", "firmware_sha256": "a" * 64,
        "inputs_sha256": "b" * 64, "frame": 123,
        "buffers": {"final": {"path": "frame.rgba", "format": "rgba8",
                              "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                              "width": 1, "height": 1, "stride": 4}},
    }
    document.update(changes)
    path = root / "checkpoint.json"
    path.write_text(json.dumps(document))
    return path


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_identical_output_passes_but_one_bit_of_visual_drift_fails(self):
        reference = checkpoint(self.root / "reference")
        candidate = checkpoint(self.root / "candidate")
        self.assertEqual(compare(reference, candidate), [])
        candidate = checkpoint(self.root / "candidate", b"\x00\x7f\x81\xff")
        differences = compare(reference, candidate)
        self.assertEqual(len(differences), 1)
        self.assertEqual(differences[0]["first_byte"], 2)

    def test_different_inputs_or_frame_are_never_compared_as_equivalent(self):
        reference = checkpoint(self.root / "reference")
        for change in ({"inputs_sha256": "c" * 64}, {"frame": 124}, {"firmware_sha256": "c" * 64}):
            candidate = checkpoint(self.root / "candidate", **change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                compare(reference, candidate)

    def test_modified_reference_and_wrong_channel_layout_are_rejected(self):
        reference = checkpoint(self.root / "reference")
        candidate = checkpoint(self.root / "candidate")
        (reference.parent / "frame.rgba").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "modified"):
            compare(reference, candidate)
        reference = checkpoint(reference.parent)
        document = json.loads(candidate.read_text())
        document["buffers"]["final"]["format"] = "bgra8"
        candidate.write_text(json.dumps(document))
        with self.assertRaisesRegex(ValueError, "layouts"):
            compare(reference, candidate)


if __name__ == "__main__":
    unittest.main()
