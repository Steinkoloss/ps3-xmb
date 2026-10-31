"""Run public ISA conformance vectors on the native graphics path.

Set MOYOU_PREVIEW to the locally built native executable. These tiny programs
contain no firmware code. Their pixel results check register aliasing, masks,
predication and simultaneous vertex ALU reads on the actual graphics driver.
"""

import dataclasses
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cg import Program
from glsl_fragment import translate as fragment_glsl
from glsl_vertex import translate as vertex_glsl


def fp_mov(values, *, half=False, mask=15, end=True, predicate=7, condition=False, no_destination=False):
    dest = (1 << 24) | (mask << 9) | (int(half) << 7) | int(end)
    dest |= int(condition) << 8 | int(no_destination) << 30
    source = 2 | (predicate << 18) | (1 << 11) | (2 << 13) | (3 << 15)
    source |= (1 << 23) | (2 << 25) | (3 << 27)
    words = [dest, source, 0, 0, *values]
    return b"".join(struct.pack(">I", (word >> 16) | ((word & 65535) << 16)) for word in words)


def fragment(code):
    domain = struct.pack(">IIIHHHBBBB", len(code) // 16, 0, 0, 0, 65535, 0, 2, 1, 0, 0) + bytes(2)
    return Program(struct.pack(">8I", 7004, 6, 64 + len(code), 0, 32, 32, len(code), 64) + domain + bytes(8) + code)


def vp_instruction(vector, source0, *, source2=0, scalar=0, temp=63, scalar_temp=63,
                   output=31, owns_output=False, vector_mask=15, scalar_mask=0, end=False):
    d0 = temp << 15 | int(owns_output) << 30
    d1 = source0 >> 9 | vector << 22 | scalar << 27
    d2 = (source0 & 511) << 23 | source2 >> 11
    d3 = (source2 & 2047) << 21 | scalar_mask << 17 | vector_mask << 13
    d3 |= scalar_temp << 7 | output << 2 | int(end)
    return struct.pack(">4I", d0, d1, d2, d3)


def vertex(code, constant=False):
    domain = struct.pack(">6I", len(code) // 16, 0, 2, 1, 1, 0)
    if not constant:
        return Program(struct.pack(">8I", 7003, 6, 64 + len(code), 0, 32, 32, len(code), 64) + domain + bytes(8) + code)
    parameter = struct.pack(">12I", 1048, 2178, 4103, 0, 104, 112, 0, 0, 4097, 0, 1, 0)
    header = struct.pack(">8I", 7003, 6, 128 + len(code), 1, 32, 80, len(code), 128)
    return Program(header + parameter + domain + b"old\0" + bytes(4) + struct.pack(">4f", 2, 2, 2, 2) + code)


def position_program():
    return vertex(vp_instruction(1, 0x1B02, output=0, owns_output=True, end=True))


@unittest.skipUnless(os.environ.get("MOYOU_PREVIEW"), "Set MOYOU_PREVIEW to run native GPU conformance")
class GpuProgramTests(unittest.TestCase):
    def render(self, vp, fp):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            programs = {}
            for name, program, emit in (("test.vpo", vp, vertex_glsl), ("test.fpo", fp, fragment_glsl)):
                target = "test.vert" if program.profile == 7003 else "test.frag"
                source = emit(program)
                (root / target).write_text(source)
                programs[name] = {
                    "glsl": target, "domain": program.domain,
                    "glsl_sha256": hashlib.sha256(source.encode()).hexdigest(),
                    "parameters": [dataclasses.asdict(p) for p in program.parameters],
                    "constants": {str(i.offset // 16 + 1): i.constant_bits for i in program.instructions
                                  if getattr(i, "constant_bits", ())},
                }
            (root / "manifest.json").write_text(json.dumps({"schema": "moyou.native-shaders.v1", "programs": programs}))
            (root / "graph.json").write_text(json.dumps({"schema": "moyou.render-graph.v1", "passes": [
                {"name": "ISA conformance", "vertex": "test.vpo", "fragment": "test.fpo"},
            ]}))
            binary = str(Path(os.environ["MOYOU_PREVIEW"]).resolve())
            result = subprocess.run([binary, "--shaders", str(root), "--graph", str(root / "graph.json"),
                                     "--output", str(root / "pixel.png"), "--width", "1", "--height", "1"],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            return Image.open(root / "pixel.png").convert("RGBA").getpixel((0, 0))

    def test_full_register_words_are_visible_through_their_half_register_aliases(self):
        packed_halves = [0x38003400, 0x3C003A00, 0, 0]
        pixel = self.render(position_program(), fragment(fp_mov(packed_halves)))
        self.assertEqual(pixel, (64, 128, 191, 255))

    def test_half_write_mask_preserves_the_other_packed_components(self):
        initial = fp_mov([0x38003400, 0x3C003A00, 0, 0], end=False)
        update = fp_mov([0x3F000000] * 4, half=True, mask=1)
        self.assertEqual(self.render(position_program(), fragment(initial + update)), (128, 128, 191, 255))

    def test_condition_register_predicates_select_each_lane_independently(self):
        initial = fp_mov([0x38003400, 0x3C003A00, 0, 0], end=False)
        condition = fp_mov([0xBF800000, 0, 0x3F800000, 0x80000000], end=False, condition=True, no_destination=True)
        update = fp_mov([0x3F000000] * 4, half=True, predicate=1)
        self.assertEqual(self.render(position_program(), fragment(initial + condition + update)), (128, 128, 191, 255))

    def test_coissued_vertex_units_read_the_state_before_either_unit_writes(self):
        code = vp_instruction(1, 0x1B03, temp=0)
        code += vp_instruction(1, 0xFF02, source2=1, scalar=2, temp=0, scalar_temp=1, vector_mask=8, scalar_mask=8)
        code += vp_instruction(1, 5, output=1, owns_output=True)
        code += vp_instruction(1, 0x1B02, output=0, owns_output=True, end=True)
        vp = vertex(code, constant=True)
        dest = 1 << 24 | 1 << 13 | 15 << 9 | 1 << 7 | 1
        source = 1 | 7 << 18
        words = (dest, source, 0, 0)
        fp = fragment(b"".join(struct.pack(">I", (w >> 16) | ((w & 65535) << 16)) for w in words))
        self.assertEqual(self.render(vp, fp), (128, 128, 128, 128))


if __name__ == "__main__":
    unittest.main()
