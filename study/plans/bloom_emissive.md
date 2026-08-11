# Bloom / emissive: what it takes

**A design note, not research** — the second one in this directory after
`nav_architecture.md`, and the same contract: what cromwell should build, in
what order, with the decisions named and the rejected paths recorded. Written
2026-08-11, immediately after the camera/pipeline unification, because that
work is what makes this pass cheap to add and is also what exposed the gap:
splitscreen panes cannot show the ribbon glow, and the reason is that the glow
is not a pipeline stage at all.

## 1. Where emissive stands today

Three facts, all checked against the tree as of this note:

**The HDR pipeline was built for this and is waiting.** `HdrTarget.hpp` opens
by explaining that an LDR target clamps emissive to 1 on the way in "and there
is nothing left to bloom" — the RGBA16F scene target exists precisely so that
radiance above 1.0 survives to be picked up by a pass that does not exist yet.
Every camera renders into one (`scene_` for the main view, each capture's own),
and every camera resolves through `ToneMapPass` with a per-camera
`RenderFeatures.toneMap` switch. The slot for bloom — after the lit scene,
before the resolve — exists on both paths and is currently empty.

**One emissive source already ships, and it is already HDR.** The decal system
writes an emissive mask whose contribution is scaled by
`PbrShader::kDefaultDecalEmissiveScale = 6.0` — six times white, well above
the threshold any bright-pass would use. The moment a bloom stage exists,
glowing decals halo with **zero further work**. Nothing else emits: the lit
shader has no material emissive term, so a lamp, a window at dusk, a muzzle
flash all have no way to say "I am a light source" to the image.

**The ribbon glow is not bloom — it is a bespoke stopgap, and it cannot go
per-camera.** `GlowPass` (game/render/ribbon/) re-renders the ribbon geometry
emissive at half window resolution into its own blur targets and composites
additively onto the backbuffer, after the tonemap, in display colour. Its own
comment concedes the framing: "unlit emissive is only half the material, the
other half is the bloom that would pick it up." It is tied to the main view
twice over — window-sized targets, post-fullscreen-resolve timing — which is
why `syncSplitPanes` hides `kRingGlow` on every pane camera rather than let
the switch lie. Replicating it per pane would mean a blur-target set per pane
for a geometry-specific hack. That path is rejected below (§6).

## 2. The design: an engine BloomPass, per camera by construction

A `BloomPass` in `cromwell/post/`, beside `ToneMapPass`, operating on **any
HdrTarget it is handed**:

1. **Bright pass**: threshold the scene in linear radiance (luminance above a
   knee-softened threshold survives), into the top of the chain — at the
   RESOLVE resolution, not the supersampled one. Bloom is a low-frequency
   effect; blurring the 2x buffer is paying four times for blur that is about
   to be blurred.
2. **Downsample chain**: progressive half-res blurs (1/2 → 1/4 → 1/8 → 1/16 of
   output), each a small-kernel pass. Progressive rather than one wide blur
   because a single kernel's radius does not scale — the chain is what makes
   a bright window bleed softly across many pixels for the cost of a few
   narrow blurs at tiny resolutions.
3. **Upsample-accumulate** back up the chain, then **composite additively into
   the HDR target**. Compositing into HDR rather than teaching `ToneMapPass` a
   second input keeps the resolve untouched: bloom brightens radiance, the
   tonemap grades it, and the raw/no-tonemap path (`RenderFeatures.toneMap`
   off) gets bloomed radiance for free, which is the correct meaning.

**Where it runs, on both paths, identically:**

- a capture: inside `Camera::captureNow`, between the `Main` phase and the
  resolve;
- the main view: in `FrameRenderer::render`, between the lit-scene block and
  the resolve.

Same position, same function, which is the whole lesson of the unification: a
stage added at this seam exists for every camera or for none.

**The switch and the allocation follow the established rules.**
`RenderFeatures.bloom` (a named engine feature — cromwell owns the pass), and
the blur chain allocated the way the depth prepass already is: asking for the
feature is what allocates the targets, dropping it frees them, there is no
second call to forget. Concretely: a `BloomChain` object owned per camera
(and one by the renderer for the main view), synced from the layers exactly
as `syncScreenSpaceBuffers` syncs the prepass. It is deliberately NOT crammed
into `ScenePassBuffers` — those three parts share one size (the camera's HDR
size) and one gating rule (`needsDepthPrepass`); the bloom chain has a
different size policy (resolve resolution) and a different gate, and a struct
whose parts obey different rules is two structs wearing one name.

## 3. Emissive sources — the content half

The pass is half the feature; the other half is things that emit.

**Step 0 — a material emissive term.** The lit shader gains
`emissiveColour × emissiveIntensity` per material, added to the HDR output
after lighting (emission is not lit). Authoring stays in `MaterialLibrary`
like every other factor. The demo content that proves it: window panes at
dusk, or a lamp prop — anything an artist can point at.

**Already done by accident:** decal emissive (§1). The first visible result of
P1 below should be a scorch-mark decal's ember glow blooming, before any new
material work lands.

**The ribbon migration — a decision, not a default.** The rings currently
draw post-tonemap in display colour, deliberately: crisp, ungraded interface
ink. To participate in bloom they must draw in the `Main` phase with emissive
values above 1 — which also tone-maps them, softening the look. Two honest
options:

- **Migrate**: rings become emissive HDR geometry; bloom halos them per
  camera; `GlowPass` retires; `kRingGlow` becomes meaningless and is removed
  (the halo is a consequence of emissive + bloom, not a layer). The look
  changes and needs eyes on it — this ships as its own reviewable step.
- **Don't**: rings stay display-colour ink; bloom serves world emissives
  only; `GlowPass` stays as the main view's ring halo and panes simply have
  crisp unhaloed rings. Less unified, zero look risk.

The recommendation is to migrate — the whole point of the exercise is one
pipeline with no main-view-only passes — but it is gated on a visual pass, and
the fallback costs nothing to keep open since P1/P2 do not touch the rings.

## 4. Order of work

Each phase ships independently and leaves the build green.

- **P1 — the pass, main view only.** `BloomPass` + `BloomChain` in
  `cromwell/post/`; `RenderFeatures.bloom` (default **off** until measured);
  wired into `FrameRenderer::render` at the resolve seam; a `bloom` profiler
  zone in the same commit (CLAUDE.md rule — an unzoned pass inflates its
  neighbours invisibly); dev-panel checkbox beside the other engine features
  and tuning for threshold / intensity; bright-pass and final-chain textures
  added to the dev panel's preview strip, because a bloom that misbehaves is
  diagnosed by looking at its intermediates. Acceptance: a placed emissive
  decal halos; the panel row says what it costs.
- **P2 — per camera.** The `BloomChain` sync on `Camera`, the call in
  `captureNow`, and nothing else — the feature switch already travels on
  every camera's layers. Acceptance: the same decal halos in a splitscreen
  pane and in the CCTV feed, each through its own chain.
- **P3 — material emissive term** in `PbrShader` + `MaterialLibrary`
  plumbing + one authored glowing surface in the demo map.
- **P4 — the ribbon decision** (§3), executed whichever way the visual pass
  lands. If migrated: `GlowPass` deleted, `kRingGlow` removed from
  `DrawLayers`, the pane special-casing in `syncSplitPanes` deleted with it.

## 5. Costs, so the choice is informed

**Memory**: the chain starting at half output resolution sums to roughly a
third of the output's pixel count, in RGBA16F — about 5–6 MB at 1920×1080 for
the main view, proportionally less per pane/capture. Paid only by cameras
whose layers ask (the allocation rule makes that automatic).

**Time**: the bright pass at output resolution plus ~6–8 blur passes at ≤ half
resolution — the shape of cost that has always been cheap on this hardware,
but the rule stands: the panel row is the authority, and sub-zones are earned
by a measurement, not anticipated (CLAUDE.md, granularity).

## 6. Rejected, and why

- **Replicating GlowPass per pane.** Per-pane blur targets for a pass that
  only knows how to draw ribbons; solves one camera's rings and nothing else,
  and every future emissive would need its own copy of the same trick.
- **Bloom after the tonemap.** Display-colour bloom halos from an image whose
  highlights have already been compressed to ≤1 — the exact information the
  HDR target exists to preserve is gone by then. `HdrTarget.hpp` said this
  first.
- **A compute-shader mip chain.** GL 4.3 compute is available, but the
  fragment ping-pong is simple, portable across the pipeline's existing
  target machinery, and nowhere near the measured limit. Revisit only if a
  capture-heavy frame shows the chain as a big slice — chasing constant
  factors in an unmeasured pass is the discipline document's named mistake.
- **One wide blur instead of a chain.** Kernel radius does not scale with
  bloom radius; the halo either shimmers (undersampled wide kernel) or costs
  quadratically. The progressive chain is the standard answer because it is
  the correct one.

## 7. Open questions

- **Threshold vs exposure.** The palette is dark and the default exposure is
  4.5 (`ToneMapPass` explains why); a threshold fixed at 1.0 scene-linear may
  sit above or below "reads as glowing" once graded. The threshold likely
  wants stating relative to the camera's exposure — decide with the P1 dev
  panel sliders in hand, not in advance.
- **The ribbon look** (§3) — needs Jamie's eyes on a side-by-side, not an
  argument in a header.
- **`unshaded()`** already turns every feature off by name; `bloom = false`
  joins the list. A 2D game's recipe takes bloom or leaves it via the same
  switch as everything else — no special story needed, which is the test that
  the design is in the right shape.
