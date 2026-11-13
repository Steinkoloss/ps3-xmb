#!/usr/bin/env python3
"""Development: print lifted PPU functions as annotated assembly, for hand porting.

  ppu_listing.py WORK ADDRESS [--depth N]

WORK is the build/work directory (cpu-particles manifest and images). Prints ADDRESS and the
functions it calls (to --depth), with TOC/absolute constants resolved to floats where they
point into the image. Reads firmware-derived files; keep its output local.
"""
import argparse, json, struct, sys
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument("work", type=Path); p.add_argument("address"); p.add_argument("--depth", type=int, default=1)
    p.add_argument("--recompiler", type=Path, default=None)
    a = p.parse_args()
    root = a.recompiler or a.work.parent / "third_party" / "ps3recomp"
    sys.path.insert(0, str(root / "tools"))
    from ppu_disasm import decode
    cpu = a.work / "cpu-particles"
    manifest = json.loads((cpu / "manifest.json").read_text())
    image = (cpu / "renderer-image.bin").read_bytes()
    base = 0x02000000
    bounds = {int(f["start"], 16): int(f["end"], 16) for f in manifest["rope_functions"]}
    imports = {}
    imp = a.work / "lifted" / "renderer" / "custom_render_plugin_imports.json"
    if imp.exists():
        data = json.loads(imp.read_text())
        def walk(o):
            if isinstance(o, dict):
                for k, v in o.items():
                    if isinstance(v, str) and k.upper().startswith("0X") and v.upper().startswith("0X"): imports[int(v, 16)] = k
                    else: walk(v)
            elif isinstance(o, list):
                for v in o: walk(v)
        walk(data)
    toc = 0x020a7d70
    def word(addr): return struct.unpack(">I", image[addr - base:addr - base + 4])[0]
    def floatat(addr):
        if base <= addr < base + len(image) - 4:
            w = word(addr); return f"{struct.unpack('>f', struct.pack('>I', w))[0]:.9g} (0x{w:08x})"
        return None
    seen = set()
    def show(start, depth):
        if start in seen: return
        seen.add(start)
        end = bounds.get(start)
        if end is None: print(f"\n== {start:#x}: no bounds"); return
        print(f"\n== {start:#010x} .. {end:#010x} ({(end - start) // 4} instructions)")
        calls, hi = [], {}
        for addr in range(start, end, 4):
            ins = decode(word(addr), addr); text = str(ins).split("  ", 2)[-1].strip()
            note = ""
            mn = text.split()[0] if text else ""
            ops = text[len(mn):].replace(" ", "").split(",")
            if mn == "lis": hi[ops[0]] = int(ops[1], 0) << 16
            if mn in ("lfs", "lfd", "lwz", "addi") and len(ops) >= 2 and "(" in ops[-1] or mn == "addi":
                try:
                    if mn == "addi":
                        reg, src, imm = ops[0], ops[1], int(ops[2], 0)
                    else:
                        reg = ops[0]; off, src = ops[1].split("("); src = src.rstrip(")"); imm = int(off, 0)
                    if src == "r2": target = toc + imm
                    elif src in hi: target = (hi[src] + imm) & 0xffffffff
                    else: target = None
                    if target is not None:
                        f = floatat(target) if mn != "addi" else None
                        note = f"  ; [{target:#x}]" + (f" = {f}" if f else "")
                except (ValueError, IndexError): pass
            if mn == "bl":
                t = int(ops[0], 16) if ops[0].startswith("0x") else None
                if t is not None:
                    calls.append(t); note = f"  ; -> {imports.get(t, '')}"
            print(f"{addr:08x}: {text}{note}")
        if depth > 0:
            for t in dict.fromkeys(calls):
                if t not in imports: show(t, depth - 1)
    show(int(a.address, 16), a.depth)

if __name__ == "__main__":
    main()
