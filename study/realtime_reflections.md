# Real-time reflections — completing the system

A handover for a fresh chat. The goal is the full stack: a **prefiltered probe
array** plus **screen-space reflections**, built the way shipping engines build
them.

The brief was explicit and it is the reason this document exists: *do not bake
in something modern engines do not do, in order to go faster or skip a step*.
Several shortcuts are already in the tree and each one is named below with what
it costs and what replaces it. Removing them is part of the work, not a later
tidy-up.

State verified against the tree on 2026-08-11. Tags follow the `study/`
convention: **[VERIFIED]** read from this codebase, **[PAPER]/[VENDOR]** from a
cited source, **[inferred]** reasoned rather than sourced.

---

## 1. Where things stand

| | |
|---|---|
| GL target | **4.3 core**, forced for cubemap arrays. GLSL 330 shaders compile unchanged **[VERIFIED** CMakeLists.txt:416-425**]** |
| Compute | **available and wrapped** — `cromwell/gpu/compute/ComputeShader.cpp`, `ComputeSelfTest.cpp` **[VERIFIED]** |
| Shaders | `src/cromwell/assets/shaders/`, with a working `#include` preprocessor and a `common/` split |
| Probes | `cromwell/lighting/ReflectionProbeSet` — one per room, cubemap **array**, two volumes each (influence + parallax), per-pixel selection, raw GL through `render/gpu/GL.hpp` |
| G-buffer | `cromwell/gpu/target/GBuffer.hpp` — depth texture, world normal in RGB, **roughness in alpha** |
| Custom depth/stencil | `cromwell/gpu/target/CustomDepthStencil` — per-object 0-255 id + own depth |
| Inspector | `cromwell/overlay/TexturePreviews` — remaps buffers so ids and depth are legible |
| Scheduling | `cromwell/gpu/target/CaptureSchedule.hpp` |
| **SSR** | **does not exist.** No `ssr.glsl`, no scene-colour target |

---

## 2. The four shortcuts currently in the tree

These are the "baked in" items the brief warns about. Do not build on top of
them; replace them.

### 2.1 The probe array has no mip chain

`ReflectionProbeSet.cpp:90-94` allocates `glTexImage3D(..., level 0, ...)` and
sets `GL_TEXTURE_MIN_FILTER = GL_LINEAR`. **[VERIFIED]**

Because there is no prefiltered chain, `common/environment.glsl` fades the
probe out to the analytic two-lobe sky as roughness rises, reaching plain sky
by about 0.55. That fade was written as an honest stand-in and has since become
load-bearing: `ReflectionProbeSet.hpp:80-85` justifies the 128px face size with
"reflections that blend to the analytic sky by roughness 0.55 anyway".
**[VERIFIED]**

**Why it must go.** No engine ships this. A rough surface is supposed to
reflect a *blurred version of its surroundings*; ours reflects a flat gradient.
It also blocks the whole glossy path — FidelityFX's classification step routes
rough pixels to the prefiltered cubemap *instead of* tracing, which requires
the cubemap to actually be prefiltered. **[VENDOR]**

**MEASURED AFTER BUILDING IT, 2026-08-14:** the prefiltered chain landed on the
rhi path and is **visually indistinguishable** on this board — old fade against
new chain, **max 3/255 across the whole frame, not one pixel over 8**.
**[VERIFIED]**

That is not a failure of the prefilter and it was predictable from the two
sections either side of this one. Ambient specular is
`environmentSpecular(...) * environmentBRDF(f0, roughness, nDotV)`, every surface
on this board is a dielectric at f0 = 0.04 (§2.2), and that factor is small — and
smaller still at the roughness where a prefiltered chain differs most from a
flat gradient. The chain changes what the term CONTAINS; it cannot change how
little the term is worth on an all-dielectric board.

Where it pays off, in order:

1. **Metals**, whose f0 is their albedo rather than 0.04. Blocked on §2.2.
2. **SSR classification** (§5.6) — rough pixels skip tracing entirely and take
   the prefiltered probe, which requires a chain that is actually prefiltered.
   This is the real unlock and the reason it is stage one.
3. Varied authored roughness, once materials carry textures (§4.7 of MIGRATION.md).

**The lesson worth keeping:** a correct term that is multiplied by something
small is invisible, and "I built the right thing" is not the same claim as "you
will see a difference". Measure the delta before promising one.

### 2.2 The G-buffer has no F0

Roughness lives in the normal target's alpha. There is no albedo and no
metalness, so any screen-space pass must assume every surface is a dielectric
at 0.04. **[VERIFIED]**

**Why it must go.** Metals reflect their own colour — copper, brass, gold,
bronze, anodised aluminium. Under a monochrome dielectric assumption they all
reflect white, which is not "slightly wrong", it is the metal workflow not
working. Today the only metal in this game is galvanised steel, which is
achromatic, so the defect is invisible *right now* and would surface in whatever
project picks the engine up next — the exact failure mode the brief is trying to
avoid. **The engine must carry full RGB F0**; see §4.

### 2.3 There is no scene colour buffer

SSR has nothing to sample and water has nothing to refract. **[VERIFIED]**

### 2.4 The outdoor probe's parallax box is the whole board

Added 2026-08-14, after a reported bug: geometry behind the camera appeared on
the **wrong side** of an exterior window on the rhi path while the raylib path
was correct. **[VERIFIED** by moving the outdoor capture point onto the reported
viewpoint, which fixed it**]**

`ProbePlacement.cpp` gives every interior room a room-sized parallax box and
gives the outdoor volume the **entire world**, with one capture point in the
middle of the map. The reasoning was that the outdoors really is that big. Size
was never the question.

**What every engine actually does.** A cubemap is by definition an environment
at infinity, and parallax correction exists to fix exactly that. The correction
is only as good as the box: "the common point of every parallax-correction
technique is to define an approximation of the geometry surrounding the local
cubemap, and the simpler the approximation, the more efficient the algorithm
will be **at the price of accuracy**" **[PAPER** Lagarde 2012**]**. Unity's Box
Projection says to "set the size to match the dimensions of the room"
**[VENDOR]**; Source's parallax-corrected cubemaps take a bounding-box **brush**
an artist draws round the local geometry **[VDC]**; Source 2 ships
parallax-corrected cubemaps as standard **[VDC**, `source2_rendering.md` §4**]**.

A board-sized box round a street is the degenerate case of that approximation:
the ray travels twenty tiles to the box before being re-aimed from a capture
point ten tiles from the surface, so the re-aim points into the wrong half of
the world. Turning correction OFF (an environment at infinity) is not the fix
either — that is the default state parallax correction was invented to repair,
and measurement confirmed it: moving the capture point 3.7 tiles with correction
off changed the pane by a mean of 2.44/255, because with no correction the
capture point only alters cubemap *contents*, not the lookup direction.

**What replaces it.** Exterior probes with **local boxes** — a box per street
block, sized to the geometry around it, with correction ON. Shipped as a 3x3
grid over the outdoor volume plus a board-sized fallback for the seams, 13 of
16 layers on the demo map. **[VERIFIED** confirmed correct on the reported
viewpoint**]**

**AND THE GRID IS A STAND-IN FOR AUTHORED PLACEMENT.** Source 2 does not
generate these: `env_cubemap_box` is an entity a level designer places, with a
rectangular volume they size by hand — "in a simple cuboid room, you'd want the
volume bounds to meet the walls, floor and ceiling". **[VDC]** The placement
guidance is far tighter than any grid: a cubemap serving a glass pane goes "in
the center of the brush 16 units away from the face meant to receive the
reflection", and one serving the player's eye goes 64 units above the ground.
**[VDC]** That is a probe per reflective surface, not per region.

A grid is the right answer here only because this map is generated. The day maps
are authored, placement should come from the map data and this becomes the
fallback for anything unplaced.

**Two things Source 2 has that this selection rule does not** **[VDC]**:

- **Volumes may OVERLAP**, and
- **priority is EXPLICIT** — the designer sets which volume wins.

Ours derives priority from box volume (smallest wins), which cannot express
"this one overrides that one" and forces the boxes not to overlap, because two
equal volumes tie and `selectProbes` breaks the tie by array index rather than
by which box the fragment sits deeper inside. That is why block seams currently
crossfade through the board-sized fallback instead of into the neighbouring
block. An explicit priority field plus depth-inside tie-breaking is the fix, and
it is a change to the selection rule rather than to placement.

**What SSR does and does not fix here.** §5.6 routes smooth pixels to a trace
and rough ones to the probe, so a smooth pane would normally trace. It would not
have helped this report: the reflected boxes were **behind the camera**, and
screen-space rejection falls back to the probe for exactly that case
**[VENDOR]**. Off-screen reflections are the probe's job permanently, which is
why the box matters even after stage three lands.

---

## 3. Stage one — prefilter the probe array

This is the prerequisite for everything glossy. Nothing blocks it: the file
already calls raw GL and already uses `glFramebufferTextureLayer`, which is
exactly what writing into one (layer, face, mip) needs. **[VERIFIED]**

**Allocation.** Replace the single `glTexImage3D` with `glTexStorage3D` over
the full chain. Immutable storage suits a texture whose shape never changes and
removes the per-level loop. At 128px, 5-6 levels are useful (128 down to 4);
memory goes from ~12 MB to ~16 MB for 16 probes.

**Filtering.** `GL_TEXTURE_MIN_FILTER` becomes `GL_LINEAR_MIPMAP_LINEAR`.

**Seamless cube filtering.** `glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS)`. This
matters far more than it sounds: at high mips the filter lobe crosses face
boundaries constantly, and without it every probe shows seams exactly where
reflections are blurriest. GL 3.2+, and this file already calls GL directly.

**GGX prefilter, not `glGenerateMipmap`.** Box-filtered mips are one call and
would "work". They are also the shortcut this document exists to refuse: a box
filter is not a specular lobe, so the roughness→mip mapping means nothing
physical and the result is wrong in a way that is hard to attribute later.

Build the split-sum prefilter instead — importance-sample the GGX distribution
per mip with the standard `N = V = R` simplification, each level corresponding
to `roughness = level / (levels - 1)`. **[PAPER]** Sample the *previous* mip
rather than mip 0 and weight by solid angle, which is what suppresses fireflies
from small bright sources.

Compute is available, so the prefilter can be a compute dispatch over the array
rather than 6 × levels fullscreen draws. Either is fine; the draw version is
easier to debug and the codebase already has the framebuffer plumbing.

**Carry the coverage alpha through.** Alpha is coverage — 1 where the capture
found geometry, 0 for open sky — and the lit shader relies on it to fall back
to the analytic gradient. **[VERIFIED** ReflectionProbeSet.cpp:86-89**]** A
naive blur mixes black sky into geometry and darkens every rough reflection.
Accumulate **premultiplied** and normalise:

```
sumColour += sample.rgb * sample.a;
sumAlpha  += sample.a;
sumWeight += 1.0;
out = vec4(sumColour / max(sumAlpha, eps), sumAlpha / sumWeight);
```

That yields "average radiance over covered directions" plus "fraction covered",
so the existing `mix(sky, probe.rgb, probe.a)` keeps working at every level.

**The scheduling constraint — this is the one that is easy to get wrong.**
Capture walks one **(probe, face)** pair per frame (`ReflectionProbeSet.hpp:149-169`).
**[VERIFIED]** A GGX prefilter cannot run per face, because at high roughness
the lobe reaches into neighbouring faces — prefiltering +X while −Z still holds
pre-rebuild content bakes stale data permanently into the chain. **[inferred]**

So the prefilter runs **per probe, once all six of its faces are current**.
That is a change to `CaptureSchedule`, not just a new pass: the schedule needs
to track per-probe completion, not just a global stale count.

**Remove the sky fade last.** Keep `smoothstep(0.12, 0.55)` in
`environmentSpecular` until the chain is verified in the inspector, then delete
it and sample `textureLod(uEnvironmentMap, vec4(dir, layer), roughness * (levels - 1))`.
Deleting that fade *is* the payoff.

---

## 4. Stage two — finish the G-buffer

**Where F0 comes from.** Writing it exactly means sampling albedo and MRAO in
the prepass, which turns a cheap position-and-normal pass into a material-aware
one that binds per-material textures. That is the real cost, not the memory.
The alternative is a **per-draw uniform** computed from the material's
metalness and base colour, exactly as roughness already works. For weighting a
reflection, per-material is sufficient — the same argument that justified
per-material roughness. **[inferred]** Take the uniform.

**Where it lives — decided: a second colour attachment, carrying full RGB F0.**

RT1 = `F0.rgb` + spare alpha. rlgl supports it: `rlActiveDrawBuffers(int count)`
and `RL_ATTACHMENT_COLOR_CHANNEL1` both exist. **[VERIFIED** rlgl.h:563,666**]**
Costs one more RGBA8 at window resolution (~4 MB at 1280×800) plus per-pixel
bandwidth. The spare channel is where TAA velocity or a material id goes next.

**This is not a trade-off to revisit, and the reason is that cromwell is an
engine.** The tempting alternative is octahedral normal encoding — a unit normal
fits in two channels with *better* precision than 8:8:8 xyz, which frees one for
a monochrome F0, costs nothing and improves the normals. It is also correct for
every metal currently in this game, because the only one is galvanised steel and
steel is achromatic.

That last sentence is a fact about one tactical game's placeholder art, not
about the engine. Copper, gold, brass, bronze, anodised aluminium and every
painted metal reflect their own colour, and an engine that cannot express that
does not support metals — it supports grey ones. cromwell is meant to be lifted
into RTS, FPS and third-person projects; the first of those with a gold trim or
a copper pipe would have to tear the G-buffer open, and every screen-space pass
written against the monochrome assumption in the meantime would need revisiting
with it.

Full RGB F0 is what makes the metal/rough workflow actually complete: `f0 =
mix(vec3(0.04), albedo, metalness)` is already what the forward shader computes
per material, and the G-buffer should carry the same thing rather than a
flattened approximation of it.

**A consequence worth deciding consciously, not inheriting.** With a real
prefiltered chain in place, the split-sum approximation's *second* half becomes
live: the environment BRDF term. `common/brdf.glsl` currently uses Karis'
analytic fit and says so — it "stands in for the lookup table a real IBL would
sample". **[VERIFIED]** That fit is legitimate and widely shipped, and with RGB
F0 it behaves correctly across the metal range. Keep it or bake a real BRDF LUT,
but make it a choice: it is the one remaining place where the IBL path is an
approximation rather than the real integral.

---

## 5. Stage three — screen-space reflections

### 5.1 Scene colour

An RGBA16F target at window resolution holding the lit scene, resolved before
the transparent pass. This is CS2's `g_tSceneColor` and **water refraction needs
the same buffer**, so it is built once and serves both. See
`study/source2_rendering.md` §12.3.

Ordering note: SSR marches the **current** frame's G-buffer depth (geometrically
correct hits) but samples the **previous** frame's colour. One frame of colour
lag, no reordering of the forward pass, and no G-buffer albedo required.
**[inferred]**

### 5.2 The march — DDA, not linear stepping

**Do not step in world space.** Fixed world-space steps oversample near the
camera (many steps inside one pixel) and skip pixels far away (missed hits).
McGuire and Mara adapt perspective-correct **DDA line rasterisation** to march
in screen space, which guarantees a contiguous run of pixels with no
oversampling; the paper gives full implementation details of a method "proven
in production of recent major game titles". **[PAPER]**

This was the single biggest error in the original plan for this feature and is
the main reason this document was written.

### 5.3 Resolution and reuse

Trace at **half resolution** and reuse rays from adjacent pixels in the Monte
Carlo integration, then resolve to full resolution — Frostbite pays "only a
small fraction of the ray-tracing cost" this way. **[VENDOR]** Note the scene
here is 2× supersampled, so tracing in the forward shader would trace four
times the necessary rays; a separate half-res pass is roughly a sixteenth of
that.

### 5.4 Hi-Z acceleration

Build a min-depth mip pyramid and march it hierarchically: start at mip 0, drop
to a coarser mip when nothing is hit, climb back on a collision. Skips large
empty spans. FidelityFX builds it with a single-pass compute downsampler using
groupshared memory rather than sequential dispatches, which avoids pipeline
stalls. **[VENDOR]** Compute is available here.

### 5.5 Glossy — importance sampling and denoise

A single mirror ray gives mirrors only. Glossy reflections come from importance
sampling the GGX **visible** normal distribution — "the potential set of
unoccluded, valid rays" — followed by:

- **spatial filter**, Halton-sequence neighbours weighted by normal, depth,
  radiance and variance difference so it does not overblur;
- **temporal resolve**, which **cannot be ordinary TAA**: reflected objects move
  according to *their own* depth, not the depth of the surface reflecting them.
  Reconstruct the reflected point from the surface point plus the hit distance
  stored during the trace, and bias the history blend by frames-since-
  disocclusion. **[VENDOR]**

### 5.6 Classification and fallback

Rough pixels **skip tracing entirely** and take the prefiltered probe; only
smooth ones trace. Write the environment radiance for skipped pixels into a
separate target. **[VENDOR]** This is why stage one comes first.

Rejection rules, each falling back to the probe by confidence:

- screen-edge — hits outside the viewport, faded rather than cut;
- backface — the reflected surface must face the camera;
- self-intersection — reject hits in front of the reflecting surface;
- rays toward the camera;
- **thickness** — a depth-buffer thickness parameter so thin geometry does not
  occlude more than it should. **[VENDOR]**

---

## 6. Verification

Add to `TexturePreviews`/the textures tab as each stage lands, because every
one of these has already been an invisible bug once:

- probe array **per mip** — a slider or one entry per level. Prefilter output
  that is subtly wrong at mip 4 is invisible in the lit frame.
- G-buffer **F0** channel.
- **scene colour** history.
- SSR **radiance** and **confidence**, separately. Confidence is what explains a
  reflection that is missing rather than wrong.

Follow the project's verification rule: **build the diagnostic, amplify it until
the defect is unmistakable, then hand the visual judgement to the user.**
Localising a reported defect is ours; deciding whether it looks right is not.

---

## 7. rlgl and raylib traps already paid for

Each of these cost real time in this codebase. They are documented at the sites
that hit them, and collected here because a new chat will hit the same ones.

- **Material map slots 7, 8, 9 are cubemap slots.** `DrawMesh` binds them with
  `rlEnableTextureCubemap`; a `sampler2D` reading one gets black, silently. The
  transmission plane sat in slot 7 and read zero everywhere for weeks.
- **`rlLoadTextureCubemap` refuses to allocate an *empty* float cubemap.** Pass
  a zeroed buffer and it takes the other branch, which has no such restriction.
  Its mipmapped path *is* correct — it halves `mipSize`, advances the data
  pointer per level and recomputes `dataSize`. **[VERIFIED** rlgl.h:3450-3459**]**
- **rlgl cannot generate cubemap mipmaps** (`rlGenTextureMipmaps` binds
  `GL_TEXTURE_2D`) and has no cubemap-array API at all. Hence the raw-GL door in
  `ReflectionProbeSet`.
- **Cubemap face rendering flips triangle winding.** A cubemap's faces are
  defined left-handed, so a view matrix built for them reverses winding and
  backface culling discards the entire scene — framebuffer complete, draws
  submitted, six faces of nothing. Disable culling for the capture.
- **Id buffers must be `TEXTURE_FILTER_POINT`.** Bilinear between object 3 and
  object 7 returns 5, an id belonging to nothing, along every silhouette edge.
- **Sampler uniforms are just ints.** A sampler never pointed at its unit reads
  unit 0 — whatever the albedo happens to be — and the effect silently does
  nothing. Two full debugging sessions went to this.
- **A framebuffer preview needs exactly one flip.** GL's origin is bottom-left,
  ImGui's is top-left; flipping in both the copy and the viewer cancels out.

---

## Sources

- [Image-based Lighting approaches and parallax-corrected cubemap — Lagarde 2012](https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/)
- [Parallax Corrected Cubemaps — Valve Developer Community](https://developer.valvesoftware.com/wiki/Parallax_Corrected_Cubemaps)
- [env_cubemap_box — Valve Developer Community](https://developer.valvesoftware.com/wiki/Env_cubemap_box)
- [Cubemaps (Source 2) — Valve Developer Community](https://developer.valvesoftware.com/wiki/Cubemaps_(Source_2))
- [Troubleshooting reflections (Box Projection) — Unity](https://docs.unity3d.com/Manual/AdvancedRefProbe.html)
- [Efficient GPU Screen-Space Ray Tracing — McGuire & Mara, JCGT 2014](https://jcgt.org/published/0003/04/04/paper.pdf)
- [Stochastic Screen-Space Reflections — Frostbite](https://www.ea.com/frostbite/news/stochastic-screen-space-reflections)
- [AMD FidelityFX Stochastic Screen-Space Reflections](https://gpuopen.com/fidelityfx-sssr/)
- [Notes on screenspace reflections with FidelityFX SSSR — Interplay of Light](https://interplayoflight.wordpress.com/2022/09/28/notes-on-screenspace-reflections-with-fidelityfx-sssr/)
- `study/source2_rendering.md` §4 (probes), §12.3 (scene-colour grab for refraction)
