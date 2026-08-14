# Ray-traced lighting and shadows — what it costs, and what "on par with RE ENGINE" means

A design note, not research. It answers one question asked directly: *how hard
would it be to add ray-traced lighting and shadows, on par with RE ENGINE?*

The short version is that the question contains two separate questions and the
second one dissolves most of the first. **Can this engine trace rays in
hardware** is a large, well-defined, API-shaped answer: not at any OpenGL
version, ever, so it is a Vulkan backend first and a renderer feature second.
**What RE ENGINE's ray tracing actually does** is the more useful half, and the
answer — read from the deck research already in
[`re_engine_rendering.md`](../games/rendering/re_engine_rendering.md) §6 — is
that for five years it did *indirect illumination only*, and that as late as
Dragon's Dogma 2 in 2024 a ray-traced-GI title still took its shadows from a
shadow map.

State verified against the tree on 2026-08-14. Tags follow the `study/`
convention: **[VERIFIED]** read from this codebase, **[VENDOR]/[PAPER]** from a
cited source, **[inferred]** reasoned rather than sourced.

---

## 1. The blocker is the API, and it is absolute

**OpenGL has no ray tracing, at any version, and never will.** This is not a
gap waiting to be filled — it is a decision, stated by NVIDIA on their own
developer forum: *"Unfortunately, NVIDIA never added Ray Tracing Support to
OpenGL."* The follow-up from NVIDIA's own staff is that they chose against
shipping hardware RT as a proprietary GL extension and Khronos never pursued it
for the core specification. The recommendation is Vulkan. **[VENDOR]**

There is one piece of misinformation worth naming, because a search will find
it within two results and it looks like the opposite answer. **`GL_NV_ray_tracing`
exists, and it is not an OpenGL extension.** It is a *GLSL* extension string —
`#extension GL_NV_ray_tracing : require` — used when writing GLSL that will be
compiled to SPIR-V and run under **Vulkan**. The `GL_` prefix is GLSL's naming
convention, not an API claim. Headlines describing NVIDIA "stabilising its
Vulkan/OpenGL ray-tracing extension" are describing the shader language, not a
GL entry point. **[inferred**, from the extension's registry home and the forum
thread above**]**

So hardware ray tracing means **Vulkan or D3D12**, and for this project that
means Vulkan, which is the same conclusion
[`console_porting.md`](console_porting.md) §4.5 already reached from a
completely different direction: *"the first real step toward a console is not a
console but a Vulkan backend on Windows, verifiable against the existing GL
path, and the only honest moment to design the RHI."*

**That is the load-bearing fact of this whole note.** Ray tracing is not a
rendering feature this engine could add. It is a rendering feature that rides on
a backend this engine does not have, behind a migration that is not finished.

A second, much smaller thing *is* available today and is not the same thing at
all — software ray marching in compute, over the voxel lattice. §5.

---

## 2. What "on par with RE ENGINE" actually means

The per-title table, from Capcom's *Advances in Ray Tracing* slide 41
**[COC23]**, reproduced from `re_engine_rendering.md` §4.4:

| title | version | ray traced |
|---|---|---|
| DMC5 Special Edition | 1 | GI, reflections |
| Resident Evil Village | 2 | GI, reflections, AO — backgrounds only |
| Resident Evil 2 / 3 / 7 | 2 | GI, reflections, AO, including characters |
| **Resident Evil 4** | 2 | **reflections only** |
| Exoprimal | 2 | cutscenes only |

Three things fall out of it, and each one moves the answer:

**Ray-traced shadows are not on this list, because they did not exist.** No
runtime ray-traced shadow appears in v1 or v2 at all. The first is the v3
area-light shadow, and the evidence is worked through in `re_engine_rendering.md`
§6.1 — including the *reason a technique was abandoned*, which is the best kind
of evidence: Capcom dropped the "guiding light position" idea because *"it needs
shadow rays to check if small geometry lights are reachable. **Geometry lights
don't have shadow maps.**"* That sentence only parses in an engine where the
directional, punctual and area lights all *do*. **[COC23]**

**Dragon's Dogma 2, 2024, says it in one line** **[REAC25]**:

> "Dragon's Dogma 2 uses raytracing for global illumination. **GI lighting reuses
> shadow map for shadows. Unable to represent shadows outside field of vision.**"

A ray-traced-GI title taking its shadows from the rasterised map, and hitting
the obvious wall — a shadow map only covers the frustum. Their fix is worth
knowing on its own merits and needs no ray tracing hardware to imitate: **inject
the previous frame's ray-hit positions into the shadow map** to extend coverage
past the camera.

**And the best-looking title in the series ships the least of it.** RE4R is
reflections only — no RT GI, no RT AO, no SDF — running probe-plus-cubemap
indirect. `re_engine_rendering.md` §11.0 states the conclusion the whole document
was written to reach: *there is no exotic technique; what is exceptional is
integration.*

So, stated plainly:

> **"Ray-traced lighting and shadows on par with RE ENGINE" means ray-traced
> indirect illumination and reflections, with direct shadows still rasterised
> from a shadow map.** Parity on the *shadow* half is a better shadow map, not a
> ray. Parity on the *lighting* half is what §3 prices.

The exception is the path-traced line — Requiem and PRAGMATA, Dec 2025 — where
direct lighting finally moves into the tracer **[GDC26]**. That is a different
renderer wearing the same name, it costs 8.78 ms at 1080p on an RTX 4070 Ti, and
it took two developers roughly 1.5 years on an engine that already had every
prerequisite in §3.

---

## 3. The prerequisite chain

Everything hardware RT needs that this tree does not have. This is the honest
cost, and note how little of it is "ray tracing".

| prerequisite | state today | tracked in |
|---|---|---|
| A backend that can trace | **none.** `rhi/` has zero hits for accel/ray/BLAS/TLAS vocabulary **[VERIFIED]** | this note |
| RHI at parity, raylib deleted | in progress — UI text, dev panel, decals, material textures outstanding | `rhi/MIGRATION.md` §4.1–4.13 |
| Offline shader toolchain (glslang → SPIR-V) | does not exist; dialect is "GLSL 450 a GL driver accepts" | `MIGRATION.md` §4.9 — **optional today, mandatory under Vulkan** |
| **An engine-owned render scene** | does not exist — `IGeometrySource` is a submit callback | `MIGRATION.md` §4.12 |
| G-buffer F0 (RGB) | absent; every surface assumed dielectric at 0.04 | `realtime_reflections.md` §2.2 |
| Scene colour buffer | absent | `realtime_reflections.md` §2.3 |
| Motion vectors / temporal history | **zero hits across `src/`** **[VERIFIED]** | this note |
| More than one light | **zero local lights; one directional sun** **[VERIFIED]** | `re_engine_rendering.md` §11 |

Two of those rows deserve more than a table cell.

### 3.1 A TLAS is a scene, and the engine does not own one

An acceleration structure is a **top-level list of instances with transforms and
geometry references, rebuilt or refitted every frame**. It is, structurally, the
render scene — the thing `MIGRATION.md` §4.12 says the engine must eventually
own and explicitly defers as *"a second migration on top of this one"*.

You cannot build a TLAS out of `IGeometrySource::submit(encoder, pass)`. A
submit callback hands the engine draws; a TLAS needs the *list*, persistent
across frames, with stable instance identity so the structure can be refitted
rather than rebuilt. **Ray tracing is therefore blocked behind the exact
architectural item the migration document defers**, and it does not merely want
it — it cannot be expressed without it.

This is worth knowing even if RT is never built, because it is the second
independent argument for the render scene. The first was that the engine can
neither cull nor sort what it does not own.

### 3.2 The denoiser is the architecture

`re_engine_rendering.md` §6.6 is the section to read before costing any of this.
A 1-ray-per-pixel signal is not an image; it is noise that a temporal filter
turns into an image, and **the filter constrains everything upstream of it**.
Capcom spend ten slides on the consequence: *anything that changes a surface's
appearance without changing a guide buffer gets denoised away* — shader-animated
normals, translucent raindrops, animated cookies, animated emissive, the
screen-space SSS blur. Every fix has the same shape: write the change into a
buffer the denoiser watches.

The cost split says the rest. Village on PS5, v2: **~4.7 ms total, of which
tracing and shading is 2.3 ms and the denoiser is 2.2 ms** **[COC23]**. Half the
bill is reconstruction, and this project has **no motion vectors, no temporal
reprojection and no history buffer of any kind** **[VERIFIED]** — which
`re_engine_rendering.md` §6.6 currently records as a *virtue*, because the 2×
supersample reconstructs from nothing and therefore has no equivalent failure
mode and no tuning burden. Adding RT spends that virtue.

---

## 4. The cost, in order

Assuming the goal is RE ENGINE v2 parity — RT GI, reflections and AO, shadows
still rasterised — and that everything is done properly rather than spiked:

| stage | what | rough scale |
|---|---|---|
| 0 | Finish the RHI migration to parity, delete the raylib renderer | the existing plan, `MIGRATION.md` §4 |
| 1 | Offline shader toolchain — glslang → SPIR-V → SPIRV-Cross | weeks; already wanted independently |
| 2 | **Vulkan backend** for `rhi::IRenderDevice` — swapchain, descriptor sets, sync, VMA, pipeline cache | the single largest item. Months. Verifiable against the GL path, which is why the RHI self-test exists |
| 3 | Engine-owned render scene (`MIGRATION.md` §4.12) | a migration in its own right |
| 4 | RT vocabulary in the RHI — BLAS/TLAS descriptors, build and refit, a capability bit that is `false` on GL | weeks, given 2 and 3 |
| 5 | Guide buffers: RGB F0, scene colour, **motion vectors** | weeks each, all wanted independently |
| 6 | The tracer — inline `RayQuery` from compute, not DXR hit shaders | see below |
| 7 | **The denoiser** | the real project |

**Stage 6 is smaller than it looks and stage 7 is larger.** Capcom use *inline*
ray tracing — `RayQuery` through a single material function rather than hit
shaders and a shader binding table **[COC23]** — which is the cheap path and
sidesteps the entire ray-pipeline object model. Their path tracer and their RT
mode *share one pipeline* for this reason. Stage 7 is where the two-developer,
1.5-year figure lives.

**A blunt calibration.** Stages 0–5 are all things this engine wants for
reasons that have nothing to do with ray tracing — Vulkan for consoles, the
render scene for culling and sorting, F0 for metals, scene colour for water,
motion vectors for any temporal technique at all. Stage 6–7 is the only part
that is *ray tracing*, and it is the part Capcom staffed with two people for
eighteen months.

---

## 5. What is reachable today, and it is more than it sounds

**This project has an acceleration structure already, and it is better than a
BVH for the geometry it covers.**

The world is a **24×24 lattice** **[VERIFIED** `lattice/Constants.hpp`:36-37**]**
summarised into [`OcclusionGrid`](../../src/game/world/OcclusionGrid.hpp) at
**two bytes per cell** — roughly 10 KB for the demo map, cache-resident whole —
and [`RayCaster`](../../src/game/los/RayCaster.cpp) already walks it with a
**3D DDA**, keeping a running index rather than recomputing one. Compute is
available and wrapped (`gpu/compute/ComputeShader.hpp`), and the RHI has
`beginCompute` and a `capabilities().compute` bit. **[VERIFIED]**

A voxel-DDA tracer in a compute shader needs **no BVH, no acceleration-structure
build, no Vulkan and no RT hardware.** The traversal is the CPU one ported to
GLSL. What it can honestly buy:

- **Sun visibility** per cell or per probe, correct beyond the frustum — which
  is precisely the wall Dragon's Dogma 2 reports hitting and works around by
  injecting ray hits into the shadow map;
- **Sky occlusion / single-bounce GI** over the static architecture, which is
  the term §6.1 of the RE document says ray tracing was *for* during the years
  it looked best;
- **Cone-traced soft shadows** at a controllable ray count.

**And what it cannot do, stated first rather than discovered later.** The
lattice describes the *architecture*, not the *render geometry*. Props, unit
bodies, ramps, offset floor slabs and every cell carrying `kNeedsTile` are
invisible to it, and the escape hatch that makes `OcclusionGrid` honest on the
CPU — fall through to the Tile — has no cheap GPU equivalent. Voxel-derived
shadows on mesh geometry read wrong exactly where the eye looks: at contact.
Treat it as a *low-frequency indirect and long-range visibility* term composited
under the rasterised shadow map, never as a replacement for it. **[inferred]**

The neighbouring idea is already priced: `re_engine_rendering.md` §5.1 works
through why an **SDF is cheaper for us than it was for Capcom** — one field
rather than a thousand instanced ones, built by a chamfer sweep over the
occupancy grid rather than by baking rays outward from voxel centres, with no
clipmaps and no overlap to resolve.

---

## 6. The recommendation

Ranked, and it lands on the same order `re_engine_rendering.md` §11 already
reached from the opposite direction — which is the main reason to trust it.

1. **Local lights.** One directional sun is the ceiling on every interior in
   this game, and no amount of ray tracing addresses it. Clustered forward is
   already the chosen direction.
2. **Finish the reflections plan** — `realtime_reflections.md` stages two and
   three: RGB F0, scene colour, SSR. That is RE4R's stack, and RE4R is the
   best-looking title in the series.
3. **Probe GI**, four-direction storage. RE4R's evidence is that this class of
   indirect is enough.
4. **A voxel or SDF sun-visibility term** in compute, per §5 — the honest,
   reachable, hardware-free version of the thing being asked for.
5. **Vulkan backend**, when consoles or a measured GL limitation force it — not
   for ray tracing.
6. **Ray tracing**, if ever, as an increment on top of 5.

The uncomfortable framing, because it is the useful one: **the gap between this
renderer's frame and RE4R's is not a technique this engine cannot reach.** It is
local lights, indirect that varies over the room, authored material variety and
a colour pipeline. Every one of those is reachable at GL 4.3 today, and none of
them is ray tracing.

---

## 7. What would change this answer

- **A Vulkan backend landing for console or driver reasons.** Then RT drops from
  "a project behind a project" to stages 4–7, and stages 4–6 are modest. This is
  the single event to watch.
- **The render scene landing** (`MIGRATION.md` §4.12). It is on the roadmap for
  culling and sorting; it happens to be the TLAS.
- **Content changing shape.** The lattice-as-acceleration-structure argument in
  §5 holds because the world is 24×24 and mostly architecture. A project on this
  engine with streamed instanced meshes has RE ENGINE's problem, not ours, and
  §5's advantages evaporate.

---

## Sources

- [Accessing RT Cores HW from OpenGL API extensions? — NVIDIA Developer Forums](https://forums.developer.nvidia.com/t/accessing-rt-cores-hw-from-opengl-api-extensions/247118) — NVIDIA staff confirming OpenGL has no ray tracing, deliberately
- [Vulkan Ray Tracing Final Specification Release — Khronos](https://www.khronos.org/blog/vulkan-ray-tracing-final-specification-release)
- [Ray Tracing In Vulkan — Khronos](https://www.khronos.org/blog/ray-tracing-in-vulkan)
- [`re_engine_rendering.md`](../games/rendering/re_engine_rendering.md) §4.4–4.6, §5.1, §6, §11 — the Capcom deck research this note rests on
- [`realtime_reflections.md`](../realtime_reflections.md) — the stage plan for the reachable version
- [`console_porting.md`](console_porting.md) §4.5 — the Vulkan-before-console argument, reached independently
