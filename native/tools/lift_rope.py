#!/usr/bin/env python3
"""Lift only the reachable original CPU rope and firmware math instructions.

Inputs are locally relocated/decrypted firmware images, with function ranges
recovered by PrepareMoyou.java / ExportMoyou.java. Source hashes pin both images.
Generated sources and images must remain in the ignored local build directory.
The GPU FFD submission is an explicit rendering handoff, not part of this core.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from lift_simulation import REVISION


RENDERER_IMAGE_SHA256 = "31b3c6fe7b315f94ecd7927028c5e1bac5e6d1c599b2cd8a5a62660ee13e38e4"
# The VSH image has a zero-filled 64 KiB low page and LOADs at original VAs.
MATH_IMAGE_SHA256 = "6de555381a6f8c760a7b2618177d3fe6373fbdb4211ffdd4f3591ea5d757f5ec"


def ranges(path):
    result = {}
    for line in path.read_text().splitlines():
        start, end = (int(word, 16) for word in line.split())
        if start % 4 or end % 4 or start >= end or start in result:
            raise ValueError("Invalid or duplicated original function boundary")
        result[start] = end
    return result


def lift(image, base, bounds, imports, roots, toc, code_end, prefix, stage):
    from ppu_disasm import disassemble_bytes
    from ppu_lifter import PPULifter
    lifter = PPULifter(prefix=prefix)
    lifter.function_entries = set(bounds)
    lifter.hle_stub_nids = imports
    lifter.toc_base, lifter.code_lo, lifter.code_hi = toc, base, code_end
    lifter.header_name = prefix + "program.h"
    pending, done = set(roots), set()
    while pending:
        start = min(pending)
        pending.remove(start)
        if start in done:
            continue
        end = bounds.get(start)
        if end is None or not base <= start < end <= base + len(image):
            raise ValueError(f"Missing verified function bounds at {start:#x}")
        instructions = disassemble_bytes(image[start - base:end - base], start)
        if any(instruction.mnemonic == "???" for instruction in instructions):
            raise ValueError(f"Undecoded original CPU instruction at {start:#x}")
        function = lifter.lift_function(instructions, start, end)
        done.add(start)
        pending |= (lifter.call_targets | lifter.branch_targets) - done
        if function.fallthrough_to:
            pending |= {function.fallthrough_to} - done
        if len(done) > 128:
            raise ValueError("CPU lift escaped the bounded rope/math closure")
    source = lifter.emit_source()
    if re.search(r"TODO|unsupported|unhandled|undecoded|non-code|unknown", source, re.I):
        raise ValueError("CPU lift contains an incomplete instruction or control-flow translation")
    # ps3recomp defaults to host sqrt here; the Cell estimate has specific bits.
    source = '#include "CellEstimate.h"\n' + source.replace("vmx_vrsqrtefp(", "moyou_vrsqrtefp(")
    (stage / lifter.header_name).write_text(lifter.emit_header())
    (stage / (prefix + "program.cpp")).write_text(source)
    return [{"start": f"0x{f.start_addr:08x}", "end": f"0x{f.end_addr:08x}"} for f in lifter.functions]


def prepare(args):
    recompiler = args.recompiler.resolve()
    revision = subprocess.check_output(["git", "-C", str(recompiler), "rev-parse", "HEAD"], text=True).strip()
    if revision != REVISION:
        raise ValueError("The recompiler does not match the pinned revision")
    subprocess.run(["git", "-C", recompiler, "diff", "--exit-code", REVISION, "--", "tools/ppu_lifter.py",
                    "tools/ppu_disasm.py", "runtime/ppu"], check=True, stdout=subprocess.DEVNULL)
    sys.path.insert(0, str(recompiler / "tools"))
    renderer, math = args.renderer.read_bytes(), args.math.read_bytes()
    if hashlib.sha256(renderer).hexdigest() != RENDERER_IMAGE_SHA256:
        raise ValueError("Modified or incorrectly relocated original renderer image")
    if hashlib.sha256(math).hexdigest() != MATH_IMAGE_SHA256:
        raise ValueError("Modified or incorrectly mapped original VSH math image")
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError("Use a new output directory to preserve the baseline")
    imports = {int(address, 16): int(nid, 16)
               for library in json.loads(args.imports.read_text()).values() for nid, address in library.items()}
    if imports.get(0x020879c8) != 0x831d70a5 or imports.get(0x02087b50) != 0xb5e28191:
        raise ValueError("Unexpected original CPU import table")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".moyou-rope-", dir=output.parent) as temporary:
        stage = Path(temporary) / "cpu"
        stage.mkdir()
        manifest = {"schema": "moyou.native-rope.v1", "recompiler_revision": REVISION,
                    "renderer_image_sha256": RENDERER_IMAGE_SHA256, "math_image_sha256": MATH_IMAGE_SHA256}
        renderer_bounds = ranges(args.renderer_bounds)
        # Ghidra's A2ALT decoder stops at LVLX here. The verified original
        # routine continues to its BLR immediately before the next function.
        if renderer_bounds.get(0x02024228) == 0x020242ac:
            if renderer[0x243c0:0x243c4] != bytes.fromhex("4e800020"):
                raise ValueError("Original axis-angle function boundary changed")
            renderer_bounds[0x02024228] = 0x020243c4
        manifest["rope_functions"] = lift(renderer, 0x02000000, renderer_bounds,
            imports | {0x0204c4e4: 0xffff0002, 0x0204b8e8: 0xffff0003, 0x0204b49c: 0xffff0004},
            [0x0204bed0,0x0204f814,0x02029cc8,0x0203f290,0x0204bad0,0x0204b928,0x02071794,0x02071384], 0x020a7d70,
            0x0208bc70, "moyou_rope_", stage)
        manifest["math_functions"] = lift(math, 0, ranges(args.math_bounds),
            {0x00095b44: 0xffff0001}, [0x00095f60,0x00093f94,0x00093b38,0x00092d34,0x00092e90],
            0x006f5558, 0x00635c08, "moyou_math_", stage)
        # Native integration includes one shared context declaration per TU.
        shutil.copyfile(stage / "moyou_rope_program.h", stage / "rope_program.h")
        header = "#pragma once\n#include <cstdint>\n"
        for name, image in (("renderer", renderer), ("math", math)):
            digest = hashlib.sha256(image).digest()
            header += f"constexpr unsigned {name}_image_size = {len(image)};\n"
            header += f"constexpr uint8_t {name}_image_digest[32] = {{" + ",".join(map(str, digest)) + "};\n"
            (stage / (name + "-image.bin")).write_bytes(image)
        (stage / "rope_digests.h").write_text(header)
        shutil.copyfile(recompiler / "runtime/ppu/ppu_vmx.h", stage / "ppu_vmx.h")
        shutil.copyfile(recompiler / "LICENSE", stage / "ps3recomp-LICENSE")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        os.rename(stage, output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("renderer", "math", "renderer-bounds", "math-bounds", "imports", "recompiler", "output"):
        parser.add_argument("--" + argument, type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = prepare(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Original CPU lift failed: {error}\n")
    print(f"Translated {len(manifest['rope_functions'])} rope and {len(manifest['math_functions'])} math functions")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
