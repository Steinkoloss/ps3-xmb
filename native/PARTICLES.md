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

Layer: the original ParticlesLayer constructor (0x020322b8, camera handle) builds the
layer, and each frame follows 0x02031f44: live block copy, SPE deltas × clamped layer
factors (+0x140, +0x160; 0 at idle) with the original clamps, grid, then the lifted frame
logic 0x02031494 (writes the simulation inputs sys+0x800/0x840 matrices, 0x900-0x9b0,
LifeBounds, spot; calls the emitter) and the PPU update. Before this the update ran on
constructor defaults. The layer's animated colour (+0xd0/+0xf0) and intensity
(+0x120/+0x128) start at 0 and are faded in by the XMB boot animations
(anim_coldboot_*), not traced; they start at 1 here (the system's neutral values).
Particle passes render into rgba8 like the composition.

Open: glint pass (draw mask bit 1, firmware default on) still draws large bright halos;
the console shows only small ones. Established so far:
- Every fragment/vertex constant it reads is a literal or a set uniform; glare shaders
  loaded by the renderer (0x0207298c) are the non-MRT ones, so its colour0 is visible.
- Halo size = base size (focus curve) × |N·H|^specular power × _Glare.y (5.44);
  ~0.25 world units when aligned. Brightness then blooms through the glare chain.
- With random orientations there are fewer halos but each is still large and bright,
  so the per-glint size/brightness is the problem, not only orientation correlation.
- About a third of particles spin about the initial quaternion's axis (0,0.707,0.707).
Suspects: qgl random stand-in (0x2e67e2d8; the real one lives in a qgl firmware module),
the boot-animation layer state, and fragment intensity terms (LightPack col3).
