#!/usr/bin/env python3
"""Translate and compile the original SPU simulation programs for local x86 replay.

Requires ps3recomp at revision 4e8f3ca87a3e1d87bb83b8b7e9348fe2685cd514:
  https://github.com/sp00nznet/ps3recomp (MIT)
Build dependencies: Python 3, Git, Clang, OpenSSL development headers/library.

  python3 tools/lift_simulation.py /path/to/reference --recompiler /path/to/ps3recomp \
    --output /path/to/local-build

Generated translations contain firmware code; keep the output local and outside
version control. The replay program runs subroutine checkpoints, not the full
renderer or SPURS task environment. Successful compilation does not establish
simulation or visual equivalence. No optimization is enabled in this baseline.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from import_firmware import PARTICLES_SHA256, SPLINE_SHA256
from spu_elf import load


REVISION = "4e8f3ca87a3e1d87bb83b8b7e9348fe2685cd514"
PROGRAMS = {
    "spline": ("lines/spurs/moyou/spline/spline.elf", SPLINE_SHA256),
    "particles": ("lines/spurs/particles/particles/particles.elf", PARTICLES_SHA256),
}


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def prepare(reference, recompiler, output, compiler):
    reference, recompiler, output = reference.resolve(), recompiler.resolve(), output.resolve()
    if output.exists():
        raise FileExistsError("Use a new output directory to preserve the native baseline")
    revision = run(["git", "-C", recompiler, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if revision != REVISION:
        raise ValueError("The recompiler does not match the pinned, inspected revision")
    run(["git", "-C", recompiler, "diff", "--exit-code", REVISION, "--", "tools/spu_lifter.py",
         "tools/spu_disasm.py", "tools/find_spu_functions.py", "runtime/spu", "include/ps3emu"],
        stdout=subprocess.DEVNULL)
    manifest = {"schema": "moyou.native-spu.v1", "recompiler_revision": REVISION, "programs": {}}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".moyou-lift-", dir=output.parent) as temporary:
        stage = Path(temporary) / "native"
        stage.mkdir()
        image_digests = []
        for name, (filename, expected) in PROGRAMS.items():
            source = reference / filename
            original = source.read_bytes()
            if hashlib.sha256(original).hexdigest() != expected:
                raise ValueError(f"Modified original simulation program: {name}")
            image = load(original)
            destination = stage / name
            run([sys.executable, recompiler / "tools/spu_lifter.py", "--auto-functions", source,
                 "--code-end", hex(max(end for _, end in image.executable)), "--output", destination,
                 "--report", destination / "report.json", "--symbol-prefix", f"moyou_{name}_", "--trace"])
            report = json.loads((destination / "report.json").read_text())
            if report["unsupported"]:
                raise ValueError(f"Unsupported original instructions: {name}/{report['unsupported']}")
            for start, end in report["functions"]:
                if not any(lo <= start < end <= hi for lo, hi in image.executable):
                    raise ValueError(f"Translation crosses an original code/data boundary: {name}/{start:#x}")
            for path in (destination / "spu_recomp.c", destination / "spu_recomp.h"):
                source_text = path.read_text()
                path.write_text(source_text.replace('"../../runtime/spu/', '"spu/'))
            (destination / "original-ls.bin").write_bytes(image.memory)
            image_digests.append(hashlib.sha256(image.memory).digest())
            manifest["programs"][name] = {"sha256": expected, "entry": image.entry,
                                          "local_store_sha256": image_digests[-1].hex(),
                                          "executable": image.executable,
                                          "functions": len(report["functions"]), "unsupported": 0}
        (stage / "program_digests.h").write_text(
            "static const unsigned char program_digests[2][32] = {\n" +
            ",\n".join("{" + ",".join(str(byte) for byte in digest) + "}" for digest in image_digests) + "\n};\n")
        binary = stage / "moyou-spu-replay"
        flags = ["-std=gnu17", "-O0", "-g", "-DSPU_SCALAR_HELPERS", "-DSPU_SCALAR_INT",
                 "-ffp-contract=off", "-fno-fast-math", "-fwrapv", "-fno-strict-aliasing"]
        command = [compiler, *flags,
                   "-I", recompiler / "runtime", "-I", recompiler / "include", "-I", stage,
                   Path(__file__).resolve().parents[1] / "native/spu_replay.c",
                   *[stage / name / "spu_recomp.c" for name in PROGRAMS], "-lm", "-lcrypto", "-o", binary]
        run(command)
        manifest["compiler"] = run([compiler, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
        manifest["compiler_flags"] = flags
        manifest["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        shutil.copyfile(recompiler / "LICENSE", stage / "ps3recomp-LICENSE")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        os.rename(stage, output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang")
    args = parser.parse_args()
    try:
        manifest = prepare(args.reference, args.recompiler, args.output, args.compiler)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Native simulation preparation failed: {error}\n")
    print(f"Compiled original simulation programs without optimization: {manifest['programs']}")
    print(args.output.resolve() / "moyou-spu-replay")


if __name__ == "__main__":
    main()
