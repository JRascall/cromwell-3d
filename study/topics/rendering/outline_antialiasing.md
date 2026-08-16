# Antialiasing a selection outline

**Read 2026-08-16 from shipped source on this machine, not from talks.** Unreal
**5.7** (`C:/Program Files/Epic Games/UE_5.7/Engine/`) and **Source 2** via
**s&box** (`E:/SteamLibrary/steamapps/common/sbox/`), which ships its shaders as
plain-text `.shader` next to the compiled `.shader_c`. The UE **4.x** copy of the
same shader (2015, public mirror) is quoted once in §3.4 only to show the
approach has not changed in a decade. Source tags: `[EPIC]`, `[VALVE]`,
`[COMMUNITY]`, `[inferred]`.

Written because cromwell's outline stair-steps and the obvious diagnosis —
"we're missing an AA pass" — is wrong. Neither engine solves this with an AA
pass. **They both make the outline carry sub-pixel coverage, and they do it in
two completely different places.** That fork is the whole note.

---

## 1. The finding, before the detail

An outline aliases when its shader answers a **yes/no** question per pixel. Every
technique below is a way of turning that into a **fraction**. There are exactly
two families:

| | Where coverage comes from | Engine |
|---|---|---|
| **A** | **The rasteriser.** Draw the outline as real geometry into a multi-sampled target and let hardware coverage do it. | **Source 2** (extruded silhouette), **Unreal editor** (MSAA editor-primitive target) |
| **B** | **Counting.** Run the binary edge test N times per pixel and average. | **Unreal editor** (per-MSAA-sample loop in the composite shader) |

Unreal uses **both at once** and that is not redundancy: A gives it a
multi-sampled *stencil* to read, and B is how a full-screen pass gets at the
individual samples, since it cannot resolve a stencil the ordinary way.

**Neither engine reaches for FXAA/SMAA/TAA to fix an outline.** Unreal's game-side
custom-depth outline is precisely the one that *doesn't* get either treatment,
and it is the one that visibly aliases (§4). That is the negative control for
this whole note.

---

## 2. Why ours stair-steps

Three independent causes, and **fixing only the resolution would fix none of
them**.

1. **The buffer is 1:1 with the display.** The scene renders at 2× per axis
   (`kSupersample`, [ScenePipeline.cpp:91-112](../../../src/cromwell/render/ScenePipeline.cpp#L91-L112)),
   but the custom stencil and custom depth are allocated at *surface* resolution
   — [ScenePipeline.cpp:766-784](../../../src/cromwell/render/ScenePipeline.cpp#L766-L784).
   The silhouette is rasterised at one sample per output pixel before anything
   reads it.
2. **The pass runs after the resolve.** `drawOutline` blends into
   `view.target()` — [ScenePipeline.cpp:2375-2426](../../../src/cromwell/render/ScenePipeline.cpp#L2375-L2426).
   There is no downsample left downstream to average anything, so a 2× buffer
   alone would still buy nothing.
3. **The edge test is one bit, and this is the real cause.** `tagged()` is a hard
   `abs(id*255 - wanted) < 0.5` on a point sample, and the output is the full
   colour or fully transparent — [outline.fs.glsl:59-99](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L59-L99).
   A 1-bit mask stair-steps at *any* resolution.

The point sampling is correct and must stay: filtering two IDs yields a third
that names no object. Both engines agree — Source 2 uses `g_sPointWrap`
throughout `ObjectHighlight.shader`, Unreal uses `.Load()` and never `Sample()`
for the stencil. **So coverage cannot come from the sampler. It has to come from
counting, or from the rasteriser.** Which is exactly the fork in §1.

### 2.1 And we already have the sample-count mismatch bug, today

This is the one that matters most, because it is the *same* bug that makes MSAA
notorious for wrecking outlines in Unreal (§5), and we have it without having
MSAA at all.

`sceneDepth_` is allocated at the **supersampled** size — `createSceneTargets`
opens with `// EVERY SCENE TARGET IS SUPERSAMPLED` and computes
`width = surfaceWidth * kSupersample`
([ScenePipeline.cpp:645-662](../../../src/cromwell/render/ScenePipeline.cpp#L645-L662)).
`customDepth_` is allocated at the **surface** size
([ScenePipeline.cpp:766-784](../../../src/cromwell/render/ScenePipeline.cpp#L766-L784)).
The outline shader then compares them **at the same UV, with point samplers on
both**:

```glsl
float objectDepth = texture(uCustomDepth, uv + inward).r;   /* 1x buffer */
float sceneDepth  = texture(uSceneDepth,  uv + inward).r;   /* 2x buffer */
```
— [outline.fs.glsl:114-115](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L114-L115)

`uv` is built from `gl_FragCoord.xy / size` with `size` the *surface* dimensions,
so `uv = (x + 0.5) / w`. In the 1× custom depth that is exactly a texel centre.
In the 2× scene depth it maps to texel coordinate `2x + 1`, and `GL_NEAREST`
selects `floor(u · size)` — so it lands on subsample `2x+1` every time, and
likewise `2y+1` vertically.

**This is not a flicker, it is a consistent bias**, which is worse for
diagnosis: the scene-depth tap is always the *same* corner of the 2×2 block, the
one whose centre sits **a quarter of a pixel down and right** of where the custom
depth sample was taken. So the occlusion test compares the object's depth at the
pixel centre against the scene's depth a quarter-pixel away. On a sloped surface
that is a genuine depth difference proportional to the slope; along a silhouette
it can be a different surface entirely.

The shader already knows the symptom, and says so:

> *"A BIAS, because the two buffers are separate rasterisations and disagree by a
> fraction of a texel along exactly the silhouette this pass lives on. Without it
> a visible outline flickers between its two colours along its own edge as the
> camera moves."*
> — [outline.fs.glsl:110-113](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L110-L113)

**`kBias` is mostly a plaster over a resolution mismatch, not over a
rasterisation subtlety.** That reframes the fix in §6.1: putting the custom
stencil and depth at 2× is not only how coverage gets counted, it **deletes the
dominant error term** — both buffers become the same size, sample-for-sample
aligned, and the quarter-pixel offset disappears.

### 2.2 The residual after that, and why it is already known to be zero here

Matching the resolution leaves one honest question. The two buffers are written
by **two different shader programs**: the custom depth pass uses
`rhi/scene/depth_only.vs.glsl` (ScenePipeline.cpp:1507-1523, whose comment says
so explicitly — *"THE VERTEX STAGE IS depth_only's, unchanged"*), while
`sceneDepth_` is written by the prepass using `rhi/scene/prepass.vs.glsl`. Both
compute the identical expression with the identical association:

```glsl
gl_Position = uViewProjection * objectTransform() * vec4(inPosition, 1.0);
```
— `depth_only.vs.glsl:53` and `prepass.vs.glsl:39`

but **GLSL guarantees nothing about bit-identical results across two programs**
unless `gl_Position` is declared `invariant`. `prepass.vs.glsl` also computes
`mat3(model) * inNormal` in the same program, which gives an optimiser a reason
to schedule the matrix chain differently. This is the classic invariance trap and
it is the correct thing to worry about here.

**It is already answered, empirically, by the renderer working at all.** The lit
pass tests `CompareFunc::Equal` against the prepass depth:

```cpp
lit.depth.test    = true;
lit.depth.write   = false;
lit.depth.compare = CompareFunc::Equal;
```
— [ScenePipeline.cpp:1197-1199](../../../src/cromwell/render/ScenePipeline.cpp#L1197-L1199)

`Equal` on a float depth is the strictest possible cross-program invariance
demand — one ULP of disagreement between `lit.vs` and `prepass.vs` and the lit
pass would drop the fragment and the object would vanish. **The pipeline already
stakes the entire image on two different vertex programs producing bit-identical
`gl_Position`, and it does.** So the same holds for `depth_only.vs` against
`prepass.vs`, and once the resolutions match, the tagged object's depth in the
two buffers is equal, not merely close.

`[inferred]` Which means `kBias` can go to zero, or to a one-ULP epsilon kept
only as insurance against a future driver. It should not stay at `0.0005`, a
value tuned to hide a quarter-pixel spatial offset that no longer exists. If a
belt-and-braces guarantee is ever wanted, `invariant gl_Position;` in both vertex
shaders is the standard tool and costs a few scheduling opportunities in a
depth-only stage that has nothing to schedule.

---

## 3. Unreal — a private MSAA target, then average the samples `[EPIC]`

### 3.1 Deferred has no MSAA, so the editor builds its own

`FSceneTexturesConfig::GetEditorPrimitiveNumSamples`
(`Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:193`) reads
`r.MSAA.CompositingSampleCount`, **default 4**
(`Engine/Source/Runtime/Core/Private/HAL/ConsoleManager.cpp:4115-4121`), and its
help text is unambiguous about what it is for:

```
"Affects the render quality of the editor 3d objects.\n"
" 1: no MSAA, lowest quality\n"
" 4: 4x MSAA, high quality (high GPU memory consumption)\n"
```

This is **independent of the scene's AA method**. The deferred renderer resolves
with TAA/TSR and has no MSAA anywhere; the editor allocates a *separate*
4×-multisampled colour+depth+stencil target purely so gizmos and selection
outlines have hardware coverage. The header comment names it:

> `// Number of MSAA samples in the Editor.Primitive<Color/Depth> textures.`
> — `Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:144`

**That is the decision worth stealing: they were willing to pay for a bespoke
multi-sampled target for one interface feature**, in a renderer whose entire
architecture is built around not having MSAA.

### 3.2 The composite shader loops over samples and averages

`Engine/Shaders/Private/PostProcessSelectionOutline.usf`. The stencil is bound as
a genuine multi-sample texture:

```hlsl
#if MSAA_SAMPLE_COUNT > 1
Texture2DMS<float, MSAA_SAMPLE_COUNT> EditorPrimitivesDepth;
Texture2DMS<uint2, MSAA_SAMPLE_COUNT> EditorPrimitivesStencil;
#endif
```
— lines 13-20

and the whole edge test runs once per sample, then divides:

```hlsl
[unroll(MSAA_SAMPLE_COUNT)]
for(SampleID = 0; SampleID < MSAA_SAMPLE_COUNT; ++SampleID)
{
    Sum += PerSample(ColorPixelPos, SampleID, CenterPixelInfo, DeviceZPlane, DeviceZMinMax);
}
SelectionEffect = Sum / MSAA_SAMPLE_COUNT;
```
— lines 287-293

`PerSample` returns a hard `float4(SelectionColor, 1)` or `0`. **The binary test
is never softened — it is run four times and averaged into five alpha levels.**
That is the entire antialiasing mechanism. Everything else in the shader is about
correctness, not smoothness.

Note `EditorPrimitivesDepth.GetSamplePosition(SampleID)` (line 77) being folded
into the depth reconstruction offset, guarded off for GLSL/PSSL/Metal with
`// not yet supported on OpenGL, slightly better quality`. Each sample is
evaluated at its *actual* sub-pixel position, not at the pixel centre.

### 3.3 Four things in that shader that are not about aliasing but are worth having

- **Diagonal taps, not cardinal.** Lines 136-140, with Epic's own justification:
  `// Diagonal cross is thicker than vertical/horizontal cross.` Ours uses the
  four cardinal offsets ([outline.fs.glsl:95-98](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L95-L98)).
- **The border is an ID *range* test, not an equality test.**
  `bBorder = BorderMinMax.x != BorderMinMax.y` over min/max of the four
  neighbours' stencil values (lines 143-169). This draws a border **between two
  adjacent selected objects**, which an "is any neighbour == my id" test cannot.
  The comment: `// outline even between multiple selected objects (best usability)`.
- **Occlusion is a soft fade, not a compare.**
  `fVisible = saturate(2.0f - (SceneDeviceZ - DeviceZ) / DeviceDepthFade)`
  (line 106). Ours is a hard `objectDepth > sceneDepth + kBias`
  ([outline.fs.glsl:117-118](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L117-L118)),
  which hard-switches colour along the depth boundary.
- **Scene depth is reconstructed as a plane** from four neighbour taps
  (lines 252-267) so it can be evaluated at each sample's sub-pixel offset
  without 20 texture reads — `// 4xMSAA * 5 neighbors -> 20 samples` — then
  **clamped to the local min/max** because `// this avoids flicking artifacts on
  the silhouette by limiting the depth reconstruction error`.

And one pure optimisation we should copy outright — **the scout**:

```hlsl
const int ScoutStep = 2;
const int4 StencilScout = int4( /* four ±2px stencil loads */ );
const int ScoutSum = dot(saturate(StencilScout), 1);
// If this sum is zero, none of our neighbors are selected pixels and we can skip all the heavy processing.
if (ScoutSum > 0) { ...the 4-sample loop... }
```
— lines 233-294

Four cheap integer loads gate the expensive path. **Our shader runs its full
five-tap test on every pixel of the screen** for an outline that covers a few
hundred. This is a per-frame, per-pixel cost with a trivially cheap early
rejection available, i.e. exactly the "cull cheaply before testing expensively"
rule in CLAUDE.md.

### 3.4 The approach is a decade old and unchanged

The 2015 UE4 copy of this shader has the same `Texture2DMS` declarations, the
same `Sum / MSAA_SAMPLE_COUNT`, the same diagonal-cross comment, and the same
`ReconstructDeviceZ` plane fit. **What did change is the magic constant**:
`DeviceDepthFade` was `0.00005f` in 2015 and is `0.01f` in 5.7 — a 200× shift,
because the depth range and reversed-Z convention moved under it. That is direct
evidence that **a hardcoded depth bias is a liability**, which is the exact form
our `kBias = 0.0005` takes. Source 2 avoids the class of bug entirely (§4.2).

---

## 4. Source 2 — extrude the silhouette as real geometry `[VALVE]`

`addons/base/Assets/postprocess/ObjectHighlight/ObjectHighlight.shader`, plain
text, ~180 lines. **It is not a screen-space edge detect at all.** There is no
neighbourhood, no tap pattern, no ID comparison. Two passes over the object's own
geometry:

```
DynamicCombo( D_OUTLINE_PASS, 0..1, Sys( ALL ) );
#define OUTLINE_INSIDE 0
#define OUTLINE_OUTSIDE 1
```

**Pass 0 (`OUTLINE_INSIDE`)** draws the object and stamps the stencil:

```
RenderState( StencilEnable, true );
RenderState( StencilRef, 1 );
RenderState( StencilPassOp, REPLACE );
RenderState( StencilFunc, ALWAYS );
```

**Pass 1 (`OUTLINE_OUTSIDE`)** runs a **geometry shader** that emits the triangle
once per direction around a circle, each copy nudged outward in clip space:

```hlsl
[maxvertexcount(3*7)]
void MainGs(triangle in PixelInput vertices[3], inout TriangleStream<PixelInput> triStream)
{
    const float flOutlineSize = _LineSize / 64.0f;
    const int nNumIterations = clamp( _LineSize * 10, 3, 6 ); // Thin lines don't need many iterations
    for( float i = 0; i <= nNumIterations; i += 1 )
    {
        float fCycle = i / nNumIterations;
        float2 vOffset = float2( sin( fCycle * fTwoPi ), cos( fCycle * fTwoPi ) );
        ...PositionOffset( v[i], vOffset, flOutlineSize );
    }
}
```

with the offset applied in **clip space, scaled by `w` and the aspect ratio**, so
the width is constant in screen pixels at any depth:

```hlsl
float2 vAspectRatio = normalize(g_vInvViewportSize);
input.vPositionPs.xy += (vOffsetDir * 2.0) * vAspectRatio * input.vPositionPs.w * flOutlineSize;
```

and stencil `NOT_EQUAL` masking out the interior, so the union of the smeared
copies is a ring:

```
RenderState( StencilPassOp, KEEP );
RenderState( StencilFunc, NOT_EQUAL );
```

### 4.1 Why this antialiases for free

**The outline is triangles.** It goes through the same rasteriser, into the same
target, under the same MSAA as everything else in the frame. There is no mask, no
threshold, no 1-bit anything — sub-pixel coverage is the hardware's job and it
was already doing it. Source 2 shipped MSAA-capable forward paths (CS2), so this
composes with the renderer it lives in rather than needing a private target the
way Unreal's editor does.

The shader confirms MSAA is live and is a known complication:

```hlsl
// Get the nearest depth of the object to avoid z-fighting
// This also solves disparities between MSAA samples and our Depth::GetNormalized() sample
#if D_OUTLINE_PASS == OUTLINE_INSIDE
    objectDepth += fwidth(objectDepth);
#endif
```
— lines 154-158

### 4.2 The depth bias is a derivative, not a constant

`objectDepth += fwidth(objectDepth)` is the same job as our `kBias = 0.0005` and
Unreal's `DeviceDepthFade`, done **without a magic number**. `fwidth` is the
per-pixel rate of change of depth, so the bias is automatically small on a
surface facing the camera and large on one at a grazing angle — correct at every
distance and every angle, and immune to the depth-convention drift that moved
Unreal's constant by 200× in ten years (§3.4).

**AND IT DOES NOT TRANSFER TO A SCREEN-SPACE PASS, which an earlier draft of this
note missed.** It works here because of *where it sits*: this is the object's own
forward pass, so the fragment is on the surface and the derivative is that
surface's slope. In a fullscreen pass sampling a depth texture, the same call
returns the derivative across neighbouring output pixels, which at a silhouette
is a discontinuity rather than a slope — see the warning under §6 item 2. Source
2 can use it precisely because it never left the geometry.

### 4.3 Overdraw is handled by blending manually

Because the GS emits up to seven overlapping copies, ordinary alpha blending
would compound the colour where they overlap. Source 2 writes RGB only and does
the blend itself against a copy of the frame:

```
RenderState( ColorWriteEnable0, RGB );
...
// We do custom blending here to avoid overdraw
vColor.rgb = lerp( _ColorTexture.Sample( g_sPointWrap, screenUv ).rgb, vColor.rgb, saturate( vColor.a ) );
```

Idempotent rather than accumulating: overlapping fragments compute the same
answer instead of stacking. **This is the cost of the geometry approach** — it
needs a scene-colour copy and a hand-written blend, where the screen-space
approach gets one clean fragment per pixel for free.

### 4.4 Occlusion, same idea as ours, plus a pattern

```hlsl
float diff = (worldDepth - objectDepth);
float4 vColor = lerp( _ColorMain, _ColorOccluded, diff >= 0.0f );
```

Two colours, visible and obscured — the same design as
[ScenePipeline.hpp:199-207](../../../src/cromwell/render/ScenePipeline.hpp#L199-L207).
Optionally multiplied by a **scrolling pattern texture** (`F_USE_PATTERN`,
`g_vPatternScrollRate`). Unreal does the same thing with a procedural 2×2 checker
instead of a texture (`PatternMask = ((PixelPos>>1).x + (PixelPos>>1).y) % 2 * 0.6f`,
line 174). **Both engines independently concluded that the occluded portion of a
silhouette wants a texture, not just a second flat colour** — it reads as
"behind something" rather than as a colour change.

---

## 5. The case that *does* alias, and it is ours `[EPIC]` `[COMMUNITY]`

Everything above is the **editor** outline. Unreal's *game-side* equivalent —
Custom Depth/Custom Stencil plus a post-process material — is architecturally
identical to what cromwell built, and it has exactly our problem.

`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp` allocates a
single-sample custom depth/stencil at scene resolution. There is no
`MSAA_SAMPLE_COUNT` path, no private multi-sampled target, and no per-sample
averaging anywhere downstream. A post-process material sampling
`CustomStencil` gets one binary answer per pixel, same as ours.

The only knob Epic exposes is about *stability*, not coverage:

```cpp
static TAutoConsoleVariable<int32> CVarCustomDepthTemporalAAJitter(
    TEXT("r.CustomDepthTemporalAAJitter"),
    1,
    TEXT("If disabled the Engine will remove the TemporalAA Jitter from the Custom Depth Pass. Only has effect when TemporalAA is used."),
```
— `CustomDepthRendering.cpp:24-28`, consumed at line 205 as `bRemoveTAAJitter`

With TAA on, custom depth is rendered through the **jittered** projection so it
registers with the jittered scene; but the outline drawn from it is composited
after the temporal resolve and is never itself accumulated, so the jitter shows
up as a **shimmering edge** rather than as antialiasing. Turning the cvar off
gives a stable but very slightly misregistered mask. `[COMMUNITY]` The Epic forums
carry both complaints and this cvar as the standard answer; there is no Epic-side
fix that makes the game-path outline smooth.

**The lesson is the negative one.** Epic built the good outline where it could
afford a bespoke MSAA target (the editor) and shipped the aliased one where it
could not (the game). Our situation is the second. **We are not missing an AA
pass that Unreal has — we have built the thing Unreal also failed to
antialias.** The fix has to be the editor path's, ported.

---

## 6. What applies to cromwell

Ranked by ratio of improvement to disturbance. §6.1 touches the target
allocation and the shader; items 2-4 are the shader alone. §6.0 first, because
it answers the obvious objection to copying an MSAA design.

> **Status 2026-08-16: §6.1 and §2 are implemented**, together with the bias
> reduction of item 2 (to a 1e-6 ULP guard rather than all the way to zero), and
> then **§8** on top — a per-quality-level stencil supersample (2/4/8, default 4)
> sampled on a rotated grid, which is what finally cleared the stepping §6.1 left
> on near-vertical edges. Read §8 before trusting the "five levels, same as their
> 4×" claim below: at 2× it holds for diagonals only. §8.4 is the part to check
> if the occlusion colour ever misbehaves.
> Items 3-5 are not done. The **scout early-out was deliberately left out** —
> there is no *exactly conservative* cheap version of it, since the texels the
> full test reads are precisely the ones a scout would have to check, and Epic's
> ±2 heuristic can miss thin geometry. `drawOutline` already carries a profiler
> zone, so if the pass ever shows up in a capture the scout can be added then,
> with the miss accepted knowingly. Per CLAUDE.md: measure before committing to
> an optimisation.

### 6.0 Take the counting, not the MSAA — they are separable

**The reasonable objection to §3 is that MSAA is exactly what makes outlines
render badly in Unreal.** That is true, and it is not a reason to skip the fix,
because the two halves of Epic's design come apart cleanly:

- **The bad half is the sample-count mismatch**, not the averaging. MSAA in
  Unreal breaks outlines because the object's depth/stencil has *N* samples and
  something it is compared against has *one* — and a stencil cannot be resolved
  at all, being an integer, so it must be read per-sample with `Texture2DMS` or
  not at all. Every gnarly line in Epic's shader traces to this: the
  `DeviceZPlane` plane fit exists so scene depth can be *reconstructed* at each
  sample's sub-pixel offset (`// 4xMSAA * 5 neighbors -> 20 samples`), and the
  `DeviceZMinMax` clamp exists because that reconstruction is an approximation
  that `// avoids flicking artifacts on the silhouette`. Epic wrote a plane
  solver to survive their own mismatch.
- **The good half is `Sum / MSAA_SAMPLE_COUNT`** — a binary test run N times and
  averaged. That needs N samples from *somewhere*. It does not care whether they
  came from MSAA.

**We already have N samples: the 2× supersample.** Taking the counting and
leaving the MSAA gets the coverage without importing any of the failure mode:

| | MSAA | Our 2× supersample |
|---|---|---|
| What a "sample" is | a coverage bit sharing one shaded result | **a genuine, fully shaded pixel** |
| Resolving depth | ill-defined; needs a policy | not a thing — no resolve exists |
| Resolving stencil | **impossible** (integer), must go per-sample | not a thing |
| Averaging 2×2 | needs `Texture2DMS` and sample positions | **plain arithmetic on four texels** |
| Alignment with scene depth | the mismatch (§5) | **exact** — same grid, same size |

And per §2.1 we are currently on the *worst* square of that table anyway: a 1×
custom depth compared against a 2× scene depth. **Moving the custom buffers to 2×
does not add a mismatch, it removes the one we have** — and with it the reason
`kBias` exists and the reason we would ever need Epic's plane solver.

The one thing MSAA buys that supersampling does not is cheapness — coverage
without paying to shade every sample. Irrelevant here: we already pay the 2×
across the whole scene, so the outline gets its samples for free.

### 6.1 The change itself

**Coverage by counting — the port of §3.2.** Allocate `customStencil_` /
`customDepth_` at the supersampled size instead of the surface size
([ScenePipeline.cpp:766-784](../../../src/cromwell/render/ScenePipeline.cpp#L766-L784)),
and have the outline shader run its edge test over the 2×2 block backing each
output pixel, averaging the results into alpha. **That is Unreal's
`Sum / MSAA_SAMPLE_COUNT` with supersampling standing in for MSAA** — five alpha
levels, the same as their 4×. Costs one RGBA8 + one D32F at 2× and 4× the taps in
a pass that currently costs almost nothing. Combine with the **scout** early-out
(§3.3) and the pass gets *cheaper* than it is today on the ~99% of pixels nowhere
near the outline.

Note the comment at
[ScenePipeline.cpp:766-771](../../../src/cromwell/render/ScenePipeline.cpp#L766-L771)
justifying the current 1× sizing — *"its consumer is a post-resolve pass working
in output pixels, so anything finer would be downsampled before it was read"*.
That reasoning is sound for a pass that reads one texel per output pixel, and it
is what has to change: the point of going to 2× is that the consumer stops doing
that and starts averaging four.

**2. Retire `kBias`** (§2.2). Once §6.1 lands, the two buffers agree exactly — the
same way the lit pass already agrees with the prepass under
`CompareFunc::Equal` — so the honest value is zero or one ULP, not `0.0005`. **Do
this second and measure, rather than bundling it into §6.1**: shrinking the bias
is how you *verify* the mismatch is actually gone, and leaving it at `0.0005`
would mask a botched resize.

> **DO NOT REACH FOR SOURCE 2's `fwidth` HERE — it does not transfer, and an
> earlier draft of this note recommended it without saying so.** §4.2 describes
> it accurately as *Source 2's* answer, and it is a good one **in the pass it
> lives in**: `ObjectHighlight.shader` runs it in the object's own FORWARD pass,
> where the fragment sits on the surface and `fwidth(objectDepth)` is that
> surface's screen-space slope — exactly the quantity a depth bias wants.
>
> Ours is a **fullscreen** pass sampling a depth texture. `fwidth` there is the
> derivative across adjacent OUTPUT pixels, and at a silhouette — the only place
> this pass does any work — that is a depth **cliff**, not a slope. It would
> produce an enormous bias precisely where precision is needed and read
> everything as visible. Same function, different meaning, because the shader is
> in a different place.
>
> **The screen-space equivalent is Epic's, and we already read it** (§3.3): fit a
> plane to scene depth from four neighbour taps (`DeviceZPlane`) and clamp the
> reconstruction to the local min/max. Epic's own comment says the clamp exists
> because it *"avoids flicking artifacts on the silhouette by limiting the depth
> reconstruction error"* — which is this exact discontinuity problem, handled.
> That is the tool if a non-integer resolution ratio ever makes a real bias
> necessary; today the grids are identical and there is nothing to correct.

**3. Diagonal taps and a soft occlusion fade** (§3.3). Two small edits with
Epic's stated reasoning behind each.

**4. The ID-range border test** (§3.3), if two selected units ever stand adjacent
— which is the ordinary case in a tactics game and the exact scenario
[outline.fs.glsl:27-33](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L27-L33)
says the ID edge exists to handle. The current equality test outlines their
union; `BorderMinMax.x != BorderMinMax.y` outlines each.

**5. The Source 2 architecture is the road not taken, and it is genuinely
open to us.** The RHI has a real stencil — `D24S8`/`D32FS8`
([Formats.hpp:58-70](../../../src/cromwell/rhi/Formats.hpp#L58-L70)),
`StencilState` with `write()`/`testEqual()` helpers and `setStencilReference`
([Descriptors.hpp:149-216](../../../src/cromwell/rhi/Descriptors.hpp#L149-L216)).
The "NOT A REAL STENCIL, and that is forced" paragraph in
[CustomDepthStencil.hpp:21-24](../../../src/cromwell/gpu/target/CustomDepthStencil.hpp#L21-L24)
describes the **raylib-era** class and no longer describes the RHI path.

Against it: it needs a geometry shader (GL 4.3 has them, but they are the least
loved stage on every driver), a scene-colour copy for the manual blend (§4.3),
and it would draw the outline *into* the HDR scene before the resolve — which
contradicts the argument in
[outline.fs.glsl:10-25](../../../src/cromwell/assets/shaders/rhi/post/outline.fs.glsl#L10-L25)
that an outline is interface and must not be tone-mapped. `[inferred]` **Not
recommended for the selection outline**, but worth remembering the moment
something wants a silhouette that *is* part of the picture rather than ink over
it.

### What is explicitly not the answer

**A full-screen FXAA/SMAA pass.** Neither engine uses one for this. The scene is
already supersampled and does not need it; the outline is interface ink and
post-tonemap AA would soften it against a background it was deliberately drawn
over. It would also do nothing about the 1-bit mask — it would blur a staircase
into a soft staircase.

---

## 7. Will ours shimmer? — the audit

The symptom that motivated all of this is not stair-stepping but the *other*
Unreal complaint: **an outline that crawls, flickers between its two colours, or
pops in and out as the camera moves.** Stair-stepping and shimmer are different
failures with different causes, and fixing one does not automatically fix the
other. Ours is audited against each known cause below.

### 7.1 TAA jitter — structurally impossible here

**The dominant cause in Unreal.** `r.CustomDepthTemporalAAJitter` defaults to
**1**, so the custom depth pass is rendered through the *jittered* projection
matrix — the sub-pixel offset TAA varies every frame. The mask therefore wobbles
by up to half a pixel frame to frame, and because the outline is composited
*after* the temporal resolve it is never itself accumulated, so the wobble
arrives on screen as crawl rather than as antialiasing. Thin features straddling
the jitter rasterise on some frames and not others, which is the
disappear-and-reappear.

**We have no temporal anything.** A sweep of the tree for
`jitter|halton|temporal|taa|history|reproject` returns no renderer hits — only
the PCSS disc rotation (a *spatial* per-pixel hash, and shadows are not sampled
by this pass), plus unrelated profiler and UI smoothing. ScenePipeline.cpp:100
states it directly: *"There is no MSAA … and no temporal filter."* There is no
per-frame jitter in the projection matrix to wobble the mask, so **this cause
cannot occur** — it is not mitigated, it is absent.

### 7.2 Sample-count and viewport mismatch — was present, now removed

Cause two in Unreal, and §2.1 showed we had our own version. It was worse than a
sampling offset: `beginPass` sets `glViewport(0, 0, w, h)` from the target's own
dimensions (`OpenGlRenderDevice.cpp:1546-1552`) and neither pass overrides it, so
a 1× custom depth target and a 2× scene depth target meant the **two passes
rasterised the same triangles into differently-sized viewports**. Not merely
sampled at different points — mapped to different pixel grids.

After §6.1 the two agree on all four things that decide a depth value:

| | prepass → `sceneDepth_` | custom depth pass → `customDepth_` |
|---|---|---|
| Matrix | `view.viewProjection()` | `view.viewProjection()` — the same call, no derivation |
| Viewport | `glViewport(0,0,·)` from target | same, and the targets are now the same size |
| Resolution | supersampled | **supersampled** |
| Position expression | `uViewProjection * objectTransform() * vec4(inPosition, 1.0)` | identical text, `prepass.vs.glsl:39` vs `depth_only.vs.glsl:53` |

The comment at `drawCustomDepth` is what protects the first row and is worth not
breaking: *"THE CAMERA'S OWN EYE, DERIVED ONLY FOR THE KIND. Not a new viewpoint:
the buffer's entire value is that its depth is comparable with the frame's, and a
derived matrix would silently destroy that."*

### 7.3 The one residual: cross-program invariance `[inferred]`

Everything above is structural. This one is a genuine, if small, open risk and
should not be written up as solved.

Two *different shader programs* write the two depth buffers, and GLSL guarantees
nothing about bit-identical `gl_Position` across programs without the `invariant`
qualifier — which appears **nowhere** in this tree. If a driver reassociates the
matrix chain differently in `depth_only.vs` than in `prepass.vs`, the tagged
object's depth differs slightly between the buffers.

**Why it is very probably fine.** The lit pass already tests
`CompareFunc::Equal` against the prepass depth
([ScenePipeline.cpp:1197-1199](../../../src/cromwell/render/ScenePipeline.cpp#L1197-L1199)),
which demands *bit-exact* agreement between `lit.vs` and `prepass.vs` — one ULP
of drift and lit geometry would vanish from the screen. The renderer works, so
cross-program invariance holds exactly on this driver for shaders written this
way. `depth_only.vs` computes the same expression.

**The headroom.** `kBias` is `1e-6`. For D32F depth near 1.0 an ULP is about
`6e-8`, so the guard absorbs roughly 17 ULP — ample for a few ULP of
reassociation, and 500× tighter than the `5e-4` it replaced.

**The symptom, if it ever happens**, is specific and worth being able to name: the
outline would read *occluded*-coloured on a surface that is plainly visible,
and — because floating-point error is view-dependent — it would flip as the
camera moves. **That is the shimmer, arriving by a different road.** It would not
look like aliasing; it would look like the occlusion test being wrong.

**The fix, if wanted:** `invariant gl_Position;` in the depth-writing vertex
shaders. Deliberately *not* applied yet, and the reason is that it is
all-or-nothing: `invariant` constrains how the value is computed, so declaring it
in `prepass.vs` but not `lit.vs` could make the pair diverge where today they
agree — turning a theoretical risk into a real regression in the `Equal` test the
whole image depends on. It is a coordinated change across every vertex shader
writing a cross-compared depth, worth doing deliberately rather than as a
by-product of an outline fix.

### 7.4 Sub-pixel geometry — reduced fourfold, not eliminated

A feature thinner than a stencil texel (a rifle barrel, an antenna) can rasterise
on one frame and not the next as the camera moves, and its outline pops. This is
inherent to any mask-based outline and neither engine escapes it.

What changed is the odds: each output pixel now gets **four** chances to catch the
feature instead of one, and a caught-in-one-of-four sample produces alpha 0.25
rather than a hard on/off. So a feature at the resolution limit now fades rather
than blinks. That is the same standing Unreal's 4× MSAA editor path has, and
strictly better than the single-sample game path we previously matched.

---

## 8. The residual staircase: ordered grid vs rotated grid

**Found by looking at the result.** §6.1 removed most of the stepping, but a
near-vertical silhouette still showed it. That is not a bug in the averaging —
it is the sampling pattern, and it is the one place where "supersampling stands
in for MSAA" (§6.0) is not quite true.

### 8.1 Why four samples is only three levels on a vertical edge

A plain 2×2 supersample is an **ordered grid**. Its four texels sit at sample
positions (0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75) — **two distinct
x values and two distinct y values.** A near-vertical edge is resolved by x
alone, so coverage can only be 0, ½ or 1. Three levels. Near-horizontal edges
have the same problem in y. Only a diagonal, which uses both axes, sees all five.

**This is exactly what rotated-grid sampling exists to fix, and it is why MSAA
gets away with four samples.** 4× MSAA's positions are deliberately not
axis-aligned, so they give four distinct x *and* four distinct y — five levels on
near-vertical and near-horizontal edges alike, which are the common cases in a
built environment. Unreal's editor outline inherits that for free from the
hardware pattern (§3.1). **We cannot rotate**: our samples are texels of a
supersampled buffer and they sit where the rasteriser put them.

So the earlier claim that our 2×2 reaches "five alpha levels, the same as their
4×" holds only for diagonals. On the axis-aligned edges that dominate a tile
game it is three against five, and the difference is visible because an outline
is high contrast. The scene's own geometry is quantised identically — it just
does not show, because a blue box against a grey floor has nowhere near the
contrast of yellow ink.

### 8.2 The wider filter that does not work, and the strip that half did

**Sampling a wider footprint is the obvious move and it ruins the line.** A 4×4
block of the same buffer is a two-pixel box filter, and the outline is two pixels
thick: convolving a 2px band with a 2px box gives a 4px triangular profile with
peak opacity only at the centre. A glow, not an outline.

**Attempted and rejected: an edge-oriented strip.** The reasoning was that the
profile *across* the edge is already correct, and the staircase is really the
edge drifting sideways while coverage steps in halves — an error *along* the
edge. So sample a strip: two columns by four rows for a near-vertical edge,
extended one texel each way along the edge only, oriented by a four-tap probe.
No across-edge softening, and in principle nine levels from the same two columns.

**It did not fix it, and the reason is worth keeping.** The probe tested for the
object at the same `reach` the band test uses, so at the *outer boundary of the
band* — precisely where the gradation matters — the probe is exactly as marginal
as the test it is steering. It answered "no direction" there, the shader fell
back to the plain block, and that boundary kept its three levels. A heuristic
that degrades exactly where the thing it is fixing lives is not a fix. Removed.

### 8.3 What worked: a finer buffer, and a rotated grid on it

The information is not in a 2× buffer, so the buffer got finer — on **its own
dial**, `ScenePipeline::withOutlineSupersample`, independent of the scene's fixed
2×. An outline is saturated ink and the eye reads a step in it that it would
never notice on a wall, so the silhouette earns finer sampling than the picture
does; tying the two together would mean paying for the whole scene at 4× to fix a
line two pixels wide.

**Four positions per axis is the threshold that matters**, because four is where
a rotated pattern becomes possible: one tap per column, each in a different row,
giving as many distinct x as y. That is exactly what MSAA's sample positions buy
and what an ordered grid throws away.

**And the sample count deliberately does not follow the buffer.** Reading all
sixteen texels of a 4×4 block costs four times as much and still yields five
levels, because the extra taps land in columns already counted. `samples` taps in
a rotated permutation give `samples + 1` levels for the price of the taps alone.
The stride must be coprime with the width or the walk revisits a column before
covering them all; half the width plus one is coprime for every power of two in
range. **Raising the dial buys positions, not work — memory is what it spends.**

### 8.4 The dial must not reintroduce §2.1, and this is how it doesn't

A stencil finer than `sceneDepth_` puts the occlusion test back into the
mismatch this whole note is about: at 4× against a 2× depth, sampling both at the
same position reads points up to **an eighth of an output pixel apart** — the
same class as the original quarter-pixel defect, and at grazing angles the same
visible/occluded flip. **Finer coverage must not be bought with a fuzzier depth
test.** Three things prevent it:

1. **The factor is floored at the scene's.** A *coarser* stencil cannot be
   aligned by any means, so the cheapest setting is parity — already correct
   rather than merely cheap. Allowed values are 2, 4, 8.
2. **The occlusion test drops to the coarser grid.** The stencil is an integer
   multiple of the scene depth, so a stencil texel lies wholly inside one scene
   texel and the mapping is an integer divide — nothing that can round
   differently from one frame to the next.
3. **The object's depth is reduced over that footprint, not point sampled**, so
   both sides of the comparison describe the same area of screen. The reduction
   is a `min` — the object's nearest surface there — and four corners suffice,
   since the extremes of a plane over a square are at its corners and a depth
   buffer is planes almost everywhere.

Where it is ambiguous — a silhouette crossing the footprint — the min errs
towards *visible*, drawing the main colour rather than the x-ray one. A wrong
colour in the safe direction, and being derived from an integer mapping, a
**stable** one. That is the property that matters: a flicker is far worse than a
bias. At parity the block is one texel and the arithmetic reduces to the exact
comparison it replaced, so the default path is unchanged and `kBias` stays at the
1e-6 ULP guard.

### 8.5 What it costs

| | buffer | samples | across-edge softening | levels, near-vertical |
|---|---|---|---|---|
| before §6.1 | 1× | 1 | none | 2 (a 1-bit mask) |
| §6.1 | 2× | 4 | none | 3 |
| 4×4 box filter | 2× | 16 | **~2px — a glow** | 5 |
| strip (rejected, §8.2) | 2× | 8 | none | 3 at the band's edge |
| **rotated grid** | **4×** | **4** | **none** | **5** |
| rotated grid | 8× | 8 | none | 9 |

Memory at 1280×800, for the RGBA8 stencil and D32F depth together: **~33 MB at
2×, ~131 MB at 4×, ~524 MB at 8×.** Fill cost barely moves — the pass draws only
tagged objects, a soldier or two — so this dial is almost purely a memory trade,
which is why it belongs in settings next to texture resolution rather than being
picked once by the engine. `--outline-ss <n>` drives it for A/B comparison; a
stored preference belongs in the user settings bag, not argv.

---

## 9. The corner gap, and why a cross is the wrong structuring element

**Found by looking at the result, again, and it predates everything above.** With
the edges finally smooth, the outline showed a square notch at every convex
corner — the segments visibly failing to meet.

### 9.1 The cross can never draw a corner

The band is a **dilation**: a texel draws when the object lies `reach` away in one
of the tested directions. So the tap set *is* the structuring element, and the
cardinal cross `{(±r,0), (0,±r)}` has a blind spot that is not subtle.

Take an object filling the quadrant `x ≤ 0, y ≤ 0`, corner at the origin, and a
texel at `(a, b)` with `a, b > 0` — diagonally outside the corner. The horizontal
taps land at `(a ± r, b)`, and `b > 0` puts both above the object. The vertical
taps land at `(a, b ± r)`, and `a > 0` puts both beside it. **No cardinal tap can
ever hit**, so an `r × r` square at every convex corner is unreachable. At
`reach` = 8 texels that is a two-pixel notch, per corner.

The original shader argued the opposite in a comment — diagonals *"only matter at
a corner sharper than the thickness, and a body is a box, so the four cardinal
ones give a uniform border"*. **A box is nothing but corners**, and the border is
not uniform: for an edge with unit normal **n**, a tap set S extends the band by
`max(s · n)`, which for the cardinal cross is `r · max(|nx|, |ny|)` — `r` on an
axis-aligned edge, `0.707r` at 45°, and **zero** in the corner's diagonal
quadrant, which is no edge at all.

It survived because at one sample per pixel the whole silhouette was ragged and a
two-pixel notch read as more of the same. **Antialiasing everything around it is
what made the hole legible** — the recurring shape of this whole note.

### 9.2 An octagon, with the diagonals pulled in by √2

Adding the four diagonal taps fills the corner, but at full `reach` they overshoot:
`max(s · n)` for `{(±r, ±r)}` is `r(|nx| + |ny|)`, which is `1.41r` at 45°. That
is Epic's *"diagonal cross is thicker than vertical/horizontal cross"* (§3.3)
seen from the other side — they accepted the thickening; we do not have to.

Pulling the diagonals in to `r/√2` equalises it. Extent becomes `r` at 0°, 45° and
90° alike — an octagon inscribing the disc the band actually wants, within about
4% at the half-angles. Corners come out **rounded at radius r**, which is what a
band of uniform width around a shape genuinely is.

| tap set | axis-aligned | 45° edge | convex corner |
|---|---|---|---|
| cardinal `{(±r,0),(0,±r)}` | r | 0.707r | **nothing — the gap** |
| diagonal `{(±r,±r)}` | r | 1.41r | r |
| **octagon, diagonals at r/√2** | **r** | **r** | **r, rounded** |

Cardinals are tested first and the chain short-circuits, so a straight edge still
costs four taps and only the corners pay for the other four.

---

## Sources

**Read from disk, 2026-08-16:**

- `C:/Program Files/Epic Games/UE_5.7/Engine/Shaders/Private/PostProcessSelectionOutline.usf`
- `C:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp` (`GetEditorPrimitiveNumSamples`, line 193)
- `C:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:144`
- `C:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Private/HAL/ConsoleManager.cpp:4115` (`r.MSAA.CompositingSampleCount`, default 4)
- `C:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:24` (`r.CustomDepthTemporalAAJitter`)
- `E:/SteamLibrary/steamapps/common/sbox/addons/base/Assets/postprocess/ObjectHighlight/ObjectHighlight.shader`

**Fetched:** UE 4.x `PostProcessSelectionOutline.usf`, public mirror
[johndpope/UE4](https://github.com/johndpope/UE4/blob/master/Engine/Shaders/PostProcessCompositeEditorPrimitives.usf)
— §3.4 only, for the decade-long comparison.

`[COMMUNITY]` Epic Developer Community threads on custom-depth outline jitter:
[Fix for Outline PostProcess Jitter](https://forums.unrealengine.com/t/fix-for-outline-postprocess-jitter/354546),
[CustomDepthTemporalAAJitter Does Not Work](https://forums.unrealengine.com/t/customdepthtemporalaajitter-does-not-work/396402).
