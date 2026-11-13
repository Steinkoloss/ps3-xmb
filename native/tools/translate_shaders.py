#!/usr/bin/env python3
"""Translate the verified firmware shaders for the native OpenGL renderer.

  python3 tools/translate_shaders.py /path/to/reference --output /path/to/generated

The generated GLSL is derived from original firmware and stays local. Translation
does not certify a rendered frame: bindings, precision and the complete pipeline
also need verification. Unsupported original instructions stop this command.
"""

import argparse
import dataclasses
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from cg import Program
from glsl_fragment import translate as fragment
from glsl_vertex import translate as vertex
from import_firmware import PUP_SHA256


def translate(reference, output):
    if output.exists():
        raise FileExistsError("Use a new directory to preserve the generated baseline")
    manifest = json.loads((reference / "manifest.json").read_text())
    if manifest["firmware_sha256"] != PUP_SHA256:
        raise ValueError("Expected the verified original firmware import")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    result = {"schema": "moyou.native-shaders.v1", "firmware_sha256": PUP_SHA256, "programs": {}}
    with tempfile.TemporaryDirectory(prefix=".moyou-glsl-", dir=output.parent) as temporary:
        stage = Path(temporary) / "shaders"
        stage.mkdir()
        for name, identity in manifest["files"].items():
            if not name.endswith((".vpo", ".fpo")):
                continue
            data = (reference / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != identity["sha256"]:
                raise ValueError(f"Modified original shader: {name}")
            program = Program(data)
            sampler_types = {p.resource - 2048: p.type for p in program.parameters if 2048 <= p.resource <= 2063}
            suffix = ".vert" if program.profile == 7003 else ".frag"
            target = Path(name).with_suffix(suffix)
            destination = stage / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text((vertex if program.profile == 7003 else fragment)(program), newline="\n")
            # Development check: compile with glslangValidator when it is installed.
            checked = subprocess.run(["glslangValidator", str(destination)], capture_output=True, text=True) \
                if shutil.which("glslangValidator") else None
            if checked and checked.returncode:
                raise ValueError(f"Translated original shader did not compile: {name}\n{checked.stdout}{checked.stderr}")
            result["programs"][name] = {
                "source_sha256": identity["sha256"], "glsl": target.as_posix(),
                "glsl_sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
                "domain": program.domain,
                "samplers": {str((i.words[0] >> 17) & 15): sampler_types[(i.words[0] >> 17) & 15]
                             for i in program.instructions if getattr(i, "mnemonic", "") == "TEX"},
                "parameters": [dataclasses.asdict(p) for p in program.parameters],
                "constants": {str(i.offset // 16 + 1): i.constant_bits for i in program.instructions
                              if getattr(i, "constant_bits", ())},
            }
        (stage / "manifest.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", newline="\n")
        os.rename(stage, output)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = translate(args.reference.resolve(), args.output)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Original shader translation failed: {error}\n")
    print(f"Translated and compiled {len(result['programs'])} original RSX programs")


if __name__ == "__main__":
    main()
