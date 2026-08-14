# The RHI migration — status, plan, and the traps already paid for

Run it with `xcom.exe --renderer rhi`. The raylib renderer is still the default
and still the shipping one; both are built, and the raylib path is deleted only
at parity.

This file exists so the work can be picked up cold. The **Traps** section at the
bottom is the most valuable part — every entry cost real time to find, and most
of them look like a different bug than they are.

---

## 0. What this is all FOR

Read this before the task list. The tasks only make sense against it.

### The game developer's experience

Starting a new project on cromwell should mean writing **assets, shaders,
models, gameplay and networking**, and COMPOSING what the engine offers. It
should not mean writing rendering.

A game says *"I want this on PC, PS5 and Switch"*. It does not say — and should
not be able to say — which graphics API, which backend, or what a pass is.
**A game developer should never encounter the word RHI.** Unreal has one
(`FRHICommandList`, `FDynamicRHI`); no game developer has ever seen it. That is
the standard to hit.

### Mechanism versus composition

The line to judge every seam against:

- **The engine owns the mechanism** — the scene, culling, sorting, batching, the
  frame sequence, the components you draw with.
- **The game composes**, and may EXTEND the vocabulary: its own physics
  groupings, its own render layers for something like an outliner, its own
  gameplay categories. Extension is welcome.
- **The game never says HOW or WHAT is drawn** — no pass callbacks, no issuing
  draws, no knowing what a shadow pass wants.

The test for a seam: **would a new project have to write this again?** Extension
points pass. Re-implementation does not.

`IGeometrySource` currently fails this test, knowingly — see §4.12. So does the
existence of a game-side `RhiFrameRenderer`: a game should have no renderer at
all, and certainly not one named after an engine internal.

### Boundaries you can swap

Two axes, **platform first, library second**, because they change independently:

```
platform/            the interfaces
platform/pc/         win + linux + mac  (only the per-user directory differs)
platform/pc/raylib/  the third-party library backing it
platform/ps5/        a folder per console; xbox and switch are siblings
rhi/                 the graphics interfaces
rhi/pc/opengl/       the PC graphics backend
```

Swapping raylib for a hand-rolled GL layer is `pc/opengl/` beside `pc/raylib/`,
not a new platform. That distinction is the point of the two axes.

**The swap points are enumerable, and that is what makes this a plan rather than
a hope:** `ISurface`, `IInput`, `IClock`, `IFileSystem`, `IImageDecoder`,
`IPlatform`, `rhi::IRenderDevice`. Seven. Before they existed the answer was
"raylib appears in 107 of 400 files, and you find out which by breaking the
build".

**What keeps the boundary honest:** selection is a CMake decision, NOT an
`#ifdef`. A console build never hands desktop sources to a compiler, so platform
code that leaks into a shared file fails to build the moment a second platform
is configured — rather than years later. `IPlatform::create` has exactly one
definition per build, so two backends linked at once is a duplicate-symbol
error, which is the right way to find that out.

The list only stays at seven if new engine code goes through the interfaces
rather than reaching for a backend. `cromwell_base` is build-enforced;
`cromwell` is discipline, because it still links raylib for the path being
replaced.

---

## 1. Why there are two renderers

raylib binds shader inputs by its own naming convention, so a shader converted
to explicit bindings and a `std140` block cannot be driven by it. That means the
pass must bind its own resources, which means its render target must be a device
texture, which pulls in whatever else writes that target. The chain closes over
most of the renderer, so converting in place would leave the tree unable to draw
a correct frame until every link was done.

Hence: a parallel path, chosen at startup, deleted at parity.

## 2. The seams, and who owns what

| Boundary | Interface | Notes |
|---|---|---|
| Platform | `IPlatform` + `ISurface`/`IInput`/`IClock`/`IFileSystem`/`IImageDecoder` | Six interfaces and one factory is the whole cost of a new platform |
| Graphics | `rhi::IRenderDevice` | Explicit, command-buffer shaped. `pc/opengl/` today |
| Frame sequence | `cromwell::ScenePipeline` | **Engine.** Owns every target, shader, pipeline and the pass order |
| Geometry | `IGeometrySource` | **Game.** Two methods: `submit(encoder, pass)` and `worldBounds` |
| Materials | `.mat` files → `DeviceMaterials` | Data, never C++ |
| Probes | `DeviceProbeSet` + `ProbePlacement` | **Engine** owns the array and the schedule; **game** says where the rooms are |
| UI | `ui::IUiPainter` | `DeviceUiPainter` vs the raylib `UiPainter`. **Every command kind converted** |
| Text | `ui::GlyphAtlas` | **Engine.** The bake is neutral; each painter does its own upload |

**The rule:** if converting a pass grows `RhiFrameRenderer`, something
engine-shaped has leaked into the game. It has stayed roughly constant through
every pass so far; that is the property worth watching.

## 3. Done

**Platform + device**
- Six platform interfaces, folders by platform then library (`pc/raylib`,
  `rhi/pc/opengl`, `ps5/README.md` as the port contract)
- `OpenGlRenderDevice` with a 22-stage conformance self-test (`--device-selftest`)
- Source *selection* in CMake, not `#ifdef` — a console build cannot contain
  desktop code

**Scene passes** (all in `ScenePipeline`)
- Shadow map — 4096², focused on the camera frustum, texel-snapped, PCSS
- Shadow transmission plane — RGBA8 at half the shadow size
- Depth prepass, SSAO **+ its bilateral blur**
- Sky (analytic, two lobes)
- Reflection probe capture — one cube face per frame, four while stale
- Reflection probe prefilter — GGX-convolved mip chain, once per probe per sweep
- Lit (opaque), Transparent
- Tone map, resolving a 2× supersample

**Shading**
- Full Cook-Torrance via `common/brdf.glsl`, **shared with the raylib renderer**
- Real sun, sky and exposure from `SunLight`
- Per-object transforms as push constants (`rhi/object.glsl`)
- Unit bodies as device geometry

**Materials — data, not code**
- `assets/materials/<name>.mat`, parsed into the shared `PbrMaterial`
- **Blend mode is a material property.** `blend translucent` is the only thing
  that puts a surface in the transparent pass. Water is a `.mat`, not a feature
- `DeviceMaterials::isTranslucent` replaced a hardcoded `SurfaceKind` switch

**Reflection probes**
- `DeviceProbeSet` — cubemap array (16 probes × 6 faces × 128² RGBA16F = 12 MB),
  volumes, the volume block, round-robin schedule. **Names no graphics API**: the
  RHI has cube arrays (`TextureDesc::cube` with `6 * probes` layers) and per-slice
  attachment (`ColourAttachment::layer`), so unlike `ReflectionProbeSet` this
  needed no raw GL and no exemption in CMakeLists
- **Capture pass** — `ScenePipeline::drawProbeCapture`. The lit and transparent
  shaders from an arbitrary eye into one slice: no prepass (so `Less` and depth
  written, not `Equal`), no SSAO (a 1×1 white stands in), no supersample, no sky
  (clear to transparent black — alpha is "world in this direction"), and no
  probes (`uProbeParams.x = 0`, because the array being written cannot be read)
- `rhi/probes.glsl` — per-pixel selection, parallax correction, doorway
  crossfade, slotted into `ambientSpecular` in both lit shaders exactly where the
  analytic sky was. `DeviceProbeSet::Volume` is the block's C++ half
- Placement is shared: `placeProbeVolumes` returns plain data and both probe
  sets adapt it, so the outdoor fallback and the exact-bounds rule exist once

**What probes do NOT have yet:** a prefiltered mip chain (the term slides back to
the analytic sky by roughness 0.55, exactly as the raylib path does), seamless
cube filtering, a dev-panel preview of a layer, and re-placement after
destruction — the rhi world is built once and never rebuilt, so there is nothing
to hook that to yet.

**UI — complete**
- `IUiPainter`; `DeviceUiPainter` does shapes, clipping and screen mapping
- **Backdrop blur** — the pass splits at each frosted panel to copy the
  backbuffer, mip it and read a fractional level. `IRenderDevice::copyBackbufferToTexture`
  is the one interface addition it needed
- **Text.** `GlyphAtlas` is the bake — FreeType, native hinting, the phase
  shift, the shelf pack and the coverage curve — and it names no graphics API.
  `UiFontSet` caches those and builds a raylib `Font` from one lazily; the
  device path asks only for atlases and never creates a raylib texture. A
  second pipeline (`rhi/ui_text.*.glsl`) draws glyph quads from an R8 atlas,
  interleaved with the shape commands so painter's order survives
- Two things fell out of the split rather than being aimed at: `loadWeight` no
  longer needs a GL context, because validating a font file is not a graphics
  operation; and `measure()` no longer goes through `MeasureTextEx`, so the
  layout above it is arithmetic over the atlas's own metrics and both painters
  advance the pen by the same numbers
- **Two placement decisions now live in `UiFontSet` and are ASKED FOR by both
  painters and by the layout** — `trackingPx` and `runOriginY`. Each replaced a
  copy of the arithmetic in every caller, and each copy had drifted into a
  visible bug: text drawn wider than the box measured for it, and text centred
  on a line box rather than on the ink. Both were reported as "the padding looks
  uneven". The reasoning is on those two functions

## 4. Remaining, in order

### 4.1 ~~UI text~~ — done

The estimate here said "`UiFontSet` holds raylib `Font`s, so this is real work
rather than a binding change". Half right, and the half that was wrong is worth
keeping: by the time it was done, `UiFontSet` was ALREADY rasterising its own
atlas with FreeType and packing it by hand — hinting, phases, coverage curve,
shelf pack — and raylib appeared only in the last dozen lines, where the pixels
became a `Font`. The note had aged past its subject.

So the work was a split, not a port: `GlyphAtlas` (bake, neutral) and the upload
at each painter. What that bought beyond the device path drawing text:

- **`loadWeight` no longer needs a GL context.** It used to upload as it
  validated, which made "is this file a font" a graphics call and put a
  load-order constraint in a signature that did not show one.
- **`measure()` no longer goes through `MeasureTextEx`.** It sums the atlas's
  own advances, which is what both painters move the pen by. A layout measured
  through one backend and drawn through another is a divergence waiting for the
  day the backends stop being the same process.

**The remaining gaps, all small and all deliberate:**

1. **No fallback typeface on the device path.** `atlasFor` returns null when no
   weight loaded, and the run draws nothing and counts as skipped. `fontFor`
   still falls back to raylib's built-in for the raylib path. Offering the same
   here would mean handing a raylib resource to the device painter, which is the
   dependency the split exists to remove — the honest answer is a tiny embedded
   face in the engine, and that is a decision about shipped assets rather than
   about rendering.
2. **ASCII 32–126, one atlas per (weight, size, phase).** A wider charset is a
   localisation decision and a packing one; it is not smuggled in ahead of it.
3. **The glyph textures are keyed by atlas ADDRESS**, which is sound because the
   font set caches in a `std::map` — but only while the cache lives. That is
   what `UiFontSet::generation()` is for; see the trap at the bottom about what
   the alternative would have looked like.

**HOW IT IS VERIFIED — AND THE GALLERY NOW RUNS ON BOTH RENDERERS, WHICH IS THE
PART WORTH KEEPING.**

`xcom.exe --renderer rhi --ui-gallery` draws the whole widget gallery through
the device painter, including the 10-to-56 px size ladder and the weight row.
That was not free and it was not a detour:

- `WidgetGallery::draw` takes `cromwell::Camera` and converts inside, so the
  device renderer can call it without raylib in its signature.
- `setWorldTextSample(false)` drops the ONE section that cannot follow — the
  MSDF sample is real geometry drawn through `BeginMode3D`. Everything else in
  the gallery is draw-list commands, which is the whole reason the kit narrowed
  to one painter.
- `Application` routes the toggle to BOTH renderers. It previously routed only
  to `FrameRenderer`, so `--renderer rhi --ui-gallery` silently did nothing —
  the switch was reaching a renderer that was not drawing the frame.

**WHY THIS MATTERS MORE THAN THE FEATURE IT VERIFIES.** A diagnostic screen that
only runs on the renderer being replaced can only settle an argument by
argument. With the same screen on both, "is the device path's text right" stops
being a matter of taste and becomes a diff:

| Region | raylib vs rhi |
|---|---|
| Type ladder, all eight sizes | **identical, max channel difference 0** |
| Weight row, Regular to ExtraBold | **identical** |
| Column headings, button labels, tip-panel body | **identical** |
| Loader rings, top left | differ — they animate, and two runs are not at the same phase |
| Panel backgrounds, right column | differ — backdrop blur is §4.2 |

Not "looks the same": the same bytes. Which is what should be expected, because
both painters now draw from one atlas and the pen arithmetic was ported rather
than re-derived — and it means any future divergence is a real regression with
a one-command test. Captures and the ×8 amplified diff are in
`builds/win/debug/`.

The remaining blur difference is the honest measure of what §4.2 is worth.

### 4.2 ~~UI backdrop blur~~ — done

`DeviceUiPainter` now draws every command kind the draw list has, and
`skippedCommands()` reports zero on the gallery screen. **The UI is converted.**

It needed one addition to the interface: **`IRenderDevice::copyBackbufferToTexture`**,
which is a device call rather than an encoder one and refuses to run inside a
pass. GL would not care — copying from the bound framebuffer is legal anywhere —
but Vulkan forbids an image copy inside a render pass and Metal needs a different
encoder, so an interface that allowed it here is one that cannot be implemented
on three of four targets without secretly splitting the pass behind the caller.

So the painter splits it, visibly: each frosted panel ends the pass, copies,
generates mips, and reopens with `LoadAction::Load`. That is the cost, it is in
the caller's own code where a tiler's bill can be seen, and it fixes the ordering
for free — everything appended before the blur is on the backbuffer when the copy
happens, so a panel frosts the UI beneath it and not merely the scene.

**A MIP LEVEL SETS THE RADIUS; IT DOES NOT SET THE LOOK.** Reading the level with
a single `textureLod` — the obvious implementation, and what `UiPainter` does —
comes out visibly blocky, reported as "pixelated and stair-steppy". Two causes
stack: `glGenerateMipmap` is a box filter, so a bright object leaves a square
smear rather than a soft falloff; and magnifying level 3 of a 1280-wide capture
means stretching 160 texels over 1280 pixels, which the bilinear unit does
piecewise-linearly and the eye reads as creases at every texel boundary. No
number of extra mip levels fixes the second one — the data really does have only
160 samples.

`rhi/ui_blur.fs.glsl` gathers eight taps at the sampled level instead, in the
dual-filter (Kawase) upsample pattern. Overlapping tents sum to something close
to a Gaussian, which has no creases. Eight texture reads, no extra targets, no
extra passes, inside a draw that was happening anyway. A genuinely wide blur
still wants the full downsample/upsample ladder and its own targets.

Two smaller decisions worth keeping:

- **The blur level is a push constant read by `textureLod`, not a sampler LOD
  clamp.** The raylib painter has to pin the texture's LOD range and put it back
  afterwards, because rlgl's shader cannot be told a level. A clamp is state on a
  shared object: two panels at different strengths in one frame need two samplers
  or two round trips, and forgetting the restore leaves every later read of that
  texture pinned to a blur.
- **Glyph quads and blur outlines share one vertex buffer.** Both are position,
  UV and colour; they differ in what the UV addresses and which shader reads it,
  which is what a pipeline is for and not what a second buffer is for.

**AND IT FOUND A REAL BUG IN THE SHIPPING RENDERER — see §5.** The device path is
correct here and `UiPainter` is not, so this is the one place the two paths are
expected to differ.

### 4.3 Reflection probes — done; what is left of them
The capture, the selection and the sampling all landed (see §3). Since then, from
one reported bug — geometry appearing on the **wrong side** of an exterior
window:

- **The outdoors is a grid of blocks, not one board-sized probe.** A cubemap is
  an environment at infinity and parallax correction only repairs that as well as
  the box allows; a board-sized box is the degenerate case. Each block now
  carries a box its own size and a capture point in an open street cell inside
  it. `study/realtime_reflections.md` §2.4 has the sources and the measurements.
- **Priority is explicit and volumes may OVERLAP.** It was the influence box's
  volume, smallest-wins, which cannot say "this overrides that" between equal
  boxes and therefore forbade overlap — equal volumes tied and the tie fell to
  array order. Now high wins, ties break on depth inside the box, and equal
  priorities split proportionally. This is `env_cubemap_box`'s model and it is
  what a level designer needs.
- **`Aabb::volume()`** landed on the shared type in `collision/Shape.hpp` rather
  than staying a private duplicate.

**A LIMIT WORTH KNOWING BEFORE MAPS ARE AUTHORED:** `selectProbes` tracks a
winner and ONE runner-up, so a fragment meaningfully inside three overlapping
volumes silently drops the third. That is why exterior blocks overlap by a single
tile. Deep stacking needs that loop to keep more candidates.

The remaining items, cheapest first:

1. **Re-placement after destruction.** The raylib path sets `probesDirty_` when
   the world is edited and re-floods next frame. `RhiFrameRenderer` builds its
   static world once and never rebuilds it, so there is nothing to hook to yet —
   it belongs with whatever eventually tells that renderer the statics changed,
   and a second answer to "has the world changed" invented before then is two
   answers that drift.
2. **A dev-panel preview of a layer.** A cubemap array you cannot look at is
   worse to debug than a single cubemap, because now there is also the question
   of *which* layer is wrong. Blocked on §4.5 rather than on anything hard.
3. ~~**Seamless cube filtering.**~~ **Done** — moved into device creation. It was
   a trap rather than a task, and the note is kept because the shape of it
   recurs: `GL_TEXTURE_CUBE_MAP_SEAMLESS` is CONTEXT-wide, `ReflectionProbeSet`
   enables it, and that set is still constructed alongside the device path — so
   the rhi renderer was getting seamless filtering **as a side effect of the
   renderer it exists to replace**, and deleting `FrameRenderer` at parity would
   have added a seam to every probe with the cause in an unrelated file. Anything
   else the raylib path enables context-wide has the same problem; this is the
   only one found so far.

   The original note follows.

   **Seamless cube filtering — and this one is a TRAP, not a task.**
   `GL_TEXTURE_CUBE_MAP_SEAMLESS` is a CONTEXT-WIDE enable, not a sampler state,
   and `ReflectionProbeSet::create` turns it on. That set is still constructed on
   the rhi path (verify with `--log-level debug`; the line is raylib narration and
   the default level hides it), **so the device renderer is getting seamless cube
   filtering today as a side effect of the renderer it exists to replace.**

   Delete `FrameRenderer` at parity and the rhi path silently loses it: every
   probe grows a seam along all twelve cube-face edges, and the cause is a
   deletion in an unrelated file. Move the enable into device creation BEFORE
   §4.11, not after.
4. ~~**A prefiltered mip chain.**~~ **Done, and it is invisible.** Six levels,
   split-sum GGX at 32 taps, level L at roughness L/(levels-1); the shader reads
   `textureLod` and the `smoothstep(0.12, 0.55)` sky fade is deleted. Verified:
   levels distinct and monotone, schedule fires once per probe per sweep (51 in
   250 frames, matching 13 probes x 3.9 sweeps).

   **Measured against the old fade: max 3/255 across the frame.** That is
   expected rather than disappointing — ambient specular is multiplied by
   `environmentBRDF` at f0 = 0.04 on every surface this board has, so the chain
   changes what a tiny term contains and not how tiny it is. It pays off on
   metals (needs F0 in the G-buffer) and as SSR's classification target, where
   rough pixels skip tracing entirely. See study/realtime_reflections.md §2.1,
   which now carries the measurement.

   It also needed one interface addition: **`SamplerDesc::minLod`/`maxLod`**.
   The prefilter renders into level N while sampling level N-1 of the same
   array, which is undefined unless the sampler provably cannot reach the level
   being written. The alternative was a full-size scratch array — megabytes to
   avoid two floats.

   The old note follows, for the reasoning behind refusing `generateMips`.

   **A prefiltered mip chain.** The one real feature gap. Today both renderers
   slide the probe back to the analytic sky as roughness rises, because a fully
   rough reflection converges on the irradiance and the two-lobe sky already
   approximates one. `generateMips` is **not** the answer — a box filter is not a
   GGX convolution, and it would be a different wrong answer with more memory
   behind it. It wants a compute dispatch per level.

What the change did *not* need, against the estimate above it: raw GL anywhere,
and a second copy of the placement rules. `placeProbeVolumes` returns plain data
and both sets adapt it.

The visible surface is small and that is expected: only the ladder (roughness
0.35) and the window (0.05) are smooth enough to show a probe. Everything else on
the board is 0.8, where the term has already blended back to the analytic sky.

### 4.4 Game overlays — HALF DONE, and bigger than this entry used to claim

It said these "go through the UI kit, so §4.1 landing is most of what they
needed". **They do not.** `game/render/overlay/OverlayRenderer` — visibility
field, cover shields, hover plate, path preview — draws WORLD-SPACE geometry
with `DrawCube` and `DrawLine3D` straight into raylib's immediate mode. It never
touches a `UiDrawList`. Converting text bought it nothing.

So the entry was an estimate written from the name of the thing rather than from
its code, which is the same mistake §4.1's note made about `UiFontSet`. Read the
file before believing an entry here.

The work decomposes into two halves that are worth keeping apart:

1. ~~**`cromwell/debug/DebugDraw` needs a device renderer.**~~ **Done.** It is a
   pass on `ScenePipeline` rather than a class of its own — the lines need the
   scene's depth buffer and the view projection, both of which the pipeline
   owns, and a separate renderer would have had to be handed them. `SceneFrame`
   carries the queue as a borrowed pointer; two pipelines over one vertex buffer
   give the depth-tested and x-ray passes.

   It needed one interface addition: **a vertex range on `ICommandEncoder::draw`**.
   `drawIndexed` already had `firstIndex`, `draw` had nothing, so two runs of
   vertices in one buffer could only be drawn by keeping two buffers or by
   inventing an index buffer that says 0,1,2,3… and describes nothing. Every
   backend takes a first and a count on a non-indexed draw.

   **The trap that cost the time: adding that parameter changed what the second
   argument MEANS.** `draw(mesh, 1)` was one instance and became one VERTEX, so
   the pass ran, bound, uploaded and drew a single vertex of a line list — which
   is nothing at all, with no error anywhere. Every pre-existing call site passed
   the mesh alone and was unaffected; the only casualty was the new code written
   in the same edit. If a defaulted parameter is inserted before an existing one,
   the compiler cannot help.

2. **`OverlayRenderer` is the hard half, and it is really §4.12 arriving early.**
   Its cubes are solid geometry, not lines, and it issues them itself — a game
   deciding WHAT is drawn and HOW, which is the exact seam the render scene is
   meant to close. Porting it as-is means inventing a second way for the game to
   push triangles at the device, and then deleting that when the scene lands.

   The cheap version is to let these become debug-draw primitives (a box is
   twelve lines) and accept the look changing. The right version is renderables
   the game registers. Worth deciding before writing either.

### 4.5 Dev panel
imgui on the rhi path. The widget gallery, which was going to be the first task
here, landed with §4.1 instead — it is what verifies text, so it had to.

### 4.6 Decals (DBuffer) + ribbons/glow

### 4.7 Textures in materials
`.mat` gains `albedo` / `normal` / `mrao` keys. **Note:** `assets/materials/`
holds no textures, but `assets/models/` has ~123 files that carry their own —
so the live route is `ModelAsset::adoptTextures`, not the material directory.

### 4.8 Props — blocked on content
`assets/models/props.txt` does not exist, so `PropSet` places zero instances on
**either** path. Converting it would produce no visible change. Not a code
problem.

### 4.9 Offline shader toolchain
glslang → SPIR-V → SPIRV-Cross. Until it exists the dialect is "GLSL 450 that a
GL driver accepts", which has already cost one bug (`gl_VertexIndex` rejected).

### 4.10 Debt
- **`Application` should adopt `IPlatform::beginFrame`.** It still pumps events,
  polls input and ticks the clock itself, so `beginFrame` is dead code. That is
  why the backbuffer size is refreshed in `endFrame` as a workaround. Restructuring
  the loop is delicate — double-pumping broke input once already
- `createSampler` hardcodes `mipCount = 2`, so every sampler gets a mipmapped min
  filter. Harmless today because all textures use immutable storage with the level
  count clamped
- `cromwell/diag/DepthDump` — keep or delete. It found the shadow up-vector bug in
  one shot after several rounds of reasoning failed
- SSAO radius/bias/strength are copied constants in `drawOcclusion`. They belong on
  `SceneFrame`, borrowed from the live `AmbientOcclusion` the way the sun is, so
  the dev panel's sliders reach both renderers
- Mark the CEF web surface PC-only in CMake

### 4.11 Scalability: keep it POSSIBLE, do not build it yet

The platform half is designed for: `XC_PLATFORM` selects sources, six interfaces
plus `IRenderDevice` is the whole port cost, and `LoadAction`/`StoreAction` were
put in the RHI specifically for the tile-based GPUs every mobile device uses.

The PIPELINE half is not. Its budgets are desktop constants baked into
`ScenePipeline.cpp`:

- `kShadowSize` 4096 (64 MB) and a half-size transmission plane
- `kSupersample` 2, so every scene target is 4x the window
- RGBA16F scene colour

On a phone that is roughly 200 MB of render targets and a fill rate that will not
hold 60. A mobile backend alone does not fix it.

NOT WORK TO DO NOW. It is a constraint on the work that IS being done: keep the
engine modular enough that quality settings can be added later without a rewrite.

The current shape already allows it, and the rules that keep it that way are
small:

- Budgets stay NAMED CONSTANTS IN ONE PLACE, so they can become settings
- Sizes reach shaders as UNIFORMS, never baked into shader source
- Each pass stays a separate method, so a preset can skip one entirely
- Formats stay in `TextureDesc`, never assumed by a shader

Break any of those and "medium preset" becomes a rewrite instead of a value.
The eventual goal is that a game says "target PC, PS5 and iOS, quality preset
high/medium/low" and never names a device, a backend or a pass.

### 4.12 Then: the render scene

**The division of labour this engine is FOR.** A game writes assets, shaders,
models, gameplay and networking, and COMPOSES what the engine offers.

The line is mechanism versus composition. The engine provides the components to
draw with and owns the machinery — scene, culling, sorting, batching, pass order.
The game places components and may EXTEND the vocabulary: its own physics
groupings, its own render layers for something like an outliner. That is welcome.
What it must not do is say HOW or WHAT is drawn.

`IGeometrySource` does not meet that bar and was never meant to. It is a
migration seam: the game implements it, the engine calls back, and the engine
owns no scene. Unreal, Unity and Godot all do the opposite — the game REGISTERS
renderables and the engine owns the list, culls it, sorts it, batches it and
draws it.

What the current shape costs:

- **The engine cannot cull.** It does not know what exists, so frustum culling
  has to be the game's job
- **The engine cannot sort.** The transparent pass draws in bucket order, not
  back-to-front, so two overlapping panes blend in the wrong order — and nothing
  in the architecture can fix it, because the engine does not own the draws.
  **This is the limitation most likely to force the issue**
- **The engine cannot batch or instance** draws it does not issue
- **Pass semantics leak into every game.** A second project must implement
  `submit()` AND know that `GeometryPass::Shadow` means casters-only over the
  whole world rather than the cutaway

The target: a render scene the game populates with renderables — mesh, transform,
material, flags — which the engine owns and traverses. `IGeometrySource`
disappears; `RhiStatics`/`RhiBodies` become things that POPULATE a scene rather
than things that draw. The game never implements a pass callback and never learns
what a shadow pass wants.

**THIS ENTRY USED TO SAY "not before parity — doing both at once would leave
neither finishable". THAT IS NOW WRONG, and the reversal is deliberate.**

It was written when most of the frame was unconverted. Since then the UI, the
probes and the text have landed, and what REMAINS to convert — decals, ribbons,
textures, props, the overlays — would otherwise be written against
`IGeometrySource` and then moved again. Switching first means each of them is
written once. The "second migration" cost is real only if you switch AFTER the
remaining work rather than before it.

It also unblocks §4.3's leftover for free. Probe re-placement is stuck on "the
rhi renderer builds its static world once and never rebuilds it, so there is
nothing to hook to". A scene the game adds to and removes from IS that signal.

---

## THE DESIGN

### The renderable

```cpp
struct RenderableDesc {
    rhi::MeshHandle mesh;
    MaterialId      material;      /* into DeviceMaterials */
    Mat4            transform;
    Vec4            tint;
    Aabb            localBounds;   /* the engine transforms it on add/update */
    bool            castsShadow = true;
    std::uint32_t   filterKey = ~0u;   /* GAME-DEFINED — see below */
};
```

Configured with `withX` chaining per CLAUDE.md — it has more than three optional
knobs and is filled at a call site, which is exactly the case that rule names.

`RenderScene` hands back a `RenderableId` and takes `remove`, `setTransform`,
`setVisible`, `setFilterKey`. Nothing else. **The game never sees an encoder, a
pass or a pipeline again.**

### The three things the game currently decides, and where each goes

| Today the game branches on | Becomes |
|---|---|
| casters only, for the sun | `castsShadow` on the renderable |
| opaque vs translucent | **the material's blend mode** — already data, already in the `.mat`. The engine reads it and stops asking |
| whole lattice vs the player's cutaway | a filter mask — below |

That is the whole of `GeometryPass`. It disappears, and with it the requirement
that a second project know what a shadow pass wants.

### THE CUTAWAY, WHICH IS THE ONLY HARD PART

The engine must not learn what a storey is. So: **every renderable carries a
32-bit key the game assigns, every VIEW carries a 32-bit mask, and the engine
draws where `(key & mask) != 0`.** One AND per renderable. The engine never
learns what a bit means.

The game spends the bits: one per storey, four for wall facings, the rest spare.
Hiding everything above the iso level is a mask with the low storey bits set;
stripping the facings the camera looks through clears four more. This is the
"the game may EXTEND the vocabulary" clause in §0 actually being built, and it is
the same mechanism as Unreal's visibility flags and Unity's layer masks.

**THE MASK BELONGS TO THE VIEW, NOT THE FRAME**, and that is what makes the
existing bug class impossible rather than merely fixed. The sun's view and a
probe capture's view carry an all-bits mask; only the camera's view carries the
cutaway. Today that correctness lives in game code — `RhiFrameRenderer::submit`
passes `CutawayView::whole()` for `Shadow` and `ProbeOpaque` and the player's cut
otherwise — and getting it wrong made the lighting change when the player
changed floor. After this the game cannot express the wrong thing.

So a **View** is: matrices, a filter mask, and which passes it runs. Three views
exist — camera, sun, probe face — and the pipeline already has all three.

### Culling, and what the current shape is hiding

Frustum-cull world bounds per renderable per view. That needs per-renderable
bounds, which needs the statics **chunked** — one renderable per (chunk,
material) rather than one per material for the whole map. A chunk of 8x8 tiles
per storey is the obvious grain.

`IGeometrySource` let us not think about this, because one giant mesh per bucket
has no useful bounds. Chunking is therefore real added work, and it pays twice:
culling gets something to cull, and a world edit re-uploads one chunk instead of
the whole map — which is the same granularity §4.3's probe re-placement wants.

### Sorting

Opaque sorts by pipeline and material, to cut state changes. **Translucent sorts
back-to-front by view distance**, which fixes the bug this entry already calls
"the limitation most likely to force the issue": two overlapping panes currently
blend in bucket order.

Batching and instancing become POSSIBLE here and are not built yet. The engine
owning the list is the precondition; wanting them is a measurement.

### What this does to the overlays (§4.4)

They stop being a special case. A cover marker is a mesh, a transform and a
material — a renderable like any other, with a filter bit so the cutaway hides
it with the floor it belongs to.

**One shape change is required of the game and it is an improvement.** The
visibility overlay currently emits one draw per standable cell EVERY FRAME. As
renderables that would be thousands of registrations a frame, which is the wrong
use of a retained scene. It becomes one mesh rebuilt when the field CHANGES —
on selection or a move, not per frame — and one renderable. That is strictly
less work than today; the per-frame loop only looked cheap because immediate
mode hid it.

### The order of work, keeping a working frame throughout

Each step leaves the tree building and drawing a correct frame.

1. `RenderScene`, `RenderableDesc`, `View`. Nothing uses it. `ScenePipeline`
   traverses the scene AND still calls `IGeometrySource`.
2. **Statics**, chunked. `RhiStatics` registers instead of drawing, and its
   `submit()` goes empty — so the frame is drawn once, by the scene, from the
   first converted producer onward.
3. **Bodies.** `RhiBodies` registers; transforms updated per frame.
4. **Overlays** (§4.4 dissolves).
5. Delete `IGeometrySource`, `GeometryPass`, and the `submit` halves.

### What is deliberately NOT in this

- **No scene graph.** A flat array of renderables with world transforms. Parents
  and local transforms are an animation and attachment problem, and nothing here
  has one yet.
- **No render graph.** `ScenePipeline` already owns the pass order explicitly and
  it is readable; a graph solves a problem this frame does not have.
- **No instancing or batching**, per above.
- **No LODs, no occlusion culling.** Frustum only.

### 4.13 At parity
Delete `FrameRenderer`, `StaticsMesh`, `UnitRenderer`, `ISceneSource`,
`PassContext`, `MaterialLibrary`. Then `ScenePipeline`'s per-pass clip-control
scoping collapses to one call at device creation, and `RhiStatics`/`RhiBodies`
drop their "duplicated from X for the migration" notes.

---

## 5. Traps already paid for

Each of these looked like a different problem than it was.

**`glClipControl` was never called.** `Mat4.hpp` documents 0..1 clip depth and
says the GL backend will set it. It did not. Nothing is clipped, so the scene
draws and looks right — but every depth *comparison* silently fails. **Shadows
vanish entirely and SSAO reconstructs wrong positions.** It is `ARB_clip_control`,
core in 4.5, and raylib's glad is a 4.3 loader, so the entry point is resolved by
hand. It is applied **per pass**, because raylib shares the context and wants the
opposite convention.

**The sun's up vector.** Was `+Z`, raylib uses `+Y`. Every measurable number
matched — same radius, same world-units-per-texel, same depth range, same fit —
and the shadow maps still disagreed over a third of their texels, because the
**texel grid was rotated** under the same geometry. A shadow edge aliases against
the lattice it is sampled on. Matching the numbers is not matching the basis they
are expressed in. Found by dumping both maps to BMP and diffing; six rounds of
reasoning did not find it.

**`sampler2DShadow` on a D32F map.** GL does not require linear filtering of a
32-bit float depth format. Where the driver falls back to nearest, every tap is
binary, so twelve taps return one of thirteen values — blocky plateaus along
shadow edges dissolving into speckle. Filter by hand (`shadowTap`), which is what
the raylib path was forced into and is correct on every format.

**The SSAO blur was missing.** The occlusion pass rotates its kernel per pixel to
turn banding into noise *on purpose*; the blur is the other half of that bargain.
Without it the noise is simply left on screen — a grainy four-pixel field over
every surface that reads as "the image is fuzzy and the edges are jagged" and
survives every shadow fix, because none of them are what is wrong.

**Glass in the depth prepass.** The lit pass tests `Equal` against the prepass, so
a pane's depth there means the **wall behind it is never shaded**. Blending cannot
put back geometry that was never drawn.

**`backbufferSize()` read the current GL viewport.** That is whatever the *last
pass* set — so any screen-targeting pass inherited the supersampled scene target's
viewport, twice the screen. The tone map survived by accident (it addresses by
`gl_FragCoord` and divides by the backbuffer size, so it cancels); the UI has real
vertex positions and came out at double scale. The device must be **told** its
size.

**Tuning invented rather than borrowed.** The SSAO ran at radius 0.9, bias 0.025
and strength 0.9 against the raylib path's 0.45, 0.008 and 1.0 — because the
numbers were typed into the pass instead of read from `AmbientOcclusion::Tuning`.
The bias is the one that shows: three times too large rejects the close occluders
that produce contact darkening, which is most of what the effect is for. Same
class of mistake as `authorMaterials()` hardcoding the ladder. **If a value
already exists on the raylib side, borrow it — do not retype it.**

**A reflection on the wrong side is not a mirrored cubemap.** Reported as
"there's a big box behind the camera but it's on the wrong side", looking at a
window from outside — and the obvious suspect is the six-face orientation table,
where four of the up vectors point *down* and the documented failure is a
mirrored capture. It was not that: all six faces check out against GL's `sc`/`tc`
mapping, and the rotations have determinant +1, so nothing is flipped and the
winding is not reversed either.

The cubemap was right and the **lookup into it was aimed wrong**. Parallax
correction re-aims the reflection ray from the capture point, which is valid only
while the capture point and the shading point share an environment. Every
interior got a room-sized box when the single probe was split up; the *outdoor*
volume kept the board, on the reasoning that it is the one volume genuinely that
big. Size was never the question. A window ten tiles from the map centre sends
its ray twenty tiles to the board's edge and then re-aims it from that centre —
pointing it at the opposite half of the world. The outdoor volume now carries
`parallax = false` and is sampled as an environment at infinity, which is what a
board-sized capture from one point actually is.

Two lessons worth more than the fix. **`ReflectionProbeSet.hpp` already contained
the sentence that predicts this** — "uncorrected, the same leak is a distant blur
nobody would question" — and it read as though it covered the outdoor case, so
nobody re-read it against the one volume that was exempted from the change it was
written to justify. And the symptom pointed at the wrong subsystem: a wrong-sided
reflection *looks* like an orientation bug, so the search starts at the face
table and the face table is fine.

**`probeDirection` had no guard for a fragment outside its box**, found while
reading the above. The AABB intersection there is the box's *exit* and is only
defined from inside; outside, the distance comes out negative and the hit walks
**backwards** along the reflection ray into an unrelated part of the cubemap. It
is reachable — a pane sits astride a room boundary, so half its thickness is
outside the box its inner face selects. Clamped into the box now, which moves the
origin by four hundredths of a tile and makes the result defined.

**The framebuffer cache key packed the layer into three bits.** `id * 8 + layer`
is injective only while a layer number stays under eight — true when the only
array in the engine was a six-face cubemap, and false the moment the probe array
arrived with ninety-six slices. Past that, texture 5 slice 24 and texture 8 layer
0 hash identically, and the consequence is a pass rendering into somebody else's
target: a probe face landing in the scene colour buffer, or the reverse. The
depth term happened to separate every case that existed, which is exactly the
kind of accident that stops holding when a pass is added. Each field now gets its
own multiply.

**The camera's lit pipeline cannot capture a probe face.** It tests `Equal`
against the depth prepass and writes no depth — correct, and an optimisation
available only because the prepass drew exactly that geometry. A cube face has no
prepass, so every fragment tests `Equal` against a buffer cleared to 1.0 and is
discarded. Nothing is drawn, nothing errors, and **six black faces are also what
a rejected layer attach produces** — so the cheap-looking reuse buys an ambiguity
between two completely different failures. The capture has its own `Less` +
depth-write pair.

**Screen-space passes leak into a 128-pixel cube face.** The lit shaders read the
occlusion plane by `gl_FragCoord`, which means nothing inside a capture. A 1×1
white stands in — but `texelFetch` out of range is *undefined*, not zero, so the
fetch has to be clamped to the bound texture's size for the stand-in to work at
all. Leaving the slot unbound instead reads black on most drivers, and black
occlusion is a cubemap in which every room is a cave.

**A MIP CHAIN OVER A PARTIALLY-WRITTEN TEXTURE — and this one is a live bug in
the RAYLIB painter, found by porting it.**

The obvious way to blur a backdrop is to copy the rectangle behind the panel,
mip it, and read a level. `UiPainter::executeBackdropBlur` does exactly that, the
device version was written to match, and it is wrong.

The capture texture only ever GROWS, so a small panel copies into the corner of a
texture sized by the largest one. `glGenerateMipmap` then averages the WHOLE
texture, including every texel the copy did not touch — which still holds
whatever the last, bigger capture left there. At a 24-pixel radius one texel of
the level being read covers a 24-pixel square of source, so the stale content
bleeds in from the edge and, a few levels up, dominates.

**Measured, on this project's own gallery screen: a frosted panel over an OPAQUE
BLACK scrim comes out (255, 255, 255) on the raylib path.** A blur of black is
black. With the scrim made translucent so there is something to actually blur,
the whole screen collapses to a flat grey instead of a blurred scene.

The device path captures the WHOLE SCREEN instead, so the chain is built over a
fully valid image and there are no unwritten texels to average in. It reads
(15, 15, 15) on that panel, which is the 6% white fill over black — correct.
Bleeding across a panel's edge then becomes right rather than a bug: real frosted
glass gathers light from just outside its frame too.

The cost is a full-screen copy and mip chain per frosted panel rather than a
region-sized one. **That is the price of the correct answer**, and if it ever
matters the fix is not to go back — it is a chain that can be built over a
sub-rectangle, which means generating the levels by hand.

**`UiPainter` STILL HAS THE BUG, AND IS NOT GOING TO BE FIXED.** Decided
deliberately, so nobody rediscovers the white panel and spends a day on it:

- Nothing in the game asks for a backdrop blur. The only callers are the widget
  gallery and `TipPanel`'s media area when `blurMediaBackground` is set, which
  only the gallery sets. It has never been on screen during play.
- The gallery runs on both renderers now, and on the rhi path it is correct.
- §4.13 deletes `UiPainter`.

Which makes the fix work on a file that is going, to correct a defect that only
appears on a screen that has a working version beside it. The bug is documented
above; that is what it earns.

**Push constants do not survive a pipeline switch.** They are emulated on GL as
a uniform at location 0 of the CURRENT PROGRAM (`glUniform4fv`), so binding a
second pipeline inside a pass silently abandons them. Everything before the UI
had one pipeline per pass and never met this. The text pipeline is the second
one in a pass, and the constant it needs is the SURFACE SIZE — so the failure is
a vertex stage dividing by zero, every glyph at infinity, nothing drawn. Which is
identical on screen to a font that failed to load, and the two live in different
files. Push after every `bindPipeline`, not once per pass.

**A cache keyed by a pointer into another cache.** The glyph textures are keyed
by `const GlyphAtlas*`, which is legitimate — `UiFontSet` holds atlases in a
`std::map`, and a map keeps its elements put — and is exactly as legitimate as
the lifetime of the map's nodes. An unload frees them and a reload can allocate
new ones at the same addresses, at which point a label draws in whatever
typeface previously occupied that memory. `UiFontSet::generation()` is the guard,
and the general shape is worth remembering: **borrowing an address as a cache key
means borrowing the owner's invalidation too, and the owner has to be asked for
it explicitly** — because nothing about the pointer says when it stopped meaning
what it meant.

**Teardown order.** `Application::run` reset the platform at five different exits
while the device renderer still held meshes and buffers. Access violation in a
destructor after `main` printed its exit status. One `releasePlatform()` now.

**Input on the device path.** `SetTargetFPS` only applies inside raylib's
`EndDrawing`, which the device path never calls — so the loop ran unthrottled and
every per-frame input delta was starved. Separately, unhiding a window does not
focus it, so keys went nowhere until you clicked. Both live in `RaylibSurface`.

---

## 6. Conventions

- `assets/shaders/CONVENTIONS.md` is the shader dialect and the binding table
  (0 frame / 1 pass / 2 material / 3 object)
- Shared shader code lives in `rhi/*.glsl` includes — `scene_block`,
  `material_block`, `sky`, `shadow`, `object`. A block declared twice is a link
  error at best and silently wrong offsets at worst
- `common/brdf.glsl` and `common/colour.glsl` are shared with the **raylib**
  renderer because they are pure functions. `common/environment.glsl` and
  `common/shadow.glsl` are not, because they declare loose `uniform` globals
- Interfaces take an `I` prefix
- Materials are authored in `.mat` files. A new surface parameter goes in the
  schema and the material block, never into a setter called from a renderer
