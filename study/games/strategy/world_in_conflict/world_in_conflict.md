# World in Conflict and MassTech — the renderer, read from the shipped build

How **World in Conflict** (Massive Entertainment / Sierra, 2007) draws a
kilometre of ground, a sky, and a battle on top of both: the surface system that
holds every shader, the shadow pipeline, the cloud layer and the light shafts it
drives, the terrain, and the quality database that decided what any given 2007
card was allowed to run.

> **On sources.** Massive published nothing technical about this engine. There is
> no GDC talk, no book chapter, no post-mortem; the developer interviews are
> about the Cold War and the 2020 retrospective is about design. What there *is*
> is the build, and the build is unusually forthcoming: **WiC ships its shaders
> as plain text with the comments intact.** 130 `.sur` surface files carry inline
> HLSL and `ps_1_1` assembly, Massive's own comments still attached; the vertex
> shaders are HLSL string literals compiled at runtime from inside `wic.exe`; the
> effect corpus is 2,187 text files; and the per-GPU quality database ships as a
> commented `.txt`.
>
> Everything tagged **[BUILD]** was read from the retail install on this machine
> — `E:\World in Conflict`, v1.0.1.0, **6.2 GB across 26 `.sdf` archives, 35,287
> distinct files** — using the reader in [`wic_sdf.py`](wic_sdf.py), written for
> this note against aluigi's published QuickBMS script for the container (§7).
> Comments in quoted shader code are Massive's, transcribed, not ours.

Tags: **[BUILD]** read from the retail install. **[EXE]** recovered from
`wic.exe`'s string region. **[PRESS]** a published 2007-era claim.
**[inferred]** our reading.

**The companion note is the reason to be here.**
[`world_in_conflict_particles.md`](world_in_conflict_particles.md) covers the
smoke — six-point lighting, cluster self-shadowing, the two soft-particle paths —
which is the part of this engine still worth copying. This note is the frame
around it.

Listings beside these notes:
[`particle_sixpointlight.sur`](particle_sixpointlight.sur),
[`sixpoint_particle_vs.hlsl`](sixpoint_particle_vs.hlsl),
[`postfx_softshadows.sur`](postfx_softshadows.sur),
[`wic_light_shafts.sur`](wic_light_shafts.sur),
[`tank_smoke.pe`](tank_smoke.pe), [`wic_sdf.py`](wic_sdf.py).

Related: [`ruse.md`](../ruse.md) — the same genre three years later, also read
from its shipped data; [`terrain_rendering.md`](../../../topics/world/terrain_rendering.md),
[`lod_systems.md`](../../../topics/world/lod_systems.md),
[`map_scale.md`](../../../topics/scale/map_scale.md).

---

## 1. The surface system — every shader is data, and tiered by hardware

**[BUILD]** A `.sur` file is Massive's material. It is text, it declares its own
texture slots, and it contains the pixel shader source inline. The skeleton, from
`surfaces/particle_sixpointlight.sur`:

```
Version 2

DefaultTextures { texture surfaces/dummy   texture ex_heightmap norepeat  ... }
SurfaceSettings { sort }

DefineText CommonSettings { alphatest = greater 1   blend = alphaBlend  ... }
DefineText HLSLCode       { ...the whole pixel shader... }

Config Normal PS2.0
{
    NormalPass            { $insertText CommonSettings   Code { HLSL (ps_2_0, main)  $insertText HLSLCode } }
    TexDetailPass         { ... #define DETAILTEX ... }
    ZFeatherPass          { ... #define ZFEATHER  ... }
    ZFeatherTexDetailPass { ... both ... }
}
Config Normal PS1.1 { NormalPass { Code { ps_1_1  ...hand-written assembly... } } }
```

Four things about that shape are worth taking:

**Two preprocessors, deliberately separated.** `@ifdef` is resolved by the
surface parser at load, against the *render path* (`DX9_RENDER`, `DX10_RENDER`);
`#ifdef` is left for the HLSL compiler, against *feature variants*
(`DETAILTEX`, `ZFEATHER`). One sigil for "which API am I on", another for "which
version of this shader". The distinction keeps a single file readable while
serving two backends and four variants — and the file says so in a comment where
the two interact:

```
@ifdef DX9_RENDER
    // We don't remap for DX10 because we would need to redefine the
    // texture binding in the C++ or something.
    textureRemap = (0,1,2)
@endif
```

**`DefineText` / `$insertText` is a macro system, not includes.** The shared body
is written once in the file that uses it and pasted into each pass. **[inferred]**
Crude, but it means a surface file is self-contained — you can read one and know
everything it does, which is exactly what makes this corpus legible fifteen years
later, and exactly what a tree of `#include`s would have destroyed.

**Variants are passes, not branches.** `ZFeatherPass` exists so the non-feathered
case compiles without the feather. On `ps_2_0` a dynamic branch was not an option;
the structure survives as a reminder that **the renderer selects a pass by name**
rather than setting a uniform.

**Every surface carries its own fallback.** The `Config` line names a tier and a
shader model — `Normal PS2.0`, `Normal PS1.1`, `Highend PS2.0`, `Lowend PS1.1`,
`SpeedTreeHigh PS2.0`, `Normal PS2.0 HighEndWater`, `Normal PS2.0 8TMU`. The
`ps_1_1` blocks are hand-written assembly, and they are not stubs: the six-point
particle fallback implements the full technique in nine instructions
([`particles`](world_in_conflict_particles.md) §2.5). Where a fallback genuinely
could not be written, it is honest about it — `postfx_softshadows.sur`'s
`Config Lowend PS1.1` is seven passes that each `mov r0, c0` and do nothing.

**[BUILD]** 108 distinct surface files were recovered (130 archive entries; the
difference is patch re-issues). 38 of them live in `surfaces/game/` and are the
engine's own passes; the rest are asset-facing materials.

---

## 2. The frame

**[EXE]** The pass names in the executable, in the order the string table holds
them, give the shape of a frame:

| Group | Passes |
|---|---|
| Depth / lighting | `DepthOnlyPass`, `AmbiencePass`, `AmbienceAndFirstLightPass`, `SunLightPass`, `SoftShadowsSunLightPass`, `HardAndNoShadowsSunLightPass`, `FILLLIGHTINAMBPASS` |
| Terrain | `LowendPass`, `HighendPass`, `GouraudPass`, `WreckPass`, `TexturedExplosionWreckPass`, `DeploymentZonePass`, `WireframePass`, `EditorLightingPass` |
| Sky | `CloudPackPass`, `CalcStartShaftsPass`, `CalcShaftsPass`, `UnpackShaftsPass`, `ShadowPass` |
| Shadow composite | `DownsamplePass`, `DownsamplePass2`, `3x3KernelPass`, `SoftShadowToScreenPass`, `SoftShadowToScreenAndCloudShadowPass`, `SoftShadowToScreenAndCloudShadowPassToBlur`, `LightShaftPass` |
| Particles | `NormalPass`, `TexDetailPass`, `ZFeatherPass`, `ZFeatherTexDetailPass` |
| Post | `9x9KernelPass`, `9x9KernelBleedLightPass`, `9x9KernelBleedDarkPass`, `3x3BlurPass`, `BlurDesaturatePass`, `DesaturatePass`, `InterlacePass`, `DarkenPass` |
| Debug | `ViewPenumbraPass`, `ViewBlurredShadowPass`, `DebugOverdrawPass1/2` |

This is a **forward renderer with a sun and a screen-space shadow composite** —
not deferred. The lighting split is by shadow state, not by light: a mesh is
drawn by `SoftShadowsSunLightPass` or `HardAndNoShadowsSunLightPass` depending on
what the shadow system can do for it. **[inferred]** For an RTS with one dominant
directional light, ~40 units on screen and heavy alpha, that is the right call:
deferred buys you many lights, and this game has one.

Note also `ViewPenumbraPass`, `ViewBlurredShadowPass` and two overdraw
visualisers shipping in the retail executable. The debug views for the shadow
pipeline were kept in the build.

---

## 3. Shadows — stencil volumes, an accumulated penumbra, and a min-filter

WiC's shadows are **stencil shadow volumes**, in 2007, at a time when most of the
industry had moved to shadow maps. The whole design is visible in four files.

### 3.1 A separate shadow mesh per model

**[BUILD]** Alongside every render mesh (`.mrb`) sits a `.sdw` — magic `SDW`,
its own file, with its own LOD chain:

| | Files | Bytes | Mean |
|---|---:|---:|---:|
| `.mrb` render meshes | 3,367 | 1,179,112,791 | 350 KB |
| `.sdw` shadow meshes | 1,738 | 25,838,466 | 14.9 KB |

For `props/europe_props/europe_lighthouse`: 245,200 bytes of render mesh,
**17,727 bytes of shadow mesh** (7.2%), plus a `europe_lighthouse_lod2.sdw` at
1,920 bytes. Across the corpus the shadow geometry is **2.2% of the mesh bytes**.

**[inferred]** This is what makes stencil volumes viable at all. Volume extrusion
cost scales with silhouette edge count, so the shadow caster has to be a
drastically simplified, closed, purpose-built hull — and it has to be an *asset*,
authored and LOD'd, not something derived at load. The 2.2% figure is the budget
that decision implies.

### 3.2 Umbra and penumbra are two different accumulations

**[BUILD]** Three tiny surfaces do the stencil work, and their blend modes are
the entire design:

| Surface | Blend | What it does |
|---|---|---|
| `stencil_softshad1.sur` | `blend one zero`, `colorWrite = (0,0,0,1)` | writes the **umbra** into destination alpha |
| `stencil_softshad2.sur` | `blend = one one` | **accumulates** into alpha — additive |
| `stencil_poly.sur` | `blend = zero srcColor` | modulates |

The second is the interesting one. An additive accumulation into alpha means the
penumbra is built by **drawing the shadow volume repeatedly, slightly displaced,
and summing** — each pass contributing a fraction, so the edge comes out as a
gradient instead of a step. `stencil_softshad2.sur` also ships
`DebugOverdrawPass1/2`, which counts layers and maps them to a red/green/white
ramp: **[inferred]** they were watching exactly the cost this technique
generates, because the number of accumulation passes is bounded by fill rate.

The convention the whole pipeline then runs on is stated in a comment in
[`postfx_softshadows.sur`](postfx_softshadows.sur):

```
// Shadow (from ex_3dview.a)
// Input: 0.0=noShadow, 0.5=penumbra, 1.0=umbra
```

**Shadowing lives in the alpha channel of the scene render target**, with three
meaningful values. That is why `wicwater.sur` carries the comment `IMPORTANT:
Write to dst alpha to mask out shadows in water` — anything that wants to opt out
of shadowing writes its own alpha.

### 3.3 The composite, and a blur that is a minimum

The screen-space stage downsamples the shadow alpha with a rotated 4-tap, then
runs `3x3KernelPass`, which is **not** an average:

```hlsl
half minRes = min(upperLeft, lowerLeft);
minRes = min(minRes, upperRight);
minRes = min(minRes, lowerRight);
minRes = min(minRes, t4.a);
```

A **minimum filter**. **[inferred]** Averaging a hard-edged stencil result
produces grey fringes on both sides of the edge — shadow leaking onto lit ground.
Taking the minimum spreads the *lit* value inward instead, so the softening only
ever eats into the shadow and never haloes outside it. Combined with the
accumulated penumbra from §3.2, the softness is authored rather than accidental.

Then `SoftShadowToScreenPass` recombines, with a distance fade that has two
implementations and a comment recording why:

```hlsl
//shadowStrength = shadowStrength * aVSOut.myColor.a;   // Fade out shadows by distance approximation (higher screenspace = larger distance)
shadowStrength *= saturate((400-depth) * 0.005);        // Non aproximated version when we have depthtexture anyway
```

The DX9 path fades shadows by **screen Y** — in a fixed-pitch RTS camera, further
up the screen means further away, and that is close enough. The DX10 path, having
a depth texture, does it properly. **Unit shadows end at 400 units.**

### 3.4 Cloud shadows arrive in the same composite

`SoftShadowToScreenAndCloudShadowPass` reconstructs world position from depth and
projects it into a sun-aligned cloud shadow texture:

```hlsl
float3 worldPos = setConstant3 + aVSOut.myUV2.xyz * depth;      // camera + ray * depth
float3 relWorldPos = worldPos - setConstant6.xyz;               // - shadow texture centre
float2 shadowCoords = float2(dot(setConstant4, relWorldPos) * 0.00025 + 0.5,
                             dot(setConstant5, relWorldPos) * 0.00025 + 0.5);
half4 tex3 = tex2D(sampler3, shadowCoords);

float cloudShadowStrength = saturate(tex3.r + tex3.g * saturate((tex3.a + 0.05 - worldPos.y * (1.0/255.0)) * 20));
cloudShadowStrength *= setConstant6.a * saturate((2000-depth) * 0.001);
```

Two sun-orthogonal axes at scale `0.00025` — a **4,000-unit footprint**. The
shadow texture is not a depth map: `.r` is unconditional shadow, `.g` is shadow
gated by `.a`, which holds a **height** compared against the pixel's world Y. A
shadow map with a height threshold instead of a depth comparison. **[inferred]**
It costs one texture and no shadow-camera pass, and it is enough because the
occluders are a flat cloud layer at a known altitude — the height term exists so
that a mountain top or a tall building can poke *above* the shadow.

Cloud shadows fade at **2,000 units**, five times the unit-shadow range, and are
squared twice on the way (`*= 2; cloudShadowStrength *= cloudShadowStrength;`) to
harden the contrast. Both terms combine as `1-(1-a)(1-b)` — an over, not a min or
a multiply.

---

## 4. Clouds, and light shafts computed in cloud space

**[BUILD]** The cloud layer (`surfaces/game/wicclouds.sur`) is a shape mask
texture, a scrolling noise texture and a set of per-map constants. It is a
**flat layer with a backlight model**, not a volume:

```hlsl
float backLightFactor = pow(saturate(dot(normalize(aVSOut.pointToCamera), sunDirection)), backLightExponent);
float invertedAlphaFromMask = saturate(BACKLIGHT_ALPHA_INVERT_BASE - alphaFromMask * BACKLIGHT_ALPHA_INVERT_SCALE);
float backlight = backLightColorStrength * invertedAlphaFromMask * invertedAlphaFromMask * backLightFactor;
```

Silver lining, done as a formula: brightness rises with how close you are to
looking into the sun, is strongest where the cloud is *thin*
(`invertedAlphaFromMask`, squared), and falls off towards the top of the cloud
(`BACKLIGHT_BASE - myShapeUV.y * BACKLIGHT_TOPLIGHT_SCALE`). Every coefficient is
a named constant with its default in a comment. `alphaFromMask` is
`dot(myShapeFactors, shapeMaskTexel)` — four masks in RGBA, weighted per vertex,
so cloud shape is a blend of four authored patterns.

Per-map cloud surfaces ship alongside the maps
(`maps/europe5/clouds/wicclouds.sur`, and one named
`wicclouds_20900_14051_18483_9521_31935_27384_5747.sur` — **[inferred]** a
parameter hash from the map editor).

### 4.1 The light shafts

[`wic_light_shafts.sur`](wic_light_shafts.sur) is the part with no obvious
equivalent elsewhere. God rays are **not** computed in screen space from the sun
sprite. They are computed **in the cloud shadow texture**, radially, before the
scene is drawn:

1. **`CloudPackPass`** folds the cloud mask into four quadrants and packs them
   into RGBA — four rays' worth of work per texel from then on.
2. **`CalcStartShaftsPass`** accumulates 32 taps from the texel towards the
   centre (the sun): `uv = myUV0 * a / steps`.
3. **`CalcShaftsPass`** continues the ray from where the previous stage stopped,
   32 more taps, and blends with the earlier result — a second refinement pass
   over the same radial integral.
4. **`UnpackShaftsPass`** unfolds the four quadrants back out, summing two
   textures.

**[inferred]** Two consequences follow, and both are visible in the game.
Because the integral is over the *cloud layer* rather than the screen, **shafts
exist across the whole map at once** — they fall on ground the camera is not
pointed at, they are consistent between frames as the camera pans, and they
survive the sun being off-screen entirely, which is the failure case of every
screen-space radial-blur implementation. And because the work happens in a small
packed texture, its cost is independent of screen resolution.

The composite is the detail that makes them read as air rather than as haze.
`LightShaftPass` in [`postfx_softshadows.sur`](postfx_softshadows.sur) blends
`one invSrcAlpha` and returns:

```hlsl
float add = (tex3.r - aVSOut.myUV1.y) * (1-fog) * setConstant6.w * aVSOut.myUV1.x;
float sub = (-(tex3.r - aVSOut.myUV1.y)) * (1-fog) * setConstant6.w * aVSOut.myUV1.x;
return float4(add, add, add, sub);
```

A **signed** shaft, around a reference level (`myUV1.y`): brighter than average
adds light, darker than average subtracts it. Purely additive god rays wash a
scene out; this darkens the shadowed air as much as it brightens the lit air,
which is why the effect survives being strong.

---

## 5. Terrain, and terrain that remembers being shot

**[BUILD]** `surfaces/game/map_renderer.sur` is the largest surface in the game
(24 KB) and its texture slot comments are Massive's own documentation:

```
texture surfaces/textures/gray    // [0] LOWEND: RGB   detailmap 1
texture surfaces/textures/gray    // [1] LOWEND: RGB   detailmap 2
texture surfaces/dummy noRepeat   // [2] LOWEND: DXT1a mega texture tile
texture surfaces/dummy            // [3] deployment zone texture tile
texture ex3dmap_dzmask noRepeat   // [4] deployment zone mask
texture surfaces/dummy            // [5] HIGHEND: RGB  mtrlweights0 (d1r, d1g, d1b)
texture surfaces/dummy            // [6] HIGHEND: RGBA mtrlweights1 (d2r, d2g, rmd, spec)
texture surfaces/dummy            // [7] HIGHEND: RGBA mtrlweights2 (d3r, d3g, d3b, d3a)
texture surfaces/dummy            // [8] HIGHEND: RGBA detailmap 3
texture surfaces/textures/black   // [9]  HIGHEND: Landscape wreck detail texture (DXT5), .rgb = detail/color, .a = noise to break tiling
texture surfaces/textures/black   // [10] HIGHEND: Landscape wreck from objects texture (DXT5)
texture surfaces/textures/black noRepeat // [11] HIGHEND: Landscape wreck from small explosion texture (DXT5)
texture wi3d_mapwreckdirections noRepeat // [12] Wreck directions buffer (.xy = direction, .y = wrecktexture factor, .w = attenuation)
texture ex_groundshadow           // [13] Shadows from terrain baked into a DXT5 textures
```

The structure is **a low-frequency unique "mega texture" tile plus high-frequency
detail maps splatted by weight** — 11 material weights across three textures at
high end, collapsing to one baked tile and two detail maps at low end. Vertex
colour carries baked lighting (`.rgb = vertex lightmaping, .a = vertex y-offset`),
and terrain self-shadowing is **baked into a DXT5 texture** rather than computed.

Three of the fourteen slots are **destruction**, live: wreck from explosions,
wreck from destroyed objects, wreck from small explosions, plus a *directions*
buffer whose `.xy` is a direction and `.w` an attenuation. **[EXE]** The debug
commands confirm this is a running system, not decals —
`Ground.ObjectWreckTextureExpands`, `Ground.ObjectWreckTextureBlurs`,
`Ground.ExplosionTextureBigRadius`,
`Ground.ExplosionTextureRadiusTooSmallForLod1Mesh`, `Ground.RenderWreckPass`,
`Ground.DebugDarkMapUpdateEveryFrame`. The terrain accumulates damage into
textures that are expanded and blurred over time, with a direction field so a
blast scar is oriented rather than radial.

**[BUILD]** 39 maps ship. Grass, SpeedTree vegetation and water each have their
own surface with their own `Config` tiers; water alone declares **16 custom
constants** including four independently-rotating normal maps, a depth map,
reflection, foam and a cloud-shadow density term.

---

## 6. The quality database — 821 device IDs, decided at Massive

**[BUILD]** `wic_default_gfx.txt` ships in the archive as a commented text file
mapping PCI device IDs to quality presets. It covers **7 vendors** (NVIDIA, ATI,
Matrox, XGI, SiS, S3, Intel) across 43 `Device:` lines resolving to roughly
**821 device IDs**, and its header records its own sources and last update:

```
// nVidia list is based on:  http://developer.nvidia.com/object/device_ids.html
// last update:              http://developer.nvidia.com/attach/10054 (178.13 release - 1 October 2008)
// ATI list based on:        http://developer.amd.com/gpu_assets/ATI_Device_IDs_Jan_09.txt
```

The tier distribution is the interesting part: **Low 21, Medium 13, High 8, Very
High 6**. Entries can also pin a default resolution (`Resolution4_3: 1600x1200`),
cap texture quality independently, or cap the shader model outright
(`UsePixelShaderVersion`).

**[EXE]** Underneath sit about forty individually addressable options —
`myShadowQuality`, `myWaterQuality`, `myTerrainTextureQuality`, `myZFeather`,
`myParticlePhysics`, `myPostFXSoftShadows`, `myPostFXBloom`, `myPostFXHeatHaze`,
`myCloudShadows`, `myGrass`, `myCraters`, `myUnitTracks`, `myDecals`,
`myAutoprops`, `myWreckFxFlag`, `myHighQualityTerrain`, `mySpeedTreeShadows`,
`mySpeedtreeHQShaders`, `myGlobalParticleEmitRate`, `myWaterReflectClouds` /
`Units` / `Props` / `Effects`, `myVeryLowQuality` — plus a `MiscFXMemoryStats`
line that reports the cost of the big consumers directly:

```
MiscFXMemoryStats: SoftShadows(%.1fmb) Bloom(%.1fmb) Water(%.1fmb) Clouds(%.2fmb) Decals(Tex(%.1fmb)Mesh(%.2fmb))
```

**[inferred]** Two readings. First, the shipped tier list is 34 of 48 entries at
Medium or Low — the presets encode Massive's own judgement that most of the
installed base could not run the game as designed, made per device, updated
through 2009. Second, this only works because **every feature is separable**: the
`.sur` `Config` tiers, the per-effect `DISABLEONLOWEND` flag, the pass-name
selection in §1. A renderer with an all-or-nothing pipeline has nothing to put
in such a file.

---

## 7. Reading the install yourself

**[BUILD]** The container is `RYS`, shared with Massive's earlier *Ground
Control* titles. **[Prior art]** aluigi's QuickBMS script `world_in_conflict.bms`
documents both generations; [`wic_sdf.py`](wic_sdf.py) re-implements it in Python
so the archives can be listed and selectively extracted rather than unpacked
whole (6.2 GB). WiC's archives are all the later generation: a single
LZMA-compressed table of contents at a header offset, then per-file payloads.

Two traps, both of which produced wrong data before they were found, and both
worth stating because they are silent:

**Payloads are a *sequence* of independent zlib streams, not one.** A single
`zlib.decompress()` raises on the trailing bytes; the obvious fallback — a
raw-deflate decompressor — does not raise, it returns **garbage of plausible
length**. A 10 MB texture silently became a 6.8 MB one and looked fine. The
reader now consumes stream after stream and refuses to return a short buffer.

**Every `.dds` decodes exactly 128 bytes short of its declared size** — the size
of a D3D9 DDS header. The packer stores the texel payload headerless and the
table of contents still records the authored file size. This is unresolved: the
header is not adjacent to the payload, and nothing in the entry carries it. The
reader special-cases a shortfall of exactly 128 and rejects any other. The
dimensions can be recovered from the effect or model that references the texture
— which is how the six-point atlases in
[`particles`](world_in_conflict_particles.md) §2.4 were verified.

With those handled, **3,345 of 3,345** extracted non-texture files match their
declared sizes exactly, and **25 of 25** textures come out exactly 128 bytes
short — the shortfall is systematic, not sporadic, which is what makes it safe
to special-case.

The formats worth knowing, by count across the archives:

| Ext | Files | What it is |
|---|---:|---|
| `.mp3` / `.wav` | 12,014 / 908 | audio; `sound/feedback` alone is 9,668 files |
| `.dds` | 10,029 | textures, headerless (above) |
| `.mrb` | 3,367 | render meshes, magic `MRB` |
| `.ice` | 2,228 | object/entity definitions, magic `ice0010` |
| `.pe` | 2,187 | particle effects, **plain text** |
| `.sdw` | 1,738 | shadow meshes, magic `SDW` |
| `.tga` | 1,457 | uncompressed textures (the six-point atlases live here) |
| `.pyo` | 467 | compiled Python 2.x — the game's scripting layer |
| `.gety` | 223 | LightWave `FORM…LWO2` objects; wreck geometry, `TAGS` chunk `Wreck` |
| `.slot` | 242 | text attachment points (`AddNullObject Door`) |
| `.sur` | 130 | surfaces — **plain text with inline HLSL** |

**[inferred]** The `.gety` files being raw LightWave objects, and 467 files of
Python bytecode, say something about the pipeline: destruction geometry came
straight out of the DCC without an intermediate format, and gameplay was scripted
rather than compiled. Neither is visible from outside the build.

---

## 8. What carries over

**The surface file is the model to copy, not the shader.** A self-contained text
material that declares its texture slots, carries its own HLSL, names its passes,
and ships one block per hardware tier with an honest do-nothing fallback where a
tier cannot be served. Two preprocessors, kept separate: one for backend, one for
feature variant. Fifteen years later the corpus is still readable without tools,
which is not true of any compiled shader cache. §1.

**Compute atmospherics in the space the occluder lives in.** Light shafts
integrated across the cloud shadow texture rather than the screen are cheaper,
resolution-independent, temporally stable, and correct when the sun is off
screen. The general lesson — *the radial integral belongs in the occluder's
space, not the viewer's* — outlives the specific technique. §4.1.

**Signed, not additive.** The shaft composite adds light where the cloud is thin
and subtracts it where the cloud is thick. Almost every cheap atmospheric effect
is written as additive-only and then has to be turned down until it stops washing
the image out. §4.1.

**Min, not mean, when softening a hard mask.** Blurring a binary shadow buffer
with an average haloes it outward; taking the minimum only eats inward. §3.3.

**A shadow map is not the only way to project a shadow.** The cloud layer casts
via a world-projected texture with a *height* channel compared against
reconstructed world Y — no shadow camera, no depth compare, one texture. Worth
remembering for any large flat occluder. §3.4.

**Separability is what makes a quality database possible.** 821 device IDs across
four tiers only works because every feature can be removed independently, from
`Config` blocks in surfaces down to a `DISABLEONLOWEND` flag on individual
particle effects. §6.

**And what not to carry:** stencil shadow volumes, which cost a second authored
mesh per model, a fill-rate-bound accumulation per softness step, and a debug
overdraw visualiser to keep it honest — for shadows that fade out at 400 units.
Shadow maps won that argument for good reasons. The `.sdw` corpus is the receipt:
1,738 assets that exist only because of the technique. §3.
