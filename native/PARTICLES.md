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
far 1000, eye (0,0,2), target (0,0,-7), up (0,1,0) (constants at 0x02093730). The lines layer
also renders under the camera (methods 0x02027d64/0x02027dcc apply it around update
0x02056f1c and render 0x02057cf8, whose sheet MVP comes from the qgl stack), so the
ribbon is P·V·M. Measured over the 22 s capture at 960x540, the ribbon's vertical span
has median 67 px (console), 65 px (with view), 78 px (without). Particles use MVP P·V,
Modelview V; the emission grid inv(P·V) maps the clip-space mesh to world M·node.
Particle camera distances: 7.3-14.4, 82% in focus, 17% beyond the far focus (faded by
far darkness). qgl 0xc2c9c741 is
Matrix4::lookAt on the camera vectors.

Open: the console shows more depth variety (soft out-of-focus dots, a denser trail
cluster); ours stay close to the ribbon's depth with little near/far bokeh. With the view, particles fall in the _Focus band and the quads pass
matches the console capture.

Layer: the original ParticlesLayer constructor (0x020322b8, camera handle) builds the
layer, and each frame follows 0x02031f44: live block copy, SPE deltas × clamped layer
factors (+0x140, +0x160; 0 at idle) with the original clamps, grid, then the lifted frame
logic 0x02031494 (writes the simulation inputs sys+0x800/0x840 matrices, 0x900-0x9b0,
LifeBounds, spot; calls the emitter) and the PPU update. Before this the update ran on
constructor defaults. The layer's animated colour (+0xd0/+0xf0) and intensity
(+0x120/+0x128) start at 0 and are faded in by the XMB boot animations
(anim_coldboot_*), not traced; they start at 1 here (the system's neutral values).
Particle passes render into rgba8 like the composition.

Glint halos (fixed): the particle layer is drawn after the lines layer's glare and tone
stage, onto the display output. Evidence: the console capture shows sharp sparkles and
small glints; the layer has its own glare program (particles_second), redundant if its
output went through the scene glare chain; drawn before glare, every glint core bloomed
into a large disc. Render state from 0x02060660/0x0205c254 (names via qglbase capability
map 0x21114 and libgcm NID hashes): cellGcmSetAlphaTestEnable + GREATER 0,
cellGcmSetBlendEnable ONE/ONE FUNC_ADD, depth test/cull/stencil off, depth mask off. The
alpha test is implemented (no visible effect: drawn glint fragments have alpha > 0).
The exact qgl layer order was not traced (qgl scene graph, not in the renderer image).

Layer drivers found in the renderer: 0x0202b6a8 (factor +0x140; boot states 11/12 set
0 or 1 over 2 s; idle 0), 0x0202b6dc (factor +0x160 by output height: ≥1080 → 0,
≥720 → 0.5, else 1; called from resize 0x0201acb4), 0x0202b618 (intensity +0x120, from
scene method 0x0201b128 driven by the XMB), 0x0202b658 (colour +0xd0, from 0x0201afdc;
boot passes (c,c,c,0)). Not yet wired: the resolution rule for +0x160.

qgl_base (dev_flash/vsh/module/qglbase.sprx, decrypted with sce.application_elf;
module info at file offset 0x67f2c, TOC 0x76820) answers several former guesses:
- random 0x2e67e2d8 (0x5f578): shared bss counter, Perlin-style hash, [-1, 1]. Now exact
  (start offset unknowable: other VSH users advance it). Glints unchanged with it.
- tick 0xd15d6a2c (0x4dfa0): per-frame counter.
- matrix uniform set 0xab37ac81/0x91d4236f (0x1a424/0x1a1d8): store as given; uploads
  0x193e8 (vertex) / 0x19348 (fragment) transpose with vmrghw/vmrglw — rows, as used.
- also exported: inverse 0x5f7fc, matrix→quaternion 0x61690, lookAt 0x60e20 (not yet
  lifted; native equivalents in RopeSolver).
Remaining suspect: the layer's animated state normally driven by the XMB (colour,
intensity, SPE factors; anim_coldboot_* in the renderer) and whatever else the boot
sequence sets.
