# Material extensibility: how a game expresses its own look

**A design note, not research** — alongside [`bloom_emissive.md`](bloom_emissive.md)
and [`nav_architecture.md`](nav_architecture.md), same contract: what cromwell
should build, in what order, with the decisions named and the rejected paths
recorded. Written **2026-08-16**, out of a selection-outline session that built
the wrong thing twice before the actual requirement was stated.

---

## 1. The requirement, stated properly

**cromwell must not have a house style baked into it.** The engine ships the
vocabulary; the game picks the look. That is MIGRATION.md §4.7's stated goal —
one engine carrying many looks, with the material as the surface a game is
expressed through — and it is the thing that makes the engine liftable into the
next project, exactly as the `cromwell` / `game` arrow is.

Two concrete looks, both of which must be expressible **as content, by a game
developer, without touching the engine**:

- **An XCOM-style movement path** — flat, unlit, display colour, exempt from tone
  mapping and grading, reading as fluorescent without emitting anything.
- **A bloomed emissive outline** — HDR radiance above 1.0, composited before
  bloom so it haloes with the rest of the frame's light.

**Neither is the engine's answer, and that is the point.** They are opposite
choices — one deliberately outside the frame's gamut, one deliberately inside its
lighting — and an engine that hardcodes either has picked a game's art direction
for it. What went wrong twice in the session that produced this note was
answering "make the outline bloom" with engine features: first a placement flag,
then a broader placement flag. **A flag on a hardcoded pass is a feature. The
material system is the vocabulary, and features accumulate flags forever.**

This note is the plan for that vocabulary. It is deliberately wider than the
outline that prompted it.

## 1.1 What "like Unreal and Unity, but better" has to mean

The stated bar is not just parity. It is worth pinning down, because "more
expressive than Unreal" is not a goal anyone can build against and is probably
not achievable — Unreal's graph can express nearly anything.

**What Unreal and Unity are actually bad at is telling you what you built.** A
material's real behaviour is scattered across a details panel, a domain dropdown,
a shading-model dropdown, usage checkboxes that silently multiply compile times,
and a blendable-location dropdown with five options and no statement of what each
one excludes. The asset is binary, so it does not diff and cannot be reviewed.
Finding what a post-process material costs means a shader-complexity view and a
guess. **The expressiveness is there; the legibility is not.**

So the bar is the same expressiveness with the material **declaring itself, and
the engine holding it to the declaration**. Six things, each of which is a
concrete thing to build and a concrete thing to be judged on:

**1. The file is the truth, in plain text.** Everything a material is — shading
model, placement, exemptions, inputs, parameters — is in one readable file. It
greps, it diffs, it reviews in a pull request, and it can be understood without
opening a tool. No hidden state in a binary asset or a panel.

**2. The engine validates the declaration and refuses to be silently wrong.**
This is the biggest single win available and Unreal does almost none of it.
Contradictions that are *knowable at load time* should be errors or warnings, not
mysteries discovered by eye three weeks later:

- `emissive 4.0` with `location afterToneMap` — values above one are clamped
  there, so the number is a lie. **Warn.**
- an `inputN` the shader never samples, or a sampler the shader uses that the
  file never declared. **Warn on both sides.**
- a parameter key nothing reads, or a shader parameter no file sets. **Warn.**
- `shading unlit` together with lighting-only parameters like `metalness`.
  **Warn.**

Each of these is a real mistake that Unreal accepts in silence.

**3. Every material is enumerable, and so is the vocabulary.** A developer can
ask the engine what inputs exist, what locations exist, what shading models
exist — and for a given material, exactly which it uses and where it lands in the
pass order. The dev panel (F1) already exists and this is a tab in it. Unreal has
no equivalent of "list every legal material input and what it means".

**4. Cost is visible per material, automatically.** Every post-process material
gets a profiler zone named after it, on both the CPU and GPU lanes, without the
author doing anything. This is already engine doctrine — CLAUDE.md's rule that an
unzoned system "does not show up as a zero row, it shows up as nothing at all" —
and applying it to materials means the question "what is this effect costing me"
is answered by looking, not by investigating. **This is the axis where beating
Unreal is easiest and most useful.**

**5. The vocabulary explains itself.** Every term in the format is documented
where it is defined, with what it costs and what it rules out, to the standard
the rest of this codebase already holds headers to. A dropdown with no
explanation is the thing being replaced.

**6. No hidden permutations.** One material is one shader. There is no checkbox
that quietly multiplies compile time, because §5 is the constraint that keeps it
so.

## 1.2 The limitation that proves the point, in Epic's own source

`[EPIC]` Read 2026-08-16 from
`UE_5.7/Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessMaterial.cpp:83-107`.

The specific complaint that prompted this section — *"the After Tonemap stuff was
limited last time I checked"* — is not a misremembering. **Unreal refuses to run
a post-process material's stencil test at After Tonemapping at all**, and says
so:

```cpp
else if (MaterialRenderProxy->GetBlendableLocation(Material) == BL_SceneColorAfterTonemapping)
{
    // We can't support custom stencil after tonemapping due to target size differences
    UE_LOG(LogRenderer, Warning, TEXT("PostProcessMaterial uses stencil test, but is set to blend After Tonemapping. This is not supported."));
}
```

The function returns `EMaterialCustomDepthPolicy::Disabled` and the effect
silently does nothing but log.

**BUT "YOU CANNOT DO IT" IS NOT QUITE RIGHT, AND THE TRUTH IS WORSE.** Two
different mechanisms reach the stencil there, and only one is refused:

- **The stencil TEST** — the material's `Enable Stencil Test`, which culls the
  post-process quad against custom stencil — is what the code above disables.
- **SAMPLING it** via `SceneTexture:CustomStencil` is not blocked at all. It
  compiles and runs at After Tonemapping.

And the sampling path is a straight integer load
(`Engine/Shaders/Private/SceneTexturesCommon.ush:139-146`):

```hlsl
uint CalcSceneCustomStencil(uint2 PixelPos)
{
	return SceneTexturesStruct.CustomStencilTexture.Load(uint3(PixelPos, 0)) STENCIL_COMPONENT_SWIZZLE;
}
```

`PixelPos` is the pass's pixel position. An After-Tonemapping pass runs at
**native** resolution — the same file says so while explaining SSR input, which
"is always rendered at native resolution *as if it was after tone mapping*, so we
need to account for the fact that it is independent from DRS"
(`PostProcessMaterial.cpp:268`). The custom stencil buffer is at **render**
resolution. So the load reads the wrong texels the moment those differ: any
screen percentage below 100, TSR, TAAU, DLSS, dynamic resolution scaling.

**Nothing warns.** It is correct on a developer's machine at 100% and quietly
wrong for any player who enables an upscaler — which is most of them, and which
is a bug that will be reported as "the outline looks off on my machine" and
reproduce nowhere.

**So Unreal has both failure modes at this one location**: an honest refusal for
the path it knows it cannot align, and a silent wrong answer for the path it does
not check. That pairing is the whole argument of §1.1.2 in one place — a system
that will not validate its own declarations has to choose between disabling
features and mis-rendering them, and Unreal does one of each.

**Read the stated reason: "target size differences."** The custom depth-stencil
buffer lives at render resolution while an After-Tonemapping pass runs at display
resolution, and rather than reconcile the two, the feature is switched off at
that location. **That is precisely the bug this project spent a session fixing
for its own outline** — a stencil on one grid compared against a target on
another — and the fix is written down in
[`../topics/rendering/outline_antialiasing.md`](../topics/rendering/outline_antialiasing.md)
§8.4: map the finer buffer to the coarser grid by exact integer division, and
reduce over the footprint rather than point-sampling it. Integer mapping, nothing
that can drift, exact at parity.

So the single most-wanted combination — **a stencil-driven effect in display
colour, after the tone map** — is the one Unreal does not offer, and it is
unavailable for a reason already solved here. That is not a small differentiator;
it is exactly the XCOM-style look of §6 crossed with the stencil masking of §4.2.

Two smaller restrictions in the same file, worth knowing before designing the
locations:

- **The location is a shader define, not a runtime choice.**
  `POST_PROCESS_MATERIAL_BEFORE_TONEMAP` is set at compile time from the
  blendable location (line 267), so a material is compiled *for* a location and
  moving it is a recompile. Reasonable, but it means "try it at each point to see
  which looks right" is not a fast loop in Unreal. It should be one here — see
  §10.3 on hot reload.
- **Some outputs are location-specific.** `MaterialShader.cpp:2760` notes
  UserSceneTextureOutput is "only supported for post process domain, and not
  supported for BL_ReplacingTonemapper". More dropdown combinations that are
  quietly illegal.

**The rule this sets for our design:** *any published input is available at any
location, and resolution reconciliation is the engine's job, not a reason to
disable the feature.* Where a mapping genuinely cannot be made — a buffer that
does not exist yet at that point in the frame — the engine says so **at load
time, by name**, rather than logging a warning at runtime and drawing nothing.
That is §1.1.2 applied to the exact case Unreal gets wrong.

### The honest trade

**Unreal's node graph is genuinely better for artists**, and text-first is worse
for them: visual authoring, live preview, no shader language. That is a real cost
and should not be argued away.

The position taken here is that for a programmer-led project, text-first plus
validation plus visible cost beats graph-first — and that the trade is not
permanent. **A graph is a generator; this format is what it would generate.**
Building the declarative format first keeps that door open. Building a graph
first would have made the format an implementation detail nobody could read,
which is the situation being escaped.

---

## 2. The two mechanisms, and why the distinction matters

The pair of looks above need **different machinery**, and conflating them is how
the first version of this note came out scoped to half the problem.

| | XCOM-style path | bloomed outline |
|---|---|---|
| what it is | **geometry** with a material | **fullscreen pass** over the frame |
| what drives it | a ribbon mesh the game builds | the custom stencil buffer |
| what it needs | shading model + pipeline exemptions | a post-process material |
| MIGRATION entry | §4.9 (`shading unlit`) | §4.9's screen-space corner |

**A surface material** is applied to a mesh and is about how that mesh is shaded
and which pass it lands in. `blend translucent` already works this way today.

**A post-process material** has no mesh. It runs over the whole frame at a
declared point, samples the pipeline's buffers, and writes a colour.

Both are "materials as data" and both belong in the same asset format. Neither
subsumes the other, and the engine needs both before the claim in §1 is true.

---

## 3. Where it stands, checked against the tree

**Materials are parameters, not programs.** `MaterialDefinition.cpp` parses
`key value` lines into a `PbrMaterial`. One shared opaque shader and one
transparent shader serve every material. That works, and its ceiling is already
documented: `overlay.mat` wants to be unlit and cannot say so, settling for
`roughness 1.0` with a comment conceding *"the honest answer is `shading unlit`,
which needs the per-material shaders of MIGRATION.md 4.9."*

**Screen-space effects are C++ methods on `ScenePipeline`.** `drawOcclusion`,
`drawBloom`, `drawResolve`, `drawOutline` — each with its own pipeline handle,
its own std140 block, its call site fixed in `render()`. Adding one means editing
the engine.

**The hatch exists and is the right idea.** `IScenePass` + `ScenePassPoint` lets
a game inject a pass at four points, and `SceneResources` publishes a narrow
slice of the pipeline's targets to sample. **This design is not a replacement for
the hatch — it is the hatch with the C++ taken out**, so the ordinary case is a
file.

---

## 4. What is missing, on the post-process side

Found by asking "can a developer build a stencil-driven outline today?" — no, for
three independent reasons, and only the third is interesting.

**4.1 There is no post-material format.** Nothing lets an asset say: here is a
fragment shader, run it at this point, bind me these buffers, here are my
parameters.

**4.2 `SceneResources` does not publish the custom stencil or custom depth.** It
carries scene colour, depth, normals, occlusion and the shadow maps — not the two
buffers that exist *specifically* to be read by effects.
`CustomDepthStencil.hpp` has said since it was written that the buffer's value is
generality: *"x-ray silhouettes, selection outlines, masking a post effect to
particular actors, depth-of-field and blur masks are all the same buffer with
different consumers."* Every one of those is currently locked out. **Two-line
fix; should not wait for the rest of this note.**

**4.3 There is no pass point before bloom, and this one is structural.**

```
drawLitScene → hatch(AfterOpaque) → drawTransparent → drawDebugLines
             → drawBloom → hatch(BeforeToneMap) → drawResolve
             → drawOutline → hatch(AfterToneMap)
```

`AfterOpaque` fires *before* transparents, so an effect there sits under glass.
`BeforeToneMap` fires *after* `drawBloom`, so an effect there is linear but bloom
has already sampled the scene. **Nothing can mean "everything linear is finished
and bloom has not run"** — precisely where an emissive effect must sit to glow. A
`ScenePassPoint::BeforeBloom` between `drawDebugLines` and `drawBloom` closes it.

Not only an outline problem: any game effect that wants to emit — a shield flash,
a heat shimmer's hot core, a glowing reticle — hits the same wall.

---

## 5. Why both mechanisms are tractable corners of §4.9

§4.9 is blocked on an offline toolchain (glslang → SPIR-V → SPIRV-Cross) because
per-material *surface* shaders mean variant handling: lighting model crossed with
shadows, probes, decals, blend modes, skinning, and a compile step where the
explosion can be bounded. Correctly sequenced as one large piece.

**Neither thing this note needs is that piece.**

**Post-process materials need none of it.** A fullscreen pass has one shared
vertex shader, no lighting permutations, no mesh variants — one material is
exactly one fragment shader, and the count of them is the count of effects. The
entire variant problem is absent.

**`shading unlit` is one more shader, not a variant system.** This is the part
worth noticing. Unlit is not a point in a combinatorial space — it is the
*absence* of the lighting term, so it is a single additional übershader beside
the opaque and transparent ones, selected by a material key exactly as `blend
translucent` selects a pass today. It brings no permutations with it because it
removes rather than adds.

So **both can ship well before the toolchain**, and both rehearse the same asset
plumbing at a fraction of the risk. That is a sequencing argument, not an excuse
to skip §4.9 — a hologram, cel shading or custom subsurface still needs the real
thing.

---

## 6. The XCOM case: the vocabulary is EXEMPTIONS, not just placement

The reference proves that `location` alone is not enough.
[`../games/strategy/xcom2_movement_border.hlsl`](../games/strategy/xcom2_movement_border.hlsl)
already took XCOM 2's movement ribbon apart from the SDK. **The instinct that it
does not use the tone map is right, and the reason is bigger than the tone map.**

Three facts, all from Firaxis' own files:

1. **Bloom is off globally.** `XComGame/Config/XComEngine.ini`,
   `[SystemSettings]`: `Bloom=False` — the base section, alongside DepthOfField,
   AmbientOcclusion, SSAO and ScreenSpaceReflections.
2. **The material never exceeds white.** Colour is `(0.177, 0.666, 0.666)`, one
   parameter straight to output. Under any sane threshold even with bloom on.
3. **It is drawn after the post chain entirely.** `bIs3DUI`, and Firaxis'
   comment in `Engine/Classes/Material.uc`: *"If true, this material is used with
   the 3D UI and should be rendered after gamma correction."* Not tone mapped,
   not graded, not available to bloom even in principle.

**So the halo is not a halo.** It is fifteen texels of authored shoulder on a
64-wide profile texture — painted by an artist, computed by nothing. What makes
flat cyan read as *emitting* is five flags, each exempting the surface from a cue
that would tie it to scene lighting:

| flag | exempts it from |
|---|---|
| `MLM_Unlit` | shading — perfectly flat, no gradient |
| `bIs3DUI` | tone mapping and colour grading |
| `bAllowFog = false` | aerial perspective; it never recedes |
| `ForceNoHaveSeenFOW` | fog-of-war tinting |
| `bDisableDepthTest` | occlusion — nothing is ever in front of it |

The mechanism, worth restating because it redirects the design: **a surface reads
as fluorescent when it is more saturated than the illumination can explain.** The
eye infers the gamut a real surface could occupy under the scene's light — a
gamut established precisely by everything else having gone through the same tone
map and grade — and anything outside it reads as self-luminous. The ribbon sits
outside that gamut by construction *while being dimmer than white*. **Not
brightness. Purity.**

**A caution that saves work.** Reaching for bloom to get this look is the
documented mistake: `ribbon_glow.fs.glsl` exists in this tree because that study
note *used to* claim the halo was scene bloom, and it re-draws the ribbon
overbright and blurs it to stand in for something that was never there. The note
now says plainly: *"There is nothing to stand in for."*

---

## 7. The design

### 7.1 One format, two kinds

Reuse the existing `key value` parser — same shape as `.mat`, same reasoning
(`MaterialDefinition.hpp`: the point is that it can be edited without a tool).
A material is a **post-process** one if it declares a `location`; otherwise it is
a surface material as today.

### 7.2 Surface materials gain a shading model and exemptions

```
# assets/materials/movement_path.mat
shading     unlit          # emissive is the output; no lighting term
location    afterToneMap   # = bIs3DUI: skips tone map and grade
depthTest   off            # never occluded
fog         off            # no aerial perspective
emissive    0.177 0.666 0.666
```

Each term maps to something Unreal already has, which is the test of whether it
belongs in the vocabulary. Note `location` on a *surface* material means "which
pass this geometry is drawn in", extending what `blend translucent` already does.

### 7.3 Post-process materials

```
# assets/materials/post/outline.mat
shader      post/outline.fs.glsl
location    beforeBloom    # afterOpaque | beforeBloom | beforeToneMap | afterToneMap
blend       alpha          # alpha | additive | replace

input0      customStencil  # bound to sampler slots in declaration order
input1      customDepth
input2      sceneDepth

colour      1.0 0.85 0.30 1.0
emissive    4.0            # above bloom's 1.1 threshold, so it haloes
thickness   2.0
```

The engine compiles the shader, binds the named buffers, packs the remaining keys
into a parameter block, runs the pass at the declared location.

### 7.4 The input vocabulary is a public API

The names an `inputN` may take are the engine's published list and nothing more —
`sceneColour`, `sceneDepth`, `sceneNormals`, `occlusion`, `customStencil`,
`customDepth`, `shadowMap`, `shadowTransmission`. This is `SceneResources`
spelled as strings, and it inherits §4.12's rule: a material may **sample** the
pipeline's targets and may never learn their format or size, because those are
what a quality preset moves. MIGRATION.md's own warning applies —
*"the moment a studio ships a material written against `uSunDirection`, that name
and its meaning are permanent"* — so this list is designed deliberately and once.
An unknown name is a warning and a skipped input, the same contract `.mat`
already has for unknown keys.

**AVAILABILITY IS UNIVERSAL, AND THAT IS A DELIBERATE DEPARTURE FROM UNREAL.**
Every input on the list may be sampled from every location. Where the buffer's
resolution differs from the pass's — the custom stencil at 4× read by a pass
running at surface resolution, say — **reconciling them is the engine's job**,
using the integer-mapping discipline the outline already proved
([`../topics/rendering/outline_antialiasing.md`](../topics/rendering/outline_antialiasing.md)
§8.4). §1.2 records Unreal disabling stencil-after-tonemapping outright for
exactly this reason; a material system that answers a resolution mismatch by
removing the feature has handed the problem back to the author. The only
legitimate refusal is a buffer that does not exist yet at that point in the
frame, and that is a **load-time error naming the input and the location**, never
a runtime warning and a silently blank effect.

### 7.5 Where the locations land

| location | fires | sees | can feed bloom |
|---|---|---|---|
| `afterOpaque` | after the lit pass | opaque world, no glass | yes |
| `beforeBloom` | after transparents and debug | the finished linear scene | **yes** |
| `beforeToneMap` | after bloom | linear scene incl. bloom | no |
| `afterToneMap` | after the resolve | display colour — **skips tone map and grade** | no |

`beforeBloom` is new (§4.3); the rest exist. `afterToneMap` is Unreal's
`bIs3DUI`.

**A note for the implementer:** at `afterOpaque` and `beforeBloom` a pass
rasterises into the **supersampled RGBA16F** scene target, so `gl_FragCoord` is
in scene texels, not output pixels. The occlusion-pass bug in MIGRATION.md §5 is
exactly this mistake, and it does not look like a resolution error — it looks
like the effect being noisy.

---

## 8. The acceptance test: both looks, in the vocabulary

The design is right when both of §1's looks are assets and neither is engine
code.

**XCOM-style path** — a ribbon mesh plus §7.2's material. Flat 0.666 cyan, unlit,
after the tone map, no depth test, no fog. The soft shoulder comes from the
profile texture, not from a pass. No bloom involved anywhere.

**Bloomed outline** — the custom stencil plus §7.3's material at `beforeBloom`
with emissive above bloom's threshold. Haloes with the rest of the frame's light,
tone mapped with it, and shifts with exposure — which for a radiance effect is
correct.

**A third the engine should not have to anticipate**, and the real test: an x-ray
silhouette that reads the same stencil, tints occluded pixels, and runs at
`afterOpaque` so glass draws over it. Nothing in the engine changes.

### 8.1 Both at once, in the same frame

They are different materials at different locations reading a shared buffer, so
there is no conflict to resolve: the XCOM-style path composites in display colour
after the resolve, the emissive outline composites as radiance before bloom, and
each gets the treatment its category deserves. Two IDs in the same custom stencil
tell their subjects apart. **That is the §1 requirement met — opposite choices,
both expressible, neither privileged.**

### 8.2 Why the after-tonemap case works here and not in Unreal

Worth stating precisely, because it is **structural and not cleverness**, and
because misreading it would invite copying the trick into a situation where it
does not hold.

Unreal's stated blocker is "target size differences" (§1.2), and the sizes in
question are **render resolution against display resolution**. Those differ by an
arbitrary, often *fractional* and sometimes *per-frame* factor: screen percentage
73%, TSR, TAAU, DLSS, dynamic resolution scaling. **There is no integer mapping
between the two grids to be had**, so a `.Load()` at an integer pixel position
cannot be made correct in general — which is why one path is disabled and the
other is silently wrong.

Ours differ by a factor the engine chose and constrained:

- `createSceneTargets` allocates the stencil as `surfaceWidth * outlineSupersample`
  — an **exact integer multiple** of the surface, never a percentage.
- `withOutlineSupersample` snaps the factor to 2, 4 or 8 and floors it at the
  scene's own, so the multiple is always a power of two and never below one.
- The pass runs at surface resolution, so `base = ivec2(gl_FragCoord.xy) * factor`
  is exact, and reconciling against `sceneDepth_` is a further integer divide.

So the mapping is **integral and static** where Unreal's is **fractional and
dynamic**. That is the whole difference, and it is a property of what cromwell
does not do rather than of anything it does better.

### 8.3 Does this rule out upscalers and dynamic resolution? No.

An earlier draft of this note said a temporal upscaler would bring "Unreal's
problem here unchanged". **That is too pessimistic and worth correcting**, because
read as a roadmap constraint it would rule out features for no good reason. What
would be lost is today's *exactness*, which is a bonus of the current arrangement
and not load-bearing.

**First, separate two things that get conflated** — they were conflated in the
conversation that produced this note, which is why it is worth spelling out:

| | depends on | under an upscaler |
|---|---|---|
| **Coverage** — the effect's SHAPE | the stencil buffer alone | allocate the stencil at **output** resolution → exact, no bias needed |
| **Occlusion** — visible vs x-ray COLOUR | stencil depth vs *scene* depth | scene depth stays at render resolution → the only residual |

Coverage is the part seen as quality, and it is fully solvable by allocation
alone. Occlusion decides a colour rather than a shape, and is the narrow case
that needs care. **A depth bias is only ever about the second**, so no bias
question — `fwidth` included — has any bearing on whether an upscaler is viable.

**The strongest mitigation is one Unreal does not have, and it is cheap.**

Unreal's custom depth-stencil is part of the **scene-texture set** — allocated at
render resolution alongside the G-buffer, sharing the depth-prepass
infrastructure. Moving it is not a small change, which is why "target size
differences" reads as a fact of life there.

**Ours is a standalone target that rasterises a handful of tagged objects.**
`drawCustomDepth` draws only what the scene tagged — a soldier or two — so its
cost is negligible and **its resolution is a free parameter**, already
demonstrated by `withOutlineSupersample` moving it between 2×, 4× and 8× with no
consequence to anything else. There is no reason it must follow the render
resolution.

So under an upscaler the answer is: **allocate the stencil against the OUTPUT
resolution, not the render resolution.** A display-colour post material then
reads it at exactly its own grid, and the mismatch never arises whatever the
upscaler is doing. The design principle worth writing down now, because it is
what preserves the option:

> **The custom stencil's resolution is tied to its CONSUMERS, not to the scene.**
> It is cheap enough to rasterise wherever it is wanted, and nothing about it
> needs to share the G-buffer's fate.

**The problem a temporal upscaler does bring is JITTER, not resolution** — and
that is the one to plan for. A jittered projection makes the tagged geometry
wobble sub-pixel each frame, which is exactly the shimmer
[`../topics/rendering/outline_antialiasing.md`](../topics/rendering/outline_antialiasing.md)
§7.1 records Unreal suffering by default (`r.CustomDepthTemporalAAJitter`, on).
Here the handling is better by the same argument as above: render the stencil
**unjittered at output resolution** and a display-colour effect reading it is
perfectly stable and perfectly aligned — strictly better than Unreal's default,
not merely equal.

What genuinely does get harder is the **occlusion test against an upscaled
frame**: scene depth would remain at render resolution and be temporally
jittered, while the stencil is unjittered at output resolution. Comparing them
needs the footprint reduction generalised to a non-integer ratio plus a
slope-aware bias. **The tool for that is Epic's plane fit, not Source 2's
`fwidth`** — `fwidth` measures a surface's slope only when the shader is *on* the
surface, and in a fullscreen pass it returns the depth discontinuity at the
silhouette instead, which is the one place the bias must not blow up. See
`outline_antialiasing.md` §6 item 2 for the full correction. Solvable, less
exact, and the place to spend the effort when it arrives.

**None of that is a reason to avoid upscaling.** It is a reason for §1.1.2's
load-time validation to know the ratio and say something when it stops being
integral, rather than a player's screenshot being the first report.

---

## 9. The decision on the built-in outline

**Keep the C++ outline as the fallback; let an asset replace it later.** The
user's call, 2026-08-16, against deleting it and shipping it purely as an asset.

The argument for deleting was that two paths drift, which MIGRATION.md warns
about repeatedly. The argument that won is practical: the C++ outline is
*finished and correct* — supersampled stencil on its own quality dial, occlusion
pinned to the scene's grid, rotated-grid sampling, octagonal tap set (see
[`../topics/rendering/outline_antialiasing.md`](../topics/rendering/outline_antialiasing.md)).
Deleting it before the asset path exists trades a working feature for a plan.

So the asset path arrives beside it and the built-in becomes the default an asset
overrides. Drift is managed by the built-in being the **reference implementation**
the asset version is checked against.

**What must not happen is the built-in outline growing more knobs.** Every flag
added is a feature the asset path then has to reproduce. It is done; further
outline work belongs on the asset side.

---

## 10. Open questions

**10.1 How parameters are declared.** Fixed named set per shader is safe and
needs a code change per parameter, which defeats the point. Arbitrary keys packed
in declaration order is fully modular but the shader and file must agree **by
position** — a silent-wrong-value bug of exactly the kind std140 mismatches
already cause here. **Recommendation: arbitrary but named** — the shader declares
its parameter names, the file supplies them by name, the engine warns on a key no
shader reads and a parameter no file sets.

**10.2 Multi-pass materials with their own render target.** §4.12 allows "render
targets the GAME owns", so this is in-bounds. **Sharpened by §6:** a
display-colour effect cannot use bloom by construction, so any *computed* halo on
one needs its own blur target — which is what `GlowPass` and
`ribbon_glow.fs.glsl` are today, a bespoke stopgap outside the material system.
But §6 also shows XCOM's halo is *authored*, so the reference look needs no blur
pass at all. **Prove single-pass first; absorbing `GlowPass` is the acceptance
test for multi-pass when something actually needs a computed halo.**

**10.3 Hot reload.** `.mat` files are not watched. A post material is
shader-adjacent and the iteration loop is the point of the exercise, so this
matters more here than for surface materials. F5 already reloads the splash
shader; the same mechanism should extend.

---

## 11. Rejected

**A placement flag on the built-in outline.** Answers one request and generalises
to nothing: the next effect wanting to glow needs its own flag, and the one after
that its own pass. The shape the engine already has too much of.

**Publishing `ScenePipeline` so a game can drive passes directly.** Rejected by
§4.12 in advance — publishing the class publishes every target, every private
member and the pass order, and every header published is supported forever.
`SceneResources` is the deliberately narrow slice.

**Waiting for §4.9's toolchain.** Rejected on §5: the variant problem that makes
§4.9 large is absent from both a fullscreen pass and an unlit shading model, so
coupling them delays the cheap half behind the expensive one.

**Reaching for bloom to reproduce XCOM's ribbon.** Rejected on §6, on evidence
from the game's own config: there is no bloom in that frame to reproduce.

---

## 12. Order of work

1. **Publish `customStencil` and `customDepth` in `SceneResources`.** Two lines,
   unblocks every hand-written hatch pass, correct independently of everything
   below.
2. **Add `ScenePassPoint::BeforeBloom`.** Small, and what makes emissive effects
   possible at all.
3. **`shading unlit` plus the exemption keys** (`depthTest`, `fog`) on surface
   materials. One extra übershader and some pass-state plumbing; closes
   `overlay.mat`'s standing complaint and is most of the XCOM path.
4. **The post-material loader and runner** — parse, compile, bind, dispatch. The
   bulk of the work.
5. **Port the outline to an asset** and check it against the built-in, which
   stays as the fallback per §9.
6. **Parameter naming (§10.1), multi-pass (§10.2), hot reload (§10.3)** once
   something real is using the path and the friction is known rather than guessed.

Steps 1–3 are each worth doing on their own merits whether or not 4 follows.

**And §1.1's six properties are not a later polish pass.** They are what
distinguishes this from a worse Unreal, so they land with the loader rather than
after it:

- **Validation (§1.1.2) is part of step 4, not step 6.** A loader that accepts a
  contradiction silently is the thing being replaced, and warnings are cheapest
  to write while the format is being parsed for the first time.
- **The profiler zone (§1.1.4) is part of step 4** — CLAUDE.md requires a zone in
  the same commit as the system, and a material system that made effects
  invisible to the panel would break that rule wholesale rather than once.
- **Introspection (§1.1.3) is a dev-panel tab** and can follow, but the data it
  needs — the input list, the location list, what each material declared —
  should be queryable from the start rather than retrofitted.
