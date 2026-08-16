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
| Custom passes | `IScenePass` at a named `ScenePassPoint` | **The hatch.** Read and write the frame's named resources; may not change which exist or the order |
| Geometry | `RenderScene` + `RenderableDesc` + `View` | **Engine.** The game REGISTERS what exists; the engine owns the list, culls it, sorts it and draws it. `IGeometrySource` is deleted |
| Materials | `.mat` files → `DeviceMaterials`, owned by `RenderAssets` | Data, never C++. **Device-scoped**, shared by every scene and every pipeline |
| Probes | `DeviceProbeSet` on the **scene** + `ProbePlacement` | **Engine** owns the array and the schedule; **game** says where the rooms are. **World-scoped**: a probe set describes a world, not a viewpoint |
| UI | `ui::IUiPainter` | `DeviceUiPainter` vs the raylib `UiPainter`. **Every command kind converted** |
| Text | `ui::GlyphAtlas` | **Engine.** The bake is neutral; each painter does its own upload |

**The rule:** if converting a pass grows `RhiFrameRenderer`, something
engine-shaped has leaked into the game. It has stayed roughly constant through
every pass so far; that is the property worth watching.

## 3. Done

**Platform + device**
- Six platform interfaces, folders by platform then library (`pc/raylib`,
  `rhi/pc/opengl`, `ps5/README.md` as the port contract)
- `OpenGlRenderDevice` with a conformance self-test (`--device-selftest`) that
  now covers the stencil as well
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

1. **Re-placement after destruction** — **half done, and the half that landed is
   the hook rather than the feature.** `RhiFrameRenderer::worldChanged()` now
   exists and re-floods: it arrived with §4.5's reset-world button, which is
   exactly the "whatever eventually tells that renderer the statics changed"
   this entry was waiting for, so the re-flood rides on it instead of being a
   second answer to "has the world changed".

   **WHAT IS STILL MISSING IS THE CALLER, NOT THE MECHANISM.** A grenade
   demolishing a wall does not call it — only a full reset does. The raylib
   path's `probesDirty_` is set wherever the world is edited, and the device
   path has no equivalent edit notification yet. That is one call site away and
   it belongs with whatever eventually tells this renderer a chunk changed;
   §4.12's chunking is what makes that affordable when it comes.
2. ~~**A dev-panel preview of a layer.**~~ **Done** — a six-face strip, one
   probe at a time, cycled by the panel's button. `rhi/dev/preview_cube.fs.glsl`
   samples the array by direction; alpha zero comes out magenta, because the
   capture clears to transparent black and "open sky" and "a black wall" have to
   be distinguishable or the preview answers nothing. See §4.5.
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

### 4.4 ~~Game overlays~~ — DONE, as renderables

Both halves landed. `DebugDraw` became a pass on `ScenePipeline` (below), and
`OverlayRenderer`'s four world-space layers became `game/render/rhi/RhiOverlays`
— the visibility field, the cover shields, the hover plate and the path preview,
each a mesh registered in the scene.

**It took the RIGHT version of the two this entry offered**, and the cheap one
would have been a mistake in a way worth recording: letting the overlays become
debug-draw primitives would have been a second way for the game to push
triangles at the device, invented in the same month the first one was removed.

**THE ONE SHAPE CHANGE THE RETAINED SCENE DEMANDED** is the one this entry
predicted. The visibility overlay emitted one draw per standable cell EVERY
FRAME; as renderables that is thousands of registrations a frame, which is the
wrong use of a retained scene. It is now one mesh PER STOREY, rebuilt when the
field changes — and per storey rather than per world so that raising the iso
level costs no rebuild at all, only a filter bit.

**How "changed" is decided is worth reading before copying it.** By hashing what
the layer is built from, not by keying on the things believed to change it. A
key made of "the selected unit, its position, the iso level" is a claim about
every cause, and it goes stale the first time a new one appears: a grenade
demolishes a wall, the field genuinely changes, none of the keyed values move,
and the overlay keeps showing line of sight through a wall that is no longer
there — read as a LOS bug in the simulation, which is the most expensive
possible place to look. The hash is a few thousand bytes once a frame against a
rebuild it protects that costs orders of magnitude more.

The original entry follows.

#### The original entry

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

### 4.5 Dev panel — WIRED. What is left reads zero because §4.6 has not landed

imgui on the rhi path. The widget gallery, which was going to be the first task
here, landed with §4.1 instead — it is what verifies text, so it had to.

**`DeviceImGuiRenderer` (game/render/dev/) turns `ImDrawData` into device
draws** and the panel is on screen: 48,556 pixels of toolbar in ImGui's own
theme colours, verified with `--renderer rhi --dev-view --shot`.

**IT IS THE GAME'S, NOT THE ENGINE'S**, because cromwell may not link imgui —
the CMake note where the imgui target is declared states why, and a backend is
engine-shaped work that still has to live on the game's side of that line.

**rlImGui's INPUT HALF IS STILL USED and its RENDER half must not be.**
`rlImGuiEnd()` is exactly `ImGui::Render()` plus a draw through rlgl, with no
seam; `DevView::setDeferredPresent` splits them so the device path takes the
first half. The window and the mouse are still raylib's on this path, so
rewriting the input half against `IInput` today would be a second answer to a
question that already has one — it moves when the platform does.

**ONE PANEL, BORROWED.** There is a single ImGui context and it owns the input
state, the open tabs and every slider position, so `RhiFrameRenderer` points at
`FrameRenderer::devView()` rather than owning a second one that would forget
itself on a renderer switch. Same bargain `GameUi::setPainter` makes.

**IMGUI 1.92 MANAGES TEXTURES AND A BACKEND MUST TOO** — `RendererHasTextures`
plus `WantCreate`/`WantUpdates`/`WantDestroy` on `ImDrawData::Textures`. A
backend written from memory against the old "upload one atlas at startup" model
is correct on the first frame and garbage at the first glyph that was not in the
atlas. The pinned rlImGui was the working reference.

**WHAT IS STILL PARTIAL, and it is the model rather than the panel.** The device
path fills the DevModel it can — selection, hover, iso level, sun angles, probe
count, debug view, and now every texture preview and the avatar — and leaves at
their defaults everything describing a system it has not converted: the ribbon
loop and edge counts, the baked sun, the decal tool. **Those panels show zeros,
which is the truthful answer for a subsystem that is not running.** Inventing
numbers would make a diagnostic lie about what the device renderer is doing,
which is the one thing it may never do. They fill in as §4.6 lands.

#### THE PANEL HAS TWO MECHANISMS AND THEY FAIL DIFFERENTLY

This is the thing to understand before wiring anything, because it is why the
panel looked half-broken in a way that made no sense from the outside.

- **A CHECKBOX WRITES A FLAG DIRECTLY** — `layerPair("ssao", &layers.features.
  ambientOcclusion)` edits the camera's layers in place. It works the moment the
  pipeline READS that flag.
- **A BUTTON RAISES A REQUEST** — `if (toggled("SSAO (O)", ...)) requests.
  toggleOcclusion = true`, and something else has to act on it. `Application`
  takes those from the raylib renderer through `takeDevRequests()` and folds
  them into the frame's input.

So on the device path every button was inert while the checkbox beside it
worked, and the two are a foot apart in the same panel. **Reported as "I click
SSAO and it doesn't turn off the SSAO", which is indistinguishable from a
rendering bug and is neither.**

#### WHAT IS WIRED

- **Six of the eight FEATURE checkboxes** — `sky`, `shadows`, `reflections`,
  `ambientOcclusion`, `debugDraw` and `toneMap`. Only `decals` and
  `customDepth` are unread, and honestly so: neither pass exists on this path,
  so there is nothing for them to switch off.
- **THE GAME'S DRAW LAYERS** — statics, units, overlays. As filter bits, which
  is the mechanism the render scene already has: the renderable says what it IS
  and the view says what it HIDES. `RenderFilter.hpp` spends six bits on them
  and the storey budget drops from 27 to 21 to pay for it. **They are hidden in
  every DERIVED view as well**, so switching units off takes their shadows with
  them — measured, 1,429 pixels of floor brighten. See below.
- **THE RENDERING PANEL'S PER-TERM SWITCHES** — `RenderEffects`, one bool per
  contribution to a pixel: direct sun, ambient diffuse, ambient specular,
  transmission. Through `SceneFrame::effectSuppress` into the block both surface
  shaders read. `bakedOcclusion` has nothing to switch off here, because there
  is no mrao map on this path yet.
- The sun's sliders (the one sun in the process, borrowed), the exposure, and
  the SSAO radius/bias/strength — §4.10's first debt item, closed.
- The Steam panel's text: running, reason, persona, id, avatar state and URL —
  **and the avatar image**, uploaded through the device.
- The reported state now READS BACK from what the pipeline consults, rather than
  being hardcoded true. A checkbox that displays ON regardless is worse than one
  that does nothing: it asserts something false about the picture.
- **EVERY BUTTON.** `RhiFrameRenderer::takeDevRequests()` exists and
  `Application` drains it; the requests that belong to the game arrive at the
  handlers they always had. Below.
- **The texture inspector**, over this renderer's own intermediates — including
  the probe strip, which closes §4.3's second leftover.

#### HOW THE REQUESTS ARE ROUTED, AND WHY IT IS NOT A BRANCH

The question this raised, since it had to be settled either way: should
`Application` route requests from *whichever renderer is drawing*, or should
both renderers expose the same thing and `Application` stop caring? **The
second, and it cost no abstraction to get.**

```cpp
DevRequests requests = renderer_.takeDevRequests();
if (rhiRenderer_) requests.mergeFrom(rhiRenderer_->takeDevRequests());
```

Both are drained, unconditionally, with the same signature and the same
semantics. **Nothing here asks which renderer is live**, and it is safe because
there is ONE dev panel in the process: only the renderer that is drawing is ever
handed a request buffer to fill, so the other hands back a default-constructed
struct that folds in as a no-op **by construction rather than by a check
somebody has to keep true**.

WHY NOT AN `IFrameRenderer` THE TWO IMPLEMENT. Because the interface would be
shaped by the class that is leaving. §4.13 deletes `FrameRenderer`, and
`Application` reaches into it for a few dozen things the device path has no
answer for — `ao()`, `decals()`, `probes()`, `rebakeAll`, `splitLayout`,
`clearFlashes`. A base class extracted today would either carry all of that as
pure virtuals the survivor has to keep implementing, or be a narrow one-method
interface with a vtable for a single accessor. **The property that was actually
wanted — Application does not restate which renderer is live — is delivered by
the merge**, and when `FrameRenderer` goes the merge collapses to one call with
nothing to unpick.

The rule underneath it: **which renderer draws is a fact stated once, at the
render call. Every second copy of that fact is a copy that can disagree** — and
the disagreement here is exactly the failure this section exists to describe.

#### WHAT THE ROUTING TURNED ON, AND THE ONE THING IT BROKE ON THE WAY

The game's requests — reset world, cycle ring, toggle LOS / cover / grenade /
cutaway / flat view, the bake, the camera requests, decal placement — needed the
requests to ARRIVE and not new code, exactly as this entry predicted. They are
**deliberately still not answered inside the renderer**; the two that are
(`toggleOcclusion`, and now `cyclePreviewProbe`) are state this renderer owns —
a layer flag on the camera it holds, a slot in the panel it hosts.

**RESET WORLD WAS THE EXCEPTION AND IT WOULD HAVE SHIPPED HALF-WORKING.**
`state.reset()` generates a new building, and this renderer builds its statics on
the first frame and never again — so the button would have reset the simulation
and left the OLD building on screen with the new units walking through it. A
request that half works is worse than one that never arrives, because the half
that works makes the other half look like a rendering bug. `worldChanged()` is
the fix, and **it closes §4.3's first leftover for free**: that entry says probe
re-placement "belongs with whatever eventually tells that renderer the statics
changed, and a second answer to 'has the world changed' invented before then is
two answers that drift". This is that thing; the re-flood rides on it.

#### THE TWO KINDS OF HIDING, AND WHY `View` NOW HAS TWO WORDS

`withHiddenFlags` was the only one, and `derived` deliberately drops it — that
is what stops a player's cutaway reaching the sun's pass, which is the episode
`CutawayView.hpp` documents. Correct, and it is the wrong behaviour for a
category switch:

| | scope | why |
|---|---|---|
| **cutaway** — `withHiddenFlags` | this view only | what a player can see past is a fact about where they are STANDING. In the sun's pass it makes the lighting change when the player changes floor |
| **draw layers** — `withAlwaysHiddenFlags` | this view and every derived one | a category switched off has to be GONE. `ViewLayers.hpp` already required this: "a unit hidden from the camera but still laying a shadow across the floor would be a worse debugging tool than no switch at all" |

So `derived` carries the second and drops the first, and `hiddenFlags()` returns
the union so nothing downstream of collection knows there were two. **The engine
still learns nothing about what a bit MEANS** — both words are the game's
vocabulary and what differs is scope, which is a property of views and therefore
the engine's to know.

**MEASURED, one capture per switch against the same frame with it on:**

| switch | pixels changed |
|---|---|
| statics | 147,342 |
| ambient diffuse | 149,129 |
| overlays | 125,507 |
| direct sun | 118,039 |
| ambient specular | 24,196 |
| **units** | **2,685 — of which 1,429 are floor getting BRIGHTER**, which is the shadows going with them |
| tone map | 1,023,996 (the whole frame, as a curve removal should be) |
| transmission | **0, and correctly so** — `window.mat` authors `transmissionAmount 0.0`, so the term is zero before the switch touches it. Wired through the same `effectOn` as the four above; there is nothing on this board for it to remove |
| baked AO | 0 — no mrao map on this path, so again no term. Not pretending otherwise |

#### THE TEXTURE INSPECTOR, AND WHY IT IS A BLIT

`DevTextureView` no longer carries a raylib `Texture2D`. It carries an opaque id
and a size, because **the two ImGui backends have different id spaces** —
rlImGui puts a GL name in `ImTextureID`, `DeviceImGuiRenderer` puts an RHI
handle — and passing one through the other samples whatever resource happens to
share that number. **A wrong picture rather than an error**, in the one panel
whose whole job is deciding whether a buffer is right.

So each renderer fills its own, and the device path's come from
`cromwell/diag/DeviceTexturePreviews`: shadow map, transmission plane, occlusion
plane, prepass depth, encoded normals, the same plane's roughness, HDR scene
colour, and the probe strip. **Every one of them needs an interpretation rather
than a copy** — a D32F depth buffer is a red wash whose interesting range is a
sliver at the top, RGBA16F radiance clips to white, the occlusion plane is one
channel and the G-buffer's fourth channel is roughness. The header enumerates
each and says what would be seen without it.

**ENGINE-SIDE, unlike the ImGui backend.** A backend has to be the game's
because cromwell may not link a UI toolkit; turning a D32F shadow map into a
picture names no toolkit at all and is work a second project would write again
unchanged, which is §0's test for where a seam belongs.

**IT ONLY RUNS WHILE THE PANEL IS OPEN**, and the check is in the renderer
because nothing inside the previews knows whether anybody is looking. The
shaders are not even loaded until the first F1 — a `--shot` run, a self-test and
every shipped frame pay nothing.

#### WHAT ONLY §4.6 CAN FILL, AND IT STILL READS ZERO

The ribbon loop and edge counts, the decal tool, and `RenderEffects`. The panel
writes them and nothing reads them, because the systems behind them are not
converted. **They read zero rather than plausible numbers on purpose**; a
diagnostic that invents values lies about what the renderer is doing, which is
the one thing it may never do.

### 4.6 Decals (DBuffer) + ribbons/glow — DECALS AND GLOW DONE; RIBBONS DEFERRED

**Emission is a material property, bloom is an engine pass, and the DBuffer
runs.** Only the ribbons are left, and deliberately — see the end of this
entry.

#### THE DECALS, AS A DBUFFER

Three planes between the prepass that feeds them and the lit pass that reads
them. One draw per decal: a projector BOX with back faces only, no depth test,
and separate blend factors so any number of overlapping decals compose in one
equation. The technique, the wrap, the fold, the angle fade and the gradient
clamp are the raylib pass's, ported with their notes rather than re-derived.

**WHAT THE CONVERSION ACTUALLY CHANGED** is a short list, which is the point:
the uniforms became two std140 blocks at the pass and object frequencies, the
maps became explicit bindings, `matModel` became a block member rather than
something `DrawMesh` set, and the depth unprojection **dropped its -1..1 remap**
because this renderer runs at 0..1 clip depth. That last one would have put
every recovered world position at the wrong distance — decals floating off their
surfaces, which reads as a projection bug rather than a depth convention.

**ONE UPLOAD PER FRAME, A BIND PER DRAW.** Every decal's object block goes up in
one buffer and each draw binds its own slice at an offset — `bindUniformBuffer`
takes one. Rewriting a buffer between draws inside a pass is a pipeline stall on
every backend that means it, which `DeviceMaterials` already refuses for the
same reason. The slices are padded to **256 bytes** because a uniform binding
offset must be aligned; packing them at their natural 192 is rejected on a
strict driver and silently rounded on a lax one, which hands every decal its
neighbour's transform.

**AND THE PROJECTOR BOX HAD TO BE WOUND BY HAND, WHICH COST A BUG WORTH
RECORDING.** The raylib pass drew raylib's own cube; this one writes thirty-six
positions out in `ScenePipeline`, and **four of the six faces went in wound the
wrong way round**. The symptom was not an inside-out box — nothing ever SHADES
these faces, they exist only to make the rasteriser visit the pixels the box
covers. With `cull = Front` the correctly wound faces contributed their FAR side
and the reversed ones their NEAR side, and while either set alone covers the
whole silhouette of a convex solid, **a mixture covers neither**: any pixel whose
nearest face was reversed and whose farthest was not is visited by nothing, so
the decal is simply absent there. It reads as the decal clipping or tearing, in
wedges bounded by projected cube edges and by each face's split diagonal, moving
with the camera because which face is nearest is a property of the view — and it
sends you into the fragment shader, which is entirely innocent. **The check is
the cross product, not the picture**: `(v1-v0) x (v2-v0)` must point out of the
cube for all twelve triangles. Any hand-written bounding volume on this path
wants the same check.

**WHERE THINGS LIVE, by the three lifetimes.** The projectors are a
`DeviceDecalSet` on the **scene** — a scorch mark is on a floor, not in a
viewpoint, which is the identical argument the probes make. The maps are
**RenderAssets'**, cached by name for the life of the device. The planes and the
pass are the **pipeline's**.

**AND IT COST THE FIRST TEXTURES ON THIS PATH.** `RenderAssets` grew a texture
cache — name in, device texture out — because a decal's ALPHA is its shape and
it is the one map that cannot fall back to a constant. That header said "not a
texture cache" and now is one; it grew because a caller arrived, not in
anticipation of §4.7, and the note says so.

**ONE DELIBERATE DIVERGENCE FROM THE RAYLIB DBUFFER.** Its albedo plane is
RGBA8 holding sRGB-ENCODED values, because eight linear bits crush the darks —
and the trade it accepts is that decal-over-decal blending happens in the wrong
space. Here the plane is **RGBA16F** and everything stays linear end to end, so
overlapping decals are correct rather than merely acceptable. The other two
planes stay RGBA8; a normal and three 0..1 scalars have no range to lose.

**THE PLANES ARE HALF THE SCENE TARGET**, which is the window's own resolution
given the 2x supersample. The lit pass upsamples them bilinearly and lands decal
detail where it is seen; at full scene size the three would be 48 MB carrying a
signal nothing can resolve.

**THE TRANSPARENT PASS DELIBERATELY DOES NOT READ THEM**, and it looks like an
omission. The DBuffer describes the surface the PREPASS recorded — an opaque one
— and a pane of glass in front of it is a different surface at the same pixel. A
transparent shader reading the planes would paint the wall's decal onto the
window and then composite the wall behind it with that decal already on it: the
same mark twice, one of them in mid air.

**MEASURED**, `--renderer rhi --decals` against the same frame without them:
3,095 pixels changed, bounded to (523, 296)–(738, 561). The raylib path's
footprint over the same camera is (521, 294)–(751, 554) — **within three pixels
on three edges**. Its total is larger because it also draws the three
PROCEDURAL demo materials, which a mirror keyed by name cannot follow; see
below.

**WHAT THE DEVICE PATH SKIPS AND WHY.** `DecalSet::registerTextures` exists for
generated art and `DecalDemo` uses it for three of its four marks. Those have no
file, so `RhiDecals` cannot load them and logs one warning each naming both
possible causes. The authored material — `example`, the hero and the dev tool's
default — loads and draws. This goes when the two decal sets become one at
parity.

**AND THE PANEL'S DECAL TOOL IS LIVE**: `available` is true, the material list
is the game's, and `placedCount` is the DEVICE's number rather than the game's —
so a decal whose material was skipped does not get counted as placed over a
board showing none. The three DBuffer planes are in the texture inspector, which
is what turned this on during the build: ink in the plane and nothing on screen
is the READ, nothing in the plane is the PASS. It was the READ.

**EMISSION.** `.mat` gains `emissiveColour` and `emissiveStrength`, through
`PbrMaterial` into `MaterialBlockData` as a premultiplied radiance, read by the
lit and transparent shaders. Added after everything and multiplied by nothing —
not the shadow, not the occlusion plane, not the ambient intensity, and **not
the albedo**, which is the mistake available here: a material's emission is the
colour it emits, so multiplying by the surface colour would make a red strip
light on a blue wall come out black. The decal path multiplies by albedo on
purpose, because its emissive channel is a mask over a colour it does not own.

**BLOOM.** `ScenePipeline::drawBloom` — prefilter with a soft knee and a Karis
average into a half-resolution level 0, thirteen-tap downsample to the bottom,
nine-tap tent back up ADDING, and one additive composite into the scene. One
texture with six mips rather than six textures, because the RHI already
attaches by `mip` and clamps a sampler's LOD range — the same two facilities the
probe prefilter needed for the same hazard.

**INTO THE HDR TARGET, BEFORE THE RESOLVE**, which is the decision
`study/plans/bloom_emissive.md` argues for and the one `GlowPass` gets wrong.
Compositing after the tone map means display colour added to display colour: it
cannot be exposed with the frame, it is wrong under the no-tonemap debug view,
and it is tied to the main view's resolution. It is also **before the
BeforeToneMap hatch**, so a game's custom pass there sees the finished HDR scene
including its glow.

**KNOBS**: `BloomTuning` — threshold, knee, intensity, spread — live on the
renderer and edited in the dev panel's post tab, the same arrangement
`AmbientOcclusion::Tuning` has and for the same reason. Plus a `bloom` switch on
`ViewLayers::features`, because cromwell owns the pass. The panel says the
threshold is linear radiance, because the first instinct is to drag it to 0.5
expecting "half brightness" and get a frame-wide haze.

**MEASURED**, with a temporary emissive material on the portal pads:

| | pixels changed |
|---|---|
| emissive term alone (bloom off) | 238 — the pads themselves |
| bloom on top of it | 4,495, and the bounding box reaches **74 pixels above** the emissive one — the glow spreading past its geometry, which is the whole point |
| bloom over a scene with nothing emissive | **11 pixels, max 4/255** — sun highlights just crossing 1.1, not a haze |

That last row is the one worth keeping: a bloom whose threshold is wrong shows
up as a milky image everywhere rather than as a missing glow, and it is the
failure that gets "fixed" by lowering the exposure — dimming the whole frame to
hide one term.

**NOTHING IN THE SHIPPED BOARD EMITS**, so the feature is invisible until
something is authored. That is deliberate: adding emission to a surface changes
how the game looks, which is a design decision rather than a migration one. Two
lines in a `.mat` turn it on; `assets/materials/README.md` has them.

#### CUSTOM DEPTH / STENCIL, AND THE FIRST THING THAT EVER READ IT

Not part of §4.6's plan, and landed with it because the question "does the RHI
support custom stencil" turned out to have a more interesting answer than
expected.

**THE RAYLIB FEATURE IS NOT A HARDWARE STENCIL.** `CustomDepthStencil.hpp` says
so: rlgl attaches depth with no stencil bits and wraps no stencil functions, so
the value lives in a colour channel — id in `.r`, coverage in `.a`, depth in its
own attachment. That means porting it needed **no new RHI capability at all**;
it is MRT plus a depth texture plus per-draw data, all of which the DBuffer had
just exercised. The gap was never capability, only conversion.

**HERE THE COLOUR CHANNEL IS A CHOICE RATHER THAN A CONSTRAINT**, and it is
still the right one. A hardware stencil gives eight bits of value and nothing
else; this gives the value AND coverage, which is what makes an id of ZERO
distinguishable from "nothing drawn here" — a distinction a real stencil could
only make by reserving a value and losing it.

**WHICH OBJECTS ARE IN IT IS ASKED OF THE RENDERABLE**, as
`RenderableDesc::withCustomStencil` — Unreal's `bRenderCustomDepth` and
`CustomDepthStencilValue` in one field, and the same "asked of the renderable,
not of the pass" rule `castsShadow` and `visibleInReflections` already follow.
`ViewKind::CustomDepth` is the fourth kind and collects on exactly that. **The
engine never learns what a value means**, like the filter bits.

**AN INTEGER PER OBJECT, NOT A CATEGORY PER CHANNEL** — the raylib header's
argument, kept: four channels of fixed categories cannot tell two soldiers
apart and wants a new channel per view.

**AND IT FINALLY HAS A CONSUMER.** `outline.fs.glsl` is the silhouette that
header has described since it was written — "one full-screen shader: read the
value, ask whether this depth is behind the G-buffer's, draw accordingly". Until
now the buffer's only consumers on EITHER renderer were two rows in the texture
inspector.

**IT COMPOSITES AFTER THE TONE MAP, WHICH IS THE OPPOSITE OF BLOOM'S DECISION
AND ALSO CORRECT.** Bloom is LIGHT and must be exposed and tone-mapped with the
frame. An outline is INTERFACE — the same category as the HUD — and a designer's
colour for it should survive the exposure curve rather than be graded by it. The
test is not "before or after the tone map", it is **"is this radiance, or is
this a widget"**, and §4.6's criticism of `GlowPass` does not transfer because
GlowPass is a glow.

**THE EDGE IS DETECTED ON THE ID, NOT ON DEPTH.** A depth edge finds every
silhouette in the scene, needs a threshold that is wrong at some distance, and
cannot tell apart two soldiers standing against one another — the ordinary case
in a tactics game. An id edge is exact and needs no threshold.

**MEASURED**: `--select 0/1/2` produces outlines at three distinct bounding
boxes — (649,412)-(663,435), (650,512)-(711,568) and (659,306)-(672,326) — so
the id reaches the buffer per unit and the shader reads the right one. The
larger count on unit 1 is a nearer soldier.

**WHAT HAS NOT BEEN SEEN YET**: the occluded half. The x-ray colour is wired and
the depth comparison is written, but no scripted camera puts the selected unit
behind a wall, so it has two pixels of evidence rather than a picture. Worth a
look from a camera that does.

#### What is still open here

- **The ribbons** — deliberately NOT ported, and the original entry below is
  still the plan: `GlowPass` and `kRingGlow` are a stopgap to DELETE, and the
  ribbons become renderables with an emissive material drawn in the ordinary
  passes. **Bloom now exists for them to be picked up by**, which was the
  precondition. What is left is a look decision that needs eyes rather than an
  argument — §3 of the plan, below.

The original entry follows.

### 4.6 (original) Decals (DBuffer) + ribbons/glow

**THE DESIGN FOR THE GLOW HALF IS ALREADY WRITTEN AND IS NOT IN THIS FILE.**
`study/plans/bloom_emissive.md` (2026-08-11) is a design note rather than
research, and it decides the shape. This entry was a bare heading, which is how
it came to be re-argued from scratch — the cross-reference is the fix.

**Its conclusion, in one line: the ribbon glow is not a pass to convert, it is a
stopgap to DELETE.** `GlowPass` re-renders the ribbon geometry emissive at half
window resolution and composites additively AFTER the tonemap, in display
colour. Its own comment concedes the framing: "unlit emissive is only half the
material, the other half is the bloom that would pick it up." It is tied to the
main view twice over — window-sized targets, post-resolve timing — which is why
`syncSplitPanes` hides `kRingGlow` on every pane rather than let the switch lie.

So the device path should NOT port it. What it should do:

1. **An engine `BloomPass`**, threshold → downsample chain → upsample-accumulate
   → composite additively INTO the HDR scene target, before the resolve.
   Compositing into HDR rather than teaching the resolve a second input is what
   keeps the no-tonemap debug view meaning something.
2. **Ribbons as renderables with an EMISSIVE material**, drawn in the ordinary
   lit/transparent passes rather than in a pass of their own. Bloom then halos
   them because they are bright, which is the same reason a scorch decal's
   embers halo — one mechanism, not two.
3. **`GlowPass` and `kRingGlow` are deleted**, along with the pane
   special-casing that exists only to hide a main-view-only effect.

**WHAT THE RENDER SCENE ALREADY PAID FOR.** The plan's P2 — "per camera" — was
a separate phase because `GlowPass` could not be, and because each camera had to
grow its own chain. `ScenePipeline` is per VIEW, so a stage added at the resolve
seam exists for every view or for none; that is the unification lesson the plan
opens with, and the device path now satisfies it by construction rather than by
a second phase.

**WHAT IS STILL OPEN AND IS NOT A CODE QUESTION.** §3 of the plan: rings today
are crisp, ungraded display-colour ink drawn after the tonemap. Emissive HDR
rings tone-map like everything else, so they soften. The plan recommends
migrating — "the whole point of the exercise is one pipeline with no
main-view-only passes" — and gates it on a side-by-side. That decision needs
eyes, not an argument in a header, and it cannot be taken before the pass exists
to render the comparison.

**AND THE OTHER HALF IS §4.7.** An emissive term is a MATERIAL property, so the
ribbons' emissive is a `.mat` value and not a setter called from a renderer.
Bloom without anything that emits is a pass with nothing to do; the material
system is what feeds it.


### 4.7 The material system — THE MAIN EXTENSIBILITY SURFACE

The immediate task is small: `.mat` gains `albedo` / `normal` / `mrao` keys.
**Note:** `assets/materials/` holds no textures, but `assets/models/` has ~123
files that carry their own — so the live route is `ModelAsset::adoptTextures`,
not the material directory.

**But the direction is not small, and it governs how everything else here is
weighted.**

#### The goal, stated plainly

**One engine, many looks — the way Unreal gives every game on it a different
identity, and the way Frostbite carries a football game and a shooter.** The
material is where artists and gameplay programmers control that. It is not a
parameter list bolted to a fixed shader; it is the surface a game is EXPRESSED
through.

The consequence for this document is a reordering: **materials are the main
extensibility surface and the pass hatch in §4.12 is the rare exception.** If a
material can reach sun direction, the shadow map, scene depth and time, then
almost nothing a game wants needs a custom pass at all. Every section here that
treats the hatch as the primary extension point has the emphasis backwards.

#### The five pieces, and only the first exists

1. **A `.mat` that is data.** Done. `window.mat` already decides which PASS it is
   drawn in — `blend translucent` — with no C++ naming it and no shader written
   for it. That is the pattern the rest extends.
2. **Per-material shader variants.** §4.9. Nothing today: every material feeds
   one shared PBR shader and varies only parameters, which is exactly why
   `blend` can work and `shading` cannot.

   **TWO CORNERS OF THIS ARE SEPARABLE AND MUCH CHEAPER — see
   `study/plans/material_extensibility.md`.** What makes §4.9 large is the
   variant explosion: a surface shader crossed with the lighting model, shadows,
   probes, decals, blend modes and skinning. Two things escape it entirely:

   - **A fullscreen post-process material** has one shared vertex shader, no
     lighting and no mesh, so one material is exactly one fragment shader and
     the count of them is the count of effects.
   - **`shading unlit` is one more shader, not a variant system.** Unlit is not
     a point in a combinatorial space — it is the ABSENCE of the lighting term,
     so it is a single extra übershader selected by a material key exactly as
     `blend translucent` selects a pass. It removes rather than adds, so it
     brings no permutations with it.

   Both can therefore ship well before the toolchain, and both rehearse the same
   asset plumbing at a fraction of the risk. `overlay.mat`'s standing complaint
   — it wants to be unlit and settles for `roughness 1.0` — is closed by the
   second one alone.

   That note also records three gaps found by asking whether a game could author
   an XCOM-style path material or a stencil-driven outline today (it cannot):
   there is no post-material format, `SceneResources` never published
   `customStencil`/`customDepth`, and there is **no pass point between the
   finished linear scene and `drawBloom`**, so nothing a game writes can ever be
   picked up by bloom.
3. **A stable set of INPUTS a material may read** — world position, normal, UV,
   time, camera position, sun direction, the shadow map, scene colour, depth,
   ambient. **THIS IS A PUBLIC API AND SHOULD BE TREATED AS ONE.** The moment a
   studio ships a material written against `uSunDirection`, that name and its
   meaning are permanent — see the licensing note in §4.12, which makes the same
   argument about the scene. Design this list deliberately and once.
4. **Shading models** — unlit, lit, subsurface, custom. A material saying
   `shading unlit` and doing its own lighting is how a hologram, cel shading or
   a bespoke skin gets authored without touching the engine.
5. **Material instances** — one master material, many instances overriding
   parameters with no recompile. The most-used feature in Unreal by a distance,
   and the thing that keeps variant counts survivable.

#### The failure mode to design against: PERMUTATION EXPLOSION

Unreal's shader compile times are its standing complaint, and they are the
direct consequence of static switches multiplying out. Every switch doubles the
space; a dozen of them is thousands of variants nobody asked for.

The defence has to be built in from the start, not retrofitted:

- **Parameters wherever a parameter will do.** A value that can be a uniform
  must not be a switch.
- **Static switches only where the CODE genuinely differs**, not where a number
  does.
- **The permutation count must be VISIBLE** — printed by the toolchain, so it is
  a number somebody watches rather than a compile time somebody endures.

#### What is already right

The bones are in place and worth not disturbing: `.mat` is data; blend mode
already selects the pass with no code naming the surface; `ShaderLibrary`
splices includes so shared code stays shared; `common/brdf.glsl` and
`common/colour.glsl` are already shared between both renderers because they are
pure functions.

### 4.8 Props — blocked on content
`assets/models/props.txt` does not exist, so `PropSet` places zero instances on
**either** path. Converting it would produce no visible change. Not a code
problem.

### 4.9 Offline shader toolchain — AND IT IS NOT A CHORE

glslang → SPIR-V → SPIRV-Cross. Until it exists the dialect is "GLSL 450 that a
GL driver accepts", which has already cost one bug (`gl_VertexIndex` rejected).

**THIS ENTRY UNDERSOLD ITSELF AS PORTABILITY WORK. It is also the thing that
makes the material system expressive**, and on that reading it belongs further up
this list than its number suggests.

Today every material feeds ONE SHARED PBR SHADER and varies only parameters.
That is why `window.mat` can say `blend translucent` and change which pass it is
drawn in, but nothing can say `shading unlit` and light itself — there is no
mechanism to compile a shader per material and no variant handling to manage the
result.

Per-material shaders are what Unreal's and Unity's material graphs actually ARE:
authoring that compiles to a shader variant. A custom lighting model, a
hologram, cel shading, panning UVs beyond what a fixed parameter list can
express — all of them need this and none of them needs anything else.

So the toolchain buys three things at once: a dialect that survives the console
port, a material system a game can extend without C++, and the compile step
where a variant explosion can be seen and bounded.

**§4.7 IS THE REASON THIS MATTERS — read it before sequencing anything.** The
stated goal there is one engine carrying many looks, the way Unreal does, with
the material as the surface a game is expressed through. That makes this entry a
prerequisite for the project's main extensibility story rather than a
portability chore, and it is the reason §4.12 and §4.9 are the two pieces worth
arguing about the order of. **Sequence them by which blocks more of the
remaining work, not by these numbers.**

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
- ~~SSAO radius/bias/strength are copied constants in `drawOcclusion`.~~ **Done.**
  They are `SceneFrame` fields now, filled from the live
  `AmbientOcclusion::Tuning` the device renderer owns, so the dev panel's
  sliders reach this path. **And the constants had drifted exactly as §5's
  "tuning invented rather than borrowed" entry predicts** — 0.9 / 0.025 against
  the raylib path's 0.45 / 0.008, with the bias three times too large, which
  rejects the close occluders that produce contact darkening
- ~~Still unread: `decals`, `customDepth` and `toneMap`.~~ **`toneMap` landed
  with §4.5** — `SceneFrame::toneMap` reaches the resolve's flag, and the raw
  branch was already written in `tonemap.fs.glsl`. `decals` and `customDepth`
  remain unread because neither pass exists on this path; a switch for a pass
  that is not there has nothing to turn off and is not pretending otherwise
- **The layer switches now reach the device path too**, which was the same bug
  one level up: `ViewLayers::features` has eight switches and this renderer read
  TWO, so shadows, sky and ambient occlusion were checkboxes that moved and
  changed nothing. **Two of the three are a defined "off" VALUE rather than a
  skipped pass** — the lit pass samples the shadow map and the occlusion plane
  unconditionally, because a pipeline's bindings are the same every frame, so
  skipping would freeze the effect at the last frame that had it rather than
  turn it off. Depth 1.0 and white are the answers
- ~~`decals` and `customDepth` remain unread.~~ **Both landed.** `decals` is
  §4.6's DBuffer; `customDepth` is the tagged-object buffer and the selection
  outline that reads it. **All eight `ViewLayers::features` switches now reach
  this path**, which was the point of the entry above
- ~~**THE RHI's HARDWARE STENCIL IS HALF-BUILT.**~~ **Finished.** `StencilState`
  on `PipelineDesc` — compare, read and write masks, and the three ops named
  after the OUTCOME rather than the action, because `glStencilOp(sfail, dpfail,
  dppass)` is the argument order everybody gets wrong. `StencilOp` is every
  backend's eight, unchanged. `StencilState::write()` and `::testEqual()` are
  the two shapes almost every caller wants, so they are not rebuilt by hand.
  **ONE FACE, NOT TWO**: the only technique that needs separate front and back
  is two-sided shadow volumes, which this engine does not do and which would
  arrive with its own entry.

  **`setStencilReference` WAS THE HALF THAT WAS ACTIVELY WRONG.** It passed
  `GL_ALWAYS` and `0xFF` literally, so naming a reference silently destroyed
  whatever comparison the pipeline had asked for — a masking pass would draw
  everywhere the moment it named the value it was supposed to mask against. It
  had no callers, so nothing had found it. `glStencilFunc` sets comparison,
  reference and read mask together, so the backend now latches the pipeline's
  two halves at `bindPipeline` and the reference moves without them.

  **AND THE WRITE MASK GATES THE CLEAR**, which is the trap this state is most
  likely to spring: a pipeline that left `glStencilMask` at zero — which
  `testEqual()` does, correctly — makes every later `glClearBufferfi` write
  nothing, so the next pass to use the stencil inherits the previous frame's
  values with no call anywhere near it to blame. Both `beginPass` and the
  disabled branch of `bindPipeline` reopen it; neither alone is sufficient,
  because a pass can bind several pipelines.

  **A SELF-TEST STAGE, AND IT WAS CHECKED THAT IT CAN FAIL.** Write then mask:
  one draw tags the left half with a reference, a second draws over the whole
  target and should reach only the tagged half. Both halves are asserted,
  because each failure mode shows in only one — a test that never rejects fills
  the right half, a write that never happened leaves the left half empty.
  Deliberately disabling `GL_STENCIL_TEST` in the backend turns it red with the
  cause in the message. **A stage that cannot fail is not a test**, and that is
  worth checking rather than assuming.

  **NOTHING IN THE ENGINE USES IT YET, AND THAT IS FINE.** Custom depth does not
  need it — the value lives in a colour channel on both renderers, which gains a
  coverage channel a hardware stencil would have to spend a value on. What is
  gone is an interface promising something it could not do.
- **`ScenePipeline` has no GPU profiler zones at all**, so the dev panel's GPU
  lane says nothing about the device path. The backend pushes a
  `glPushDebugGroup` per pass — which RenderDoc and Nsight read and the in-game
  panel does not — so the information exists and is going to the wrong place.
  `drawBloom` has one (CLAUDE.md requires it of a new per-frame system) and is
  currently the only row. The fix is a `CW_GPU_ZONE` per pass, or better, the
  backend raising one from `PassDesc::name` where it already raises the debug
  group — one edit that gives every pass a row instead of fifteen edits
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

### 4.12 ~~Then: the render scene~~ — DONE

**All five steps landed.** `IGeometrySource` and `GeometryPass` are deleted, the
game implements no pass callback and issues no draw, and the engine owns the
list, culls it, sorts it and draws it. What follows is the design as it was
written, then a record of what actually happened and what changed on contact.

**THE MEASURED RESULT, which is the part worth trusting.** Every step was
captured with `--shot` and diffed against the frame before it:

| Step | Frame vs the step before |
|---|---|
| 1. `RenderScene`, `RenderableDesc`, `View`, `RenderAssets`, `IScenePass` | identical, max channel difference **0** |
| 2. Statics, chunked — 13,020 triangles in **71 renderables** (was 28 draws) | identical, **0** |
| 3. Bodies registered and synchronised once a frame | identical, **0** |
| 5. `IGeometrySource` deleted | **99.70% of pixels identical**, 0.096% differ by more than 2, max 25 — see below |
| 4. Overlays as renderables | new content, verified against the raylib overlays |

**The one difference is the shadow refit and it was expected.** Deleting the
seam took `worldBounds` with it, so the sun's orthographic box is now the UNION
OF THE RENDERABLES rather than the lattice plus a tile of margin. On this board
that is a box about 11% tighter on the diagonal, which moves the texel snap and
rescales the shader's world-unit biases — both of which are functions of the
fit. It is confined to the building (bbox 430,287–900,698), it is at most 25/255
on shadow edges, and it is the better answer: the sun's resolution is now spent
on geometry that exists rather than on the empty air above an untouched map.

**The overlays were verified against the renderer that already draws them.**
`--los --mouse 640 420` on both paths, each diffed against its own no-LOS
capture to get the overlay's own footprint: **bounding boxes within one pixel**
(300,274–930,737 versus 301,274–930,737) and 114,787 of 122,505 pixels in
common. The residue is antialiasing and the path preview, which is a ribbon
here and a one-pixel line there.

#### What the design got wrong, and was changed on contact

Three things. Each is written up at length where it lives; this is the index.

1. **THE FILTER MASK'S SENSE WAS BACKWARDS, and it would have shipped.** §4.12
   proposed a 32-bit key on the renderable ANDed against a mask on the view,
   drawn where non-zero — show-if-any-match. That cannot express a conjunction,
   and this game's cutaway is two independent axes a surface must pass BOTH of:
   a wall on a hidden storey whose facing is shown ANDs to non-zero and draws.
   **The storey cut would have leaked exactly when the facing cut was in use**,
   which is every ordinary camera angle.

   Inverted: the renderable says what it IS, the view says what it HIDES, and it
   is drawn where the two do not intersect. Still one AND. It composes any
   number of axes, and the defaults come out right for free — a view that hides
   nothing sees everything, so **the sun's view and a probe capture are correct
   by construction** rather than by a caller remembering to pass "the whole
   world". `cromwell/render/Renderable.hpp` carries the argument; `SceneTests`
   pins it.

2. **THE VIEWER IS A SEPARATE FIELD, not bits carved out of the filter key.**
   Open problem 2 asked to "reserve a viewer field", and reserving it inside the
   game's 32 bits would have meant the game could spend them. `viewers` is its
   own word, in the OPPOSITE sense (show-if-any-match), because ownership is
   naturally a positive statement: "show to player 2" is one bit, where the hide
   sense would be "hidden from every player except 2" — every bit but one,
   restated whenever the player count changes, and wrong by default.

3. **`castsShadow` AND `visibleInReflections` ARE ENGINE VOCABULARY, not filter
   bits.** The design mapped "casters only, for the sun" onto `castsShadow` and
   stopped there; a probe capture needs the same treatment and did not have it.
   Both are Unreal's names for Unreal's reasons, and both are asked OF THE
   RENDERABLE rather than of the pass — so the sun's view does not know what
   glass is, it knows that a window says it does not cast.

#### What was read out of Source and deliberately NOT copied

Two of the study's findings are recorded as non-adoptions, with the reasoning in
`RenderScene.hpp`. Both look like omissions otherwise.

- **The per-view translucency cache is unnecessary rather than skipped.**
  `m_TranslucencyCalculatedView` exists because Source asks the OBJECT for its
  origin — `GetRenderOrigin` is a virtual call into game code — so the answer is
  worth caching and therefore worth invalidating per view. We hold the centre as
  data, so the distance is a subtract and a dot product computed during
  collection into the per-view list. **The departure in this section paid for
  itself somewhere it was not aimed.** The bug the field guards against is
  unreachable here, not fixed.

- **The frame stamp is deferred, and the note says when to add it.** Source
  carries `m_RenderFrame`/`m_RenderFrame2` because a renderable reachable
  through several visible BSP leaves would be gathered once per leaf.
  Collection here walks a flat array exactly once per view, so nothing can be
  gathered twice and a stamp would be a store no path can read. It becomes
  necessary the moment collection goes through a spatial index and a chunk can
  span several visible cells — **and it belongs on the collector, not on the
  renderable**, because Source's TWO int fields are a two-player split-screen
  hardcoded into a data structure. Writing that down is why the field is not
  there: adding it later is a small change to one function, and un-hardcoding it
  after copying Source's shape would not be.

**Bucketing opaque by size WAS copied**, and with the tuning taken out of it.
Source classifies against absolute world units — a threshold somebody picked, in
a unit somebody's game defined. This uses the binary exponent of the largest
extent, one bucket per octave, which is scale-free: a game in metres, one in
tiles and one in centimetres get the same relative order with nothing to tune,
which matters for an engine meant to carry three genres.

#### The five open problems, closed

1. ~~**Probes are on the wrong object; materials too.**~~ **Both moved, in
   opposite directions, and naming the reason produced something more useful
   than either move.** Probes describe a WORLD, so `DeviceProbeSet` is the
   scene's. Materials are loaded once from `.mat` and shared by everything, so
   `DeviceMaterials` is the DEVICE's — see the new `RenderAssets`, which exists
   to name the three lifetimes this renderer keeps filing things under the wrong
   one of: **device** (assets), **world** (scene), **view** (pipeline).
2. ~~**Per-player overlays in a shared world have nowhere to live.**~~ Reserved
   as `ViewerMask viewers` on the renderable and `viewerMask` on the view — a
   separate field, in the show sense, for the reasons above. `RhiOverlays`
   passes `kAllViewers` today and the line that becomes `viewerBit(pane)` is
   marked; nothing else in the renderer, the scene or the engine changes when it
   does.
3. ~~**Mesh and material lifetime across scenes.**~~ **Stated as a rule with a
   place to point at.** A scene REFERENCES device resources and never owns or
   destroys one; whoever built a mesh destroys it, AFTER removing every
   renderable naming it. Shared geometry belongs to `RenderAssets`, whose
   lifetime spans every world. `RhiStatics`, `RhiBodies` and `RhiOverlays` all
   release in that order and say so.
4. ~~**Split-screen costs N times the WHOLE FRAME.**~~ Still true and now
   stated where it is enforced: `sunProjection` takes the VIEW, so each eye gets
   its own focused shadow map — see the note on that declaration, and §4.11's
   target-memory numbers.
5. ~~**Eight storey bits is a silent limit.**~~ `cromwell::kFilterFlagBits` is
   published so a game can `static_assert` its spend, and
   `game/render/scene/RenderFilter.hpp` does exactly that. The runtime check
   beside it is not redundant: the assert bounds the BUDGET, and a lattice's
   extents are runtime values, so a map that exceeds it is logged rather than
   silently unhideable.

**And `setMesh` exists.** The document's own inconsistency — the overlay example
used it while the API list did not offer it — is settled by the overlays being
built: a visibility field is ONE renderable whose mesh is rebuilt when the field
changes, which is the normal path rather than an edge case.
Remove-and-re-add would recycle the slot and invalidate the id its owner holds,
turning a mesh swap into bookkeeping at every call site.

#### What this cost that the design did not mention

- **`SurfaceKind::Overlay` and `assets/materials/overlay.mat`.** The overlays are
  translucent with a per-grade alpha, so they need a material saying `blend
  translucent`, and borrowing `Portal`'s would have turned every portal pad
  translucent to serve a marker. **They are lit, which is not what was authored**
  — a flat colour a designer picked, going through one PBR shader. `shading
  unlit` in §4.7 is exactly this case and is the next job.
- **One shader line.** `transparent.fs.glsl` now multiplies coverage by
  `vColour.a`. Glass is unaffected — the world's vertices and every object tint
  carry alpha 1 — and it is what lets one visibility mesh draw a directly-seen
  cell more strongly than a peek-only one, which no material parameter can
  express.
- **`MeshVertexBuffer::bounds()`.** A chunk's mesh needs an extent and the caller
  cannot see the vertices. Accumulating it in the caller instead would have been
  a second answer to a question the buffer already knows, which is how bounds
  and geometry drift apart — and bounds that do not enclose their mesh are a
  renderable that vanishes at certain camera angles.
- **A region overload on `StoreyGeometryEmitter::emit`.** Chunking needs it, and
  it is a clean partition only because walls are stored canonically. If that ever
  stops being true the symptom is a flicker at chunk boundaries.

#### And two pre-existing breakages found on the way

- **`tests/RhiTests.cpp` had not compiled since §4.2 and §4.3.** Its `NullDevice`
  was missing `copyBackbufferToTexture` and had stale arities for `draw` and
  `readTexture` — so `ctest` had been building nineteen of twenty targets and
  nobody noticed, because a target that fails to BUILD does not fail as a test.
  Fixed. **Worth knowing as a shape:** an interface addition breaks its stubs,
  and stubs live in test files nobody rebuilds until they do.
- **The shadow transmission plane was using the CAMERA's cutaway.** The pass is
  the sun's, and the game submitted it with `cutaway_` rather than
  `CutawayView::whole()` — the same class of mistake the storey cut made in the
  shadow depth pass, one pass over. It is now the sun's view by construction and
  cannot be expressed wrongly.

---

The original design follows, unchanged.


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

**Read `study/topics/rendering/render_scene_architecture.md` first.** It is
Valve's client leaf system read out of the Source SDK 2013 on this machine, and
four decisions below come from it rather than from first principles.

### THE REQUIREMENT, STATED ONCE

**N players, N screens, and each pane may be a DIFFERENT WORLD RUNNING ITS OWN
SIMULATION.** Not merely a second camera on one world — four players could be in
four independent sims, or two, or all in one, and a pane may want different
quality settings from its neighbour. Single player is the same machinery at
N = 1.

Everything below follows from that, and the one-line consequence is: **nothing
renderer-side may be global.**

### HOW MANY SCENES, AND HOW MANY VIEWS

**One scene per WORLD. Many views per scene. Not a singleton.**

This is the first decision and it is the one Source gets wrong for us. Its leaf
system is `EXPOSE_SINGLE_INTERFACE_GLOBALVAR` — one instance, one world — and
split-screen renders several VIEWS of that single world. Four players in four
DIFFERENT worlds cannot be expressed at all.

cromwell has to carry both cases:

- single player: one world, one scene, one view, one pipeline;
- four-player co-op in different worlds: **four scenes, four views, and
  potentially four pipelines** at different quality settings.

So `RenderScene` is an ordinary owned object that a world holds, and nothing
anywhere is global. A `View` names the scene it draws.

**THE CODEBASE ALREADY LEANS THIS WAY**, which is the encouraging part.
`CameraSet`, `SplitScreen.hpp` and `FrameRenderer::setPaneSource` exist, and
`ViewLayers` already travels WITH a camera rather than with the frame — the
comment on `playerTwoRig_` says the pane "inherits that choice without being
told". The filter mask below is that idea finished.

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

So a **View** is: **which scene**, matrices, a filter mask, **and what it renders
INTO**. Three KINDS exist — camera, sun, probe face — and a co-op frame has that
set per player.

**THE TARGET IS PART OF THE VIEW, and the first draft of this omitted it — which
was an oversight rather than a simplification.** The game already renders views
into textures today: `Camera` owns a `resolved_` texture, the minimap is the
whole board from above into its own target, and every split-screen pane is one.
A view with no target draws to the backbuffer.

That also folds the split-screen question into the same field: four panes are
four views with four targets, or four views with four viewports into one. Which
of those is cheaper is the open question at the end of this section, and it is a
question about the TARGET rather than about the view.

**SOURCE CACHES TRANSLUCENT SORTING PER VIEW AND SO MUST WE.**
`RenderableInfo_t::m_TranslucencyCalculatedView` records which view a cached sort
answer belongs to, and split-screen is exactly why it exists: two panes see the
same pane of glass at two different depths. A single cached "distance" on the
renderable is correct until the day a second view exists, which for this project
is a day that is already scheduled.

### Two things to copy from Source outright

- **Stamp each renderable with a frame number when it is collected.** Source
  carries `m_RenderFrame`/`m_RenderFrame2` because a renderable reachable
  through several visible leaves would otherwise be gathered once per leaf. Our
  equivalent is a chunk spanning several visible grid cells.
- **Bucket opaque by SIZE before material.** Source's groups begin
  `OPAQUE_STATIC_HUGE` and `OPAQUE_ENTITY_HUGE` so the biggest things draw
  first, occlude, and let the depth test reject more of what follows. It is a
  free early-z win that costs one comparison in the sort, and it is not
  something this design would have thought of.

One thing to copy the OPPOSITE of: Source's default group is
`RENDER_GROUP_OTHER` — "unclassified, won't get drawn". A renderable that never
says what it is silently vanishes. Ours should draw by default and complain.

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

   **WRITE THESE AS THE PUBLIC SURFACE.** They are what a licensee will be given
   when the engine ships without source, so they are the one part of this that
   is expensive to change later — see the licensing note. Everything else in
   `render/` stays internal, and saying so in the headers now costs nothing.
2. **Statics**, chunked. `RhiStatics` registers instead of drawing, and its
   `submit()` goes empty — so the frame is drawn once, by the scene, from the
   first converted producer onward.
3. **Bodies.** `RhiBodies` registers; transforms updated per frame.
4. **Overlays** (§4.4 dissolves).
5. Delete `IGeometrySource`, `GeometryPass`, and the `submit` halves.

### THE ONE DELIBERATE DEPARTURE FROM SOURCE

Source registers an INTERFACE and calls `DrawModel(flags)` back on it. cromwell
registers **data** — mesh, material, transform — and the engine issues the draw.

The reason is the whole point of the port: a `DrawModel` callback puts game code
inside a render pass holding a command encoder, which is precisely what
`IGeometrySource` already is and what is being removed. Source could afford it
because it has one backend and the game and renderer are one binary.

**The cost is real and should be stated rather than discovered.** Anything that
cannot be expressed as mesh-plus-material-plus-transform needs the ENGINE to
grow a component for it — ropes, beams, trails, particles — where Source lets
the game invent one. That is the same trade Unity's SRP and Godot's
`RenderingServer` make, and the same reason both ship a fixed vocabulary of
renderer components. It is the right trade for a multi-backend engine and the
wrong one for a single-backend game.

### WHAT THE GAME MAY EXTEND, AND WHERE THE HATCH IS

Four tiers. The middle two are where every argument will be.

**READ THIS FIRST OR THE TIERS LOOK LIKE PURITY.** Today cromwell and the game
are directories in one repository, compiled together. There is NO WALL: anyone
can open `ScenePipeline.cpp` and change it, and the tiers below are advice.
Exactly one thing is genuinely enforced — `XC_PLATFORM` and `XC_PC_BACKEND`
select SOURCES in CMake and `IPlatform::create` has one definition per build, so
a platform cannot be added at runtime because it is not a runtime thing.

**THAT CHANGES WHEN THE ENGINE IS LICENSED WITHOUT SOURCE — see below. The tiers
are written as though they were already enforced because one day they will be.**

For scale: Unity ships a closed core but leaves the render pipelines as
editable packages, and Unreal ships its renderer's source outright. Both are
MORE permissive about the pipeline than these tiers. Neither lets you add a
graphics backend.

**1. Engine only.** Platforms, graphics backends, the RHI, the pass order,
target formats and sizes. §4.11's quality presets depend on the last two staying
the engine's — a preset is a value only if nobody outside has pinned a format.

**2. Extend freely — this is the vocabulary, and it is encouraged.** Most of "our
game needs to look different" lands here and should:

- **Materials as `.mat`, including their SHADING MODEL and their own shader.**
  `blend translucent` already decides which pass a surface is drawn in with no
  C++ naming it; `shading unlit` plus a shader is the same idea applied to
  lighting, and it is how a hologram, a cel-shaded unit or a custom subsurface
  material should be authored. See §4.9, which is what makes it possible.
- **UV functions, tiling and time.** Panning normal maps, scrolling detail,
  anything Unreal and Unity express in a material graph. Data, not code.
- **Render targets the GAME owns.** Ask the renderer for one, point a view at
  it, sample the result in a material or blit it to the UI. Minimaps, security
  cameras, portals, mirrors and UI previews are all this, and the engine already
  does it per camera. **Not** the pipeline's own targets — scene colour, depth,
  the shadow map, the occlusion plane, the probe array — whose formats and sizes
  are what a quality preset moves.
- **What renderables exist and where.**
- **The filter bits, whose meaning is entirely the game's**, and its own layers
  and gameplay categories.

**3. THE HATCH — supported, deliberately unattractive, and RARER THAN THIS
DOCUMENT MAKES IT SOUND.** §4.7's direction is that materials are the main
extensibility surface: once a material can read sun direction, the shadow map,
depth and time, almost nothing a game wants needs a custom pass. Reach here for
what is genuinely a PASS — a scanner sweep, a bespoke composite — not for what
is genuinely a SURFACE. A pass at a NAMED
insertion point (`AfterOpaque`, `BeforeToneMap`, …) receiving an encoder and
**READ AND WRITE** access to named frame resources — scene colour, depth, the
occlusion plane, the shadow map — plus a named auxiliary target requested by
descriptor when the game needs a channel the engine has not got. This is not
hypothetical: the game already writes unit ids into a custom stencil channel.

**READ AND WRITE, not read-only, and the first draft of this document got that
wrong.** Blitting over scene colour at a named point is how custom post, an
outline composite and decals-over-scene are all done, and Unity permits exactly
that on `_CameraOpaqueTexture`. What threatens portability is changing WHICH
targets exist, their formats and their sizes, and the ORDER passes run in — not
writing into one that already exists.

**The line: the hatch gives you THE FRAME'S RESOURCES, not THE ENGINE'S
CONSTRUCTION.**

**Make hatch use VISIBLE.** Log at startup how many custom passes are registered
and where — "game: 1 custom pass at BeforeToneMap". Not to police it: **a hatch
used for something ordinary is a signal that the engine is missing a feature**,
and that signal is worthless if nobody can see it.

**4. Physically possible, unsupported — WHILE THE SOURCE IS IN THE ROOM.** Reach
past all of it and drive the device directly. C++ has no walls and pretending
otherwise would be a lie. It is not portable, and it is the first place to look
when a console build breaks.

### WHEN THE ENGINE SHIPS WITHOUT SOURCE, THE ADVICE BECOMES A WALL

The intention is to license cromwell to studios who get headers and a library and
build a game — **not the engine's source**. That is not a distant concern; it
decides things that are cheap now and expensive later.

**The tiers stop being advice.** A licensee cannot open `ScenePipeline.cpp`.
Whatever is not in a public header does not exist for them, and tier 4 —
"physically possible, unsupported" — disappears entirely.

**So the hatch has to be GOOD, not merely present.** With source in the room, an
inadequate hatch costs a studio an afternoon editing the pipeline. Without it,
the only answers are a support ticket and a bespoke build. Every case the hatch
does not cover becomes a fork you maintain.

**Decide the PUBLIC SURFACE, and start now.** `ScenePipeline.hpp` currently
exposes every private member, every target handle and the probe set. That is
fine in one repository and unshippable: **every header published is supported
forever.** Splitting public from internal is cheap today and horrible after a
studio has included the wrong thing. Concretely, this is a task to do while the
scene API is being written, not after — the scene, the view, the renderable and
the hatch ARE the public surface, and nothing else in `render/` needs to be.

**Unity is the precedent, not Unreal.** Unreal ships its renderer's source to
licensees, so its extension story can afford gaps — worst case you fork. Unity
ships a closed core and makes the render PIPELINES open, editable packages, and
that is not an accident: the pipeline is the thing games differ in, so it is the
thing that had to be left open. A closed cromwell will feel the same pressure.

**Which argues with this document.** "What is deliberately NOT in this" rejects a
render graph on the grounds that `ScenePipeline` owns the pass order explicitly
and readably, and a graph solves a problem this frame does not have. **Licensing
IS that problem.** The resolution is §4.11's: do not build it, but keep the pass
list able to BECOME data without a rewrite — passes as separate methods, budgets
as named constants in one place, formats in `TextureDesc`. Those rules already
exist for quality presets and they serve this too.

**And one thing that is not a design question at all: ABI.** Shipping C++
binaries means compiler versions, `std::` types across the boundary and inline
functions all become contracts. Engines that go closed either expose a plain C
interface or ship source anyway. It constrains what those public headers may
look like, and it is better known now than discovered by a licensee on a
different MSVC.

### FIVE PROBLEMS FOUND BY REVIEWING THIS DESIGN, ALL STILL OPEN

Written down before any code, because three of them decide where things live.

1. **Reflection probes are on the wrong object.** `DeviceProbeSet` lives on
   `ScenePipeline` today. A probe set describes a WORLD — where the rooms are and
   what they reflect — not a viewpoint. Two players in one world would each
   capture and prefilter the same probes, wasting the work and letting the copies
   drift. **Probes move to the scene.** `DeviceMaterials` has the same smell for
   the opposite reason: it is loaded from `.mat` files and shared by everything,
   so it belongs to the device or an asset layer, not to a pipeline.

2. **Per-player overlays in a SHARED world have nowhere to live.** Two players in
   one world each have their own selection, so each has their own visibility
   overlay and cover markers. Those renderables sit in the one shared scene and
   must be visible to exactly one view. The mask can say so — but the bit budget
   above spends everything on storeys and facings and allocates nothing for
   "whose view". **Reserve a viewer field.** This falls straight out of the
   four-players-two-worlds case and was missed on the first pass.

3. **Mesh and material lifetime across scenes.** Scenes only REFERENCE device
   resources. Two worlds sharing the unit cube means tearing down one world can
   dangle the other's renderables. Needs a stated rule: shared meshes belong to
   the asset layer, and a world owns only what it built.

4. **Split-screen costs N times the WHOLE FRAME, not N times the draws.** The
   shadow map is focused and texel-snapped to the CAMERA's frustum and SSAO is
   screen-space, so both are per-view. Four players is four shadow maps and four
   occlusion passes. Not a flaw — a budget fact that belongs beside §4.11's
   target-memory numbers rather than being discovered when someone opens four
   panes.

5. **Eight storey bits is a silent limit.** `1u << storey` misbehaves quietly
   past the field width. Wants a `static_assert` against the lattice's storey
   count so a taller map is a build error rather than a rendering mystery.

Plus one inconsistency in this document's own examples: the overlay uses
`scene.setMesh(...)` while the API list offers only add / remove / setTransform /
setVisible / setFilterKey. Either `setMesh` exists or an overlay rebuild is
remove-and-re-add. Decide it when the API is written — a per-change rebuild is
the overlay's normal path, so `setMesh` probably earns its place.

### What is deliberately NOT in this

- **No scene graph.** A flat array of renderables with world transforms. Parents
  and local transforms are an animation and attachment problem, and nothing here
  has one yet.
- **No render graph.** `ScenePipeline` already owns the pass order explicitly and
  it is readable; a graph solves a problem this frame does not have — **but see
  the licensing note above, which is the problem that would eventually force
  one.** Keep the pass list able to become data; do not make it data yet.
- **No instancing or batching**, per above.
- **No LODs, no occlusion culling.** Frustum only.

### THE ONE QUESTION THIS DESIGN DOES NOT YET ANSWER

**Whether four players means four `ScenePipeline`s or one reused four times** —
which, now that the target is part of the view, is the same as asking whether
four panes are four targets or four viewports into one.

`ScenePipeline` owns every render target, and they are sized to the screen: a
4096² shadow map, a 2x supersampled scene colour and depth, the occlusion plane.
That is tens of megabytes per instance before anything is drawn — §4.11 already
counts it — so four instances is not a detail.

The two answers, and they are genuinely different:

- **One pipeline, run per view.** Targets sized to the largest pane, passes
  re-run per player. Cheapest in memory; forces every player to share one
  quality setting, because the targets and formats are the pipeline's.
- **One pipeline per view.** Each player gets their own targets and their own
  settings. This is what "different pipelines" in the requirement asks for
  literally, and it multiplies the memory by the player count.

Probably: one pipeline per DISTINCT CONFIGURATION rather than per player, so
four players at the same quality share one and a fifth at a different one gets
its own. That keeps the common case cheap and the requirement expressible.

**It does not have to be decided to start.** Steps 1 to 5 are unaffected — the
scene and the views are the same either way, and this is a question about who
owns the TARGETS. Deciding it early would be deciding it with less information
than we will have after the statics are converted.

### 4.13 At parity
Delete `FrameRenderer`, `StaticsMesh`, `UnitRenderer`, `OverlayRenderer`,
`ISceneSource`, `PassContext`, `MaterialLibrary`.

**`IGeometrySource` is already gone** — §4.12 step 5 — so the list above is
shorter than it was by the one entry that was engine-side. `RhiStatics`,
`RhiBodies` and `RhiOverlays` are the survivors of each pair, and each carries a
"duplicated from X for the migration" note naming what it replaces. Then `ScenePipeline`'s per-pass clip-control
scoping collapses to one call at device creation, and `RhiStatics`/`RhiBodies`
drop their "duplicated from X for the migration" notes.

---

## 5. Traps already paid for

Each of these looked like a different problem than it was.

**A SENTINEL THAT MEANS "NO" CANNOT ALSO MEAN "NOT ASKED YET", AND THE GAP-FILL
IS WHERE THE TWO GET CONFUSED.** `RhiDecals` maps the game's decal material ids
onto the device's, and the mapping cannot be assumed to be the identity: a
material with no albedo file is REFUSED a device id, and three of the four demo
materials are built procedurally and have no file. So the converter stores the
mapping — correct — and filled gaps in it with the refusal value, on the
reasoning that an id nothing had asked about was as good as refused.

**IT IS NOT, BECAUSE DECALS ARE CONVERTED IN DRAW ORDER RATHER THAN MATERIAL
ORDER.** `inDrawOrder()` sorts by `sortOrder`, so the first decal seen named
material 1; the gap-fill then wrote "refused" into slot 0 before anything had
tried to load it — and refusals are cached, so it was never tried. Material 0 is
`example`, the only authored decal on the board and the dev tool's default.

**EVERY DECAL DISAPPEARED, AND THE ONE THAT MATTERED LOGGED NOTHING.** The three
procedural materials still warned, exactly as designed, so the log looked
*correct*: three known-unsupported materials skipped, and no complaint about the
one that should have worked. The absence of a warning read as success.

It was caught by measuring — the frame diff went from 3,095 changed pixels back
to zero across a refactor that was supposed to be a correctness fix — and then
by noticing that `assets: loaded ... example_albedo.png` had vanished from the
log. **A missing INFO line is evidence, and it is the kind nobody looks for.**

`std::optional` now carries the three states the problem actually has: never
asked, answered, refused. The general shape: **when a cache stores "no", check
whether the container's default is distinguishable from it — a vector's is
not.**

**A FIELD WRITTEN WHOLESALE EVERY FRAME HAS EXACTLY ONE PLACE TO BE SET, AND IT
IS NOT WHERE THE OBJECT IS CREATED.** The dev panel's draw-layer switches went
in as filter bits: statics tag themselves at registration, overlays tag
themselves at each rebuild, and bodies tagged themselves at registration too.
Statics and overlays worked. **Units produced a ZERO-PIXEL difference between
"units on" and "units off"** — byte-identical frames.

Because `RhiBodies::sync` writes the whole filter word every frame, on purpose:
a body changes storey by walking up a ramp, so the storey bit cannot be latched
at registration. That write is TOTAL, so it silently dropped the layer bit set
sixty lines above it. Nothing errors, nothing is missing from a capture, and the
checkbox moves.

**AND ZERO PIXELS IS THE WORST POSSIBLE SYMPTOM**, because it is exactly what a
switch that was never wired at all produces — which is what the entire rest of
this section was about, so the obvious conclusion was "that one is still to do"
rather than "that one is wired and being overwritten". It was found by measuring
each switch rather than by looking at the frame: six of seven moved and one did
not, and the one that did not was the one whose evidence looked like the bug
already being investigated.

The rule generalises past filter flags: **any per-frame wholesale write makes
every other assignment to that field dead code**, and dead code that reads as
configuration is worse than none.

**ONE FLAG WAS ANSWERING TWO QUESTIONS, AND THE SHADOW TRANSMISSION PLANE IS
BLANK BECAUSE OF IT — a LIVE bug, found by the preview panel on its first run
and NOT fixed here.**

`castsSunShadow(SurfaceKind)` returns false for `Window`, and `RhiStatics` puts
that on the renderable as `castsShadow`. `RenderScene::collect` filters
`ViewKind::Sun` on exactly that flag — so a window is dropped from the SUN'S
COLLECTION ENTIRELY, and the sun's list is collected once and read twice: the
opaque half is the depth map, and **the translucent half is the transmission
plane**, which exists for nothing else. Glass is excluded from the pass that was
built for glass.

Measured: every texel of `shadowTransmission_` reads 255 — the clear colour,
untouched, on a board with windows in it.

**THE COMMENT IN `RhiStatics` STATES THE INTENT AND THE CODE DOES THE OPPOSITE**
— "A window transmits light rather than blocking it and belongs in the
transmission plane" is written directly above the line that keeps it out. That
is what makes this worth an entry rather than a one-line fix in passing: the
flag is being asked *does this occlude in the depth map* and *should this be
considered by the sun at all*, and those are different questions that happen to
have the same answer for every surface except the one the transmission plane was
added for.

**WHY IT WAS INVISIBLE UNTIL NOW.** A missing transmission plane is a defined
"off" value — white, meaning the sun arrives unchanged — so the frame is a
correct render of a world with no coloured glass rather than a broken one. There
is no artefact to notice. It took a picture of the buffer, which is precisely
what §4.5's texture inspector was built for and the first thing it found.

**Left alone deliberately.** The fix is a decision about what `castsShadow`
means to the collector — whether the sun's view should take translucent
non-casters, or whether the renderable needs a second word — and that is
engine vocabulary, not dev-panel wiring.

**A RENDER TARGET AND AN IMAGE DISAGREE ABOUT WHICH ROW IS FIRST, AND THE
SYMPTOM IS NOT "IT IS UPSIDE DOWN".** A texture's first row is at the BOTTOM of
whatever was rendered into it; ImGui — like every image format — addresses from
the top. So a buffer copied texel for texel into a preview and handed to a panel
comes out flipped.

**On the images this panel exists for, that does not look like a flip.** A
shadow map is a silhouette on a light field, an occlusion plane is soft grey
blobs, a normal buffer is pastel noise — none of them has a horizon or any other
cue that says which way up it was. The flipped picture looks like a perfectly
ordinary buffer that simply does not match the frame, and the conclusion
available from there is that the PASS is wrong. There is nothing in the picture
to suggest the copy.

The flip therefore lives in the copy, once, and both preview paths promise a
top-down image — which is why `DevView`'s texture panel says "drawn as given, do
not add a flip here": the panel cannot tell which buffer an entry came from and
so cannot be right about it.

**AND THE CUBE PREVIEW BESIDE IT DOES NOT FLIP, WHICH LOOKS LIKE AN OVERSIGHT
AND IS NOT.** That shader SYNTHESISES its image from a direction rather than
copying one: a cube face's `tc` already runs top-down, so the two conventions
already agree and a flip there would introduce the artefact rather than remove
it. **Write the flip where the frames of reference actually differ, not
everywhere the word "texture" appears** — a rule applied uniformly to both files
would have been wrong in one of them, and each file says which case it is.

**FOUR uint32_t PARAMETERS IN A ROW, AND TWO OF THEM WERE NOT WHAT I THOUGHT.**
`updateTexture(handle, pixels, layer, mip)` takes a SLICE and a LEVEL — the size
comes from the texture. The dev panel's ImGui backend called it as
`updateTexture(handle, pixels, width, height)`, which compiles perfectly because
all four are `uint32_t`, and uploads a 1x1 sub-image into mip level 128 of a
texture with one level. **No pixels are written and nothing errors.**

What that looks like from the outside is worth the whole entry, because none of
it points at a texture upload. The panel submits every vertex, issues every
draw with the right counts and the right texture id, and puts NO PIXEL on the
screen — because the fragment stage is `vColour * texture(atlas)` and a black
atlas multiplies the entire interface away. Every observable stage is correct.

**IT COST AN HOUR OF BISECTING BY REASONING AND FIVE MINUTES OF BISECTING BY
SHADER.** The diagnostics that ruled things out one at a time — is the panel
visible, is ImGui producing draw lists, is the atlas created, are the push
constants within budget, is the scissor clipping, is the texture incomplete —
all came back clean, because all of them WERE clean. What actually found it was
three edits to the fragment shader, which needs no rebuild:

    outColour = vec4(1, 0, 1, 1);              -> the toolbar appears: geometry,
                                                  pass, scissor and constants
                                                  are all fine
    outColour = vec4(vColour.rgb, 1);          -> white: the vertex colour lives
    outColour = vec4(texture(...).rgb, 1);     -> BLACK: found it

**The rule: when every stage you can observe is correct, stop reasoning about
the stages and start bisecting the fragment.** A shader edit is seconds on this
project and it partitions the problem in a way that argument cannot.

And the parameter shape is a repeat offence. §5 already records `draw(mesh, 1)`
silently changing from one INSTANCE to one VERTEX when a vertex range was
inserted before it. **When adjacent parameters share a type, the compiler cannot
help, and the symptom surfaces somewhere else entirely.**


**A BOUNDING BOX IS NOT A STAND-IN FOR A DIAGONAL, AND A TILE GAME IS MADE OF
DIAGONALS.** The path preview's first version emitted an axis-aligned box
spanning each segment, with a note reasoning that a diagonal's box "reads as a
slightly wider link rather than as a rotated one". It does not. Every step of a
path here is one tile long, so a diagonal's axis-aligned box is a FULL TILE
SQUARE lying on the ground — and a path across open ground drew as a row of
white plates with the occasional thin bar wherever a step happened to be
axis-aligned. Reported from a screenshot as "it renders multiple squares".

The rule the note should have applied: **a bounding box approximates a shape
only while the shape is much longer than it is wide IN THE AXES IT IS BOUND
TO.** A one-tile diagonal is precisely the case where it is not, and it is also
the commonest step this game takes — so the failure was not an edge case, it was
the normal path.

The fix is a ribbon oriented along the segment, built in world space. **Not a
rotation on the renderable's transform**, which was the other option and is a
worse one: `object.glsl` carries no normal matrix, and its `mat3(model)` is
exact only for the untransformed world and axis-aligned boxes — so a rotated
strip would have been lit by a normal that is not its own. That is a subtler
wrong than the squares, and it is the kind that gets attributed to the lighting.


**A FILTER MASK WITH THE OBVIOUS SENSE CANNOT EXPRESS TWO CUTS, and the design
review did not catch it because the arithmetic looks right.** "Every renderable
carries a key, every view carries a mask, draw where `(key & mask) != 0`" is one
AND per renderable and reads as obviously correct. It is correct for ONE axis.
This game's cutaway has two — a storey and a wall facing — and a surface must
pass BOTH; under that formulation a wall on a hidden storey whose facing is
shown ANDs to non-zero and draws. **The storey cut leaks exactly when the facing
cut is in use, which is every ordinary camera angle**, and the symptom is walls
appearing above the iso level while the cut visibly works everywhere else.

Inverting the sense fixes it in the same one AND: the renderable says what it IS
and the view says what it HIDES, drawn where the two do not intersect. The
general lesson is the one that transfers: **a mask whose default is "draw
nothing" is a mask you must remember to fill; one whose default is "hide
nothing" is correct for every pass that has no opinion** — which is the sun's,
every probe face's, and every view a second project ever adds.

**A DEFAULTED FRUSTUM MUST ACCEPT EVERYTHING, NOT REJECT EVERYTHING.** A
`Frustum` built from nothing has no planes, and the natural loop over zero
planes rejects nothing — which is the safe answer and is only safe by accident.
Written the other way (a flag meaning "not ready", tested as "cull it"), a view
whose matrices had not been filled yet draws an empty screen with no error
anywhere. Same rule as CutawayView: the safe value is the one you get for free.

**THE NEAR PLANE IS THE ONE THAT DEPENDS ON THE DEPTH CONVENTION.** Gribb and
Hartmann's row sums are independent of everything except this: `w + z >= 0` is
the -1..1 form and `z >= 0` is the 0..1 form, and this engine renders with 0..1
while raylib shares the context at -1..1 (see the `glClipControl` trap above).
`Frustum` takes the -1..1 form deliberately — it describes a volume extending
BEHIND the eye, so it can admit a box the hardware will clip and can never
reject one the hardware would draw. **A culler that errs must err toward
drawing**; the other direction is a hole in the world.

**A TEST TARGET THAT FAILS TO BUILD DOES NOT FAIL AS A TEST.** `tests/RhiTests.cpp`
had not compiled since `copyBackbufferToTexture` was added for the UI's backdrop
blur and since `draw` and `readTexture` grew parameters — three interface
changes, each of which broke a stub in a file nobody rebuilt. `ctest` reported
nineteen of twenty passing and said nothing about the twentieth, because it was
never linked. Found only by building every target at once. **An interface
addition breaks its stubs, and the stubs are in the files least likely to be
compiled by the change that broke them.**

**A CACHE KEY MADE OF "THE THINGS THAT CHANGE IT" GOES STALE, AND SILENTLY.**
The overlays rebuild when their inputs change, and the tempting key is the list
of causes — the selected unit, its position, the iso level. That list is a claim
about EVERY cause, and the first new one invalidates it without a compile error:
a grenade demolishes a wall, the visibility field genuinely changes, nothing in
the key moves, and the overlay keeps showing line of sight through a wall that
is not there. It reads as a LOS bug in the simulation. Hashing the data instead
is correct by construction and costs a few thousand bytes a frame against a
rebuild it is protecting that costs far more. Same shape as the glyph cache
keyed by an atlas address below: **borrowing a proxy for identity means
borrowing every way the real thing can change.**


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
