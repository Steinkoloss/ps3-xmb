# Particles port — working notes

Status: simulated and drawn with the original programs. Not yet a visual match: too many
large out-of-focus (bokeh) particles compared with the console capture.

Drawing (0x02060660 → 0x0205c254, lifted): the original draw setup runs against unbound
uniform objects and their values are read back (vertex object offsets: MVP 0x30,
Modelview 0x90, Camera 0xf0, ParticleSize 0x150, LightPack 0x180, Transparency 0x1e0,
Focus 0x210, FocusCurves 0x240, FrontFacingQuaternion 0x270, NearControl 0x2a0,
LifeBoundsMin/Max 0x2d0/0x300, Darkness 0x330, Glare 0x360; fragment: LightPack 0x50,
Color 0xb0, Gamma 0xe0 (inferred), NearControl/Glare 0x100, IridescentExp 0x190).
qgl context at the unrelocated global 0x34 holds an identity modelview stack entry.
Records are float4 position+size, half4 rotation (attr 11), half4 old position (attr 10);
corners (0,0),(1,0),(1,1),(0,1) on attr 8; instanced quads, additive ONE/ONE into rgba16f
passes over the composition, before glare. Draw mask (sys+0x1484 + 1): bit0 quads,
bit1 second. Matrix uniforms go up as rows (transpose of vectormath memory) — the
untransposed LightPack renders nothing, MVP rows put particles on the ribbon.

Camera: the original camera (constructor 0x02024a68) has FOV 0.925 rad (53°), near 0.1,
far 1000, eye (0,0,2), target (0,0,-7), up (0,1,0) (constants at 0x02093730). The port had
the FOV but no view; now the ribbon is drawn with P·V·M, particles with MVP P·V and
Modelview V, the emission grid uses inv(P·V), and qgl 0xc2c9c741 is Matrix4::lookAt on the
camera vectors. With the view, particles fall in the _Focus band and the quads pass
matches the console capture.

Open: the second program (glints, draw mask bit 1, enabled by the firmware default
sys+0x15b4 = 2) draws ~15 large halos per frame; the console shows none that large. Its
size is exp2(specular term) × _Glare.y (5.44). Unverified inputs: LightPack orientation
(rows), the lighting space after the view change, and the SPE parameter block
(0x020327bc writes sys+0x15c0 words 0xe, 0x20, 0x27, 0x28, 0x36 from PARTICLES_SPE.mnu:
delta time, specular power, global alpha, size middle, glare) used by the SPU path.

What runs (RopeSolver, same guest VM as the rope):
- createParticles: original constructor 0x02060b14; render object ctor 0x0205e5c8 and
  vertex-array mapping 0x020598a0 are native HLE (0xffff0005/6) with two vertex buffers.
- particleParameters: native copy of ParticlesMenu apply 0x020329a4 (52 items).
- createParticleLayer/emitParticles: layer object with +0x18 system, +0x40 grid
  (data, 128, 128, column matrix at +0x50..0x8f), +0xb0 camera (identity basis); calls
  the original emitter 0x02030b80 (random grid points with emit prob/emit per frame,
  plus the row-walking trail emitter 0x0202dbf4).
- stepParticles: memcpy live block, dt at +0x1358, original PPU path 0x0205ddc8.
- OriginalSimulation feeds the SPU mesh with inverse(projection) as the grid matrix:
  firmware grid matrix is Model × f(lines+0x230) on lines-layer vertices; our mesh is
  clip space and inv(P)·clip is the same model-space point (rounding differs).

Known deviations: qgl random 0x2e67e2d8 and tick 0xd15d6a2c are deterministic stand-ins
(xorshift; per-emit counter), powf is host powf, PARTICLES_SPE.mnu (SPE menu) unused.

## Firmware structure (custom_render_plugin, image base 0x02000000, TOC 0x020a7d70)

ParticlesLayer, vtable 0x0209d4a8 (entries are OPD pointers; OPD word 0 = code):

| slot | code | role |
|---|---|---|
| 0 | 0x02030640 | ? |
| 1 | 0x02030134 | destructor |
| 2 | 0x0202df88 | init(width, height): allocates particle system (0x1980 B, capacity 0x800) via FUN_02060b14 |
| 5,7,8 | 0x0202b710, 0x0202b2d8, 0x0202b97c | render/state (unread) |
| 6 | 0x02031f44 | per frame: see below |

Constructor: FUN_020322b8 (refcounted scene/resource handles, lwmutex syscalls).
Menus (vtable 0x0209d4d8 family): ParticlesMenu 0x020332bc, PARTICLES_SPE 0x02034290
(delta time, glare, specular power, size middle, global alpha), parameter table with
PARTICLES.mnu names 0x02034664 (FUN_020225a4(min,max,...,dest) registers one value),
ParticlesInteractionMenu 0x02032c2c, ParticlesHackMenu 0x020366f4.

Per frame 0x02031f44(layer):
1. memcpy(sys+0x1360, sys+0x1490, 0x128): live parameter block from menu copy.
2. clamps/accumulates a few params (FUN_020030a0 = clamp).
3. reads camera/model matrices from the ribbon scene (layer+0x20 → … +0x230, +0x1f0).
4. FUN_0202b288(layer, …, matrices); FUN_02031494(dt, layer) — interaction modes
   (flags layer+0x1003..0x1005), animated values with callbacks at layer+0x10c/0x138/
   0x158/0x178, spot position into sys+0x1340, sys+0x1350. Ghidra truncates it at an
   LVLX (0x02031968).
5. FUN_02062464: SPU path (jobs via SPURS) — skip.
6. FUN_0205de2c(dt, sys): sys+0x1358 = dt; if sys+0x30 == 0 (no SPU manager) run the
   PPU path FUN_0205ddc8.
7. FUN_02060380: stats text only.

PPU path FUN_0205ddc8(sys): sys+0xa0 = sys+0xa4 = sys+0x60, then
- 0x0205ca80 clears the write list for buffer sys+0x16f4,
- 0x0205cad0 executes queued commands: count sys+0x1328, array sys+0x1334 (0x18 B each,
  type 1..4 → 0x0205bbf4 / 0x0205bbc0 / 0x02059140 / 0x0205934c(float)), default emitter
  sys+0x1354; sys+0x1900 = count / sys+0x132c,
- 0x0205ca00 picks vertex output: sys+0x1700 = aligned(*(sys+0x16f0 + idx*0xc + 0x1c)),
- 0x0205d7b0 update: particles at *(sys+0xa4)+0x10, count *(sys+0xa4)+0x14, 0x30 B each;
  live ones write 0x20 B (two vec4) at sys+0x1700, count in sys+0x16f8 / sys+0x1888,
- 0x0205c934 flips sys+0x16f4, sys+0x16fc = written count,
- 0x0205c988 stats.

SPU program (spurs/particles/particles.elf, entry 0x3070): job main 0x80e8; compute in
0x48b8, 0x5dd8, loop 0x52f8; DMA helpers 0x5ff0/0x62d0; channel use only 0x7xxx-0x9xxx,
0x52c8, 0x560c, 0x566c, 0xa940. The SPU path is what the console runs; the PPU path is
the firmware's own fallback for the same simulation.

Shaders: lines/lib/particles/particles_quads.{vpo,fpo}, particles_second.{vpo,fpo};
texture lines/particles/proc_iridescent.tga.

## Lifting

`tools/lift_rope.py` roots now include 0x02060b14 (system constructor) and 0x0205ddc8.
Output `build/work/cpu-particles` (304 renderer + 19 math functions); the rope output it
contains is bit-identical to cpu-glare's. Lifter fixes made for this closure:
- truncated Ghidra bounds (LVLX) extended to the next function when a BLR precedes it,
- call targets in unclaimed gaps become functions ending at a BLR,
- lis/addi-based switch tables (0x02058cb0) dispatch through ps3recomp's jump tables.

Imports the closure references (HLE needed only if reached): allocator 0x83b2cc6f
(new), 0xd84b3689 (delete), 0xed3da02b (memalign); stdc 0x1a00f889, 0x2d8be7e8,
0x2f45d39c, 0x5b162b7f, 0x683ca70a, 0xc7931798 (+ memcpy and math already handled);
sdk 0x97aeded8, 0xf80196c1; sysPrxForUser 0x8461e528; qgl_base 0x1e3764f7, 0x5262639a,
0x85bd0c33, 0x89c88658, 0x90e4802b, 0x9308c8c0, 0xad4384f2, 0xaddb8a27, 0xc59c1fe5,
0xd4fd336d, 0xf3b69a9a. 12 indirect calls (refcount releases) — ps3_indirect_call needs
an address → lifted-function table.

## Next steps

1. Emit the indirect-call table from lift_rope.py; implement ps3_indirect_call.
2. Drive the system: either run 0x02060b14 with HLE allocators/qgl stubs, or lay out
   the fields the update path reads. Then queue an emit command and step 0x0205ddc8,
   dump vertex output, check counts/positions are sane.
3. Parameter block 0x1490: map PARTICLES.mnu names to offsets from 0x02034664.
4. Layer frame logic 0x02031494 + 0x0202b288 (spot follows ribbon) — lift, provide
   layer fields.
5. Rendering: find the draw in slots 5/7/8; vertex layout 0x20 B; pass placement
   relative to composition/glare.
6. Compare with reference-video.png / player clip.

Ghidra helpers: `tools/ghidra/DecompileAt.java <out> <addr>...` and
`DecompileTree.java <out> <depth> <root>...` (also writes function-bounds.txt). Run
`analyzeHeadless project Moyou -process -noanalysis -readOnly -scriptPath native/tools/ghidra
-postScript …` with JAVA_TOOL_OPTIONS=-Duser.home=build/work/decompiler/user.
Closure decompilation: `build/work/particles-ppu/`.
