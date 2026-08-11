# UNIGINE 2 volumetric clouds — reference notes

Unlike every other study in this folder, **none of this is inferred from a talk,
a capture or a disassembly.** UNIGINE ships the whole cloud system as readable
shader source inside the SDK, the same way it ships its water. Every constant,
threshold and loop bound below is quoted from a file you can open. Where I say
something the source does not state outright, it is tagged **[inferred]**.

That makes this the most useful companion to
[`rdr2_atmospherics.md`](rdr2_atmospherics.md): RDR2's talk describes a
1-ray-in-4 reconstructed cloud raymarch in prose and slides, and UNIGINE ships a
working one. Where the two disagree, the readable one wins the argument about
what the code actually has to do.

Read against [`rdr2_atmospherics.md`](rdr2_atmospherics.md) §9 before acting on
any of it — that section's conclusion (a bounded 24 × 24 board does not need a
cloud raymarch) still holds, and §10 here does not overturn it.

> **The other UNIGINE study is filed elsewhere.** Its ocean is
> [`sea_of_thieves_water.md`](sea_of_thieves_water.md) **§11** — Gerstner waves
> rather than FFT, the tessellation and deferred composite, the subsurface
> function, and (§11.9) the decoded texture set. It lives under a Sea of
> Thieves name because it was written as the third point of comparison there,
> not as a UNIGINE document.

---

## 0. Where it is on this machine

The SDK Browser installs SDKs under `%LOCALAPPDATA%`, not Program Files, which
is why it is easy to lose:

```
C:\Users\JMasc\AppData\Local\unigine\browser\sdks\community_windows_2.17.0.1_bin
```

The install list lives in `%APPDATA%\unigine\browser.json` under `sdk.installed`
if that path ever changes. Art and shaders are unpacked under `data\core\`; a
*project* instead gets them packed into `data\core.ung`, so always read the SDK
copy, not a project's.

Everything the cloud system is made of, relative to `data\core\`:

| path | what |
|---|---|
| `materials/base/objects/clouds/shaders/common.h` | `CloudData` (the per-layer parameter block), coverage sampling, inter-layer shadows |
| `materials/base/objects/clouds/shaders/geometry.h` | **the density function** and all three sky-intersection forms |
| `materials/base/objects/clouds/shaders/shading.h` | phase function, sun term, ambient term |
| `materials/base/objects/clouds/compositor/compositor.comp` | **the raymarch** — 600 lines, the heart of it |
| `materials/base/objects/clouds/reprojection/reprojection.comp` | temporal reconstruction of the interleaved frames |
| `materials/base/objects/clouds/intersection/intersection.comp` | per-pixel nearest layer-entry distance |
| `materials/base/objects/clouds/copy/scene_copy.frag` | depth-aware bicubic upsample to full res |
| `materials/base/objects/clouds/clouds_base.basemat` | every artist parameter, with UNIGINE's own tooltips |
| `materials/base/objects/clouds/presets/*.mat` | 12 calibrated presets, cirrus → cumulonimbus |
| `textures/clouds/` | the noise, coverage and shape textures |

The `.basemat` files are worth reading purely for the tooltips — they are the
only place the *intent* of a parameter is written down.

### Reading the textures

`.texture` is UNIGINE's own container and nothing third-party opens it, so
[`../tools/unigine/unigine_texture.py`](../../../tools/unigine/unigine_texture.py) decodes it. Format
notes are in that file's docstring.

```powershell
.\tools\unigine\unigine_extract.ps1 -Info      # header table, decodes nothing
.\tools\unigine\unigine_extract.ps1            # clouds + water -> unigine_extracted/
```

Output is gitignored. **UNIGINE's art is licensed for use in UNIGINE projects**,
so the point of decoding it is to see what each channel holds and then generate
our own — the same rule
[`README.md`](../../README.md) already applies to XCOM's `MovementBorder_Line`.

---

## 1. The shape of the system

Eight passes. Only one is expensive.

| pass | shader | writes |
|---|---|---|
| curved frustum | `render_clouds_curved_frustum.basemat` | `RGBA32F` volume: per-froxel offset from flat space to the ellipsoid. Geodetic worlds only |
| 3D atlas copy | `render_clouds_3d_atlas_copy.basemat` | packs each artist-supplied cloud shape volume into one shared `R8` atlas along Z |
| dynamic coverage | `dynamic_coverage.frag` | camera-centred coverage texture array — brush quads painted by `FieldWeather` objects |
| intersection | `intersection.comp` | `R32F` nearest layer-entry distance per pixel |
| depth downsample | `render_clouds_depth_downsample.basemat` | low-res scene depth, `min`/`max` of a `TEXTURE_GATHER` quad, optionally checkerboarded |
| **compositor** | **`compositor.comp`** | **`RGBA16F` cloud colour, optional `RG32F` depth. This is the raymarch** |
| reprojection | `reprojection.comp` | reconstructs the full-res buffer from this frame's interleaved subset plus last frame's |
| copy | `scene_copy.frag` | depth-aware bicubic upsample, composited into the scene |

The interesting structural choice is that **clouds are not a mesh**. There is no
proxy geometry, no sky dome; `compositor.comp` is a compute dispatch over
screen tiles (`MAIN_COMPUTE_BEGIN(8, 8, 1)`) that computes its own ray from
`screenUVToViewDirection(uv)` and its own bounds by intersecting analytic
layer altitudes. Layers are a `StructuredBuffer<CloudData>`, not draw calls.

---

## 2. What a cloud is made of — the density function

`geometry.h:351`, `sampleCloudDensity()`. This is the part worth understanding
even if nothing else here is ever used; it is the Schneider/Häkkinen
*coverage × height gradient × progressive noise erosion* model, implemented in
full with every knob exposed.

The sample point is `float4`: `xyz` in cloud space, **`w` is normalised height
within the layer** (`getCloudHeight()`, `geometry.h:317` — an affine remap from
`layer_height`). Almost every parameter below can be a *curve in `w`* rather
than a constant, which is what the `TEXTURE_RAMP` macro fetches:

```c
#define TEXTURE_RAMP(NAME, COORD, LAYER_ID) \
    saturate(TEXTURE_BIAS_ZERO(TEX_CURVE_ATLAS, float3(COORD, 1.0f, LAYER_ID + NAME * CLOUDS_NUM_LAYERS)))
```

All the artist curves for all layers live in one texture array, indexed by
`curve_id * num_layers + layer_id`. Cheap, and it means "constant" and "curve"
are the same code path with a `state_mask` bit choosing between them.

### 2.1 Coverage — a 2D map, four channels

`common.h:187`. One `RGBA` fetch defines the whole horizontal structure:

| channel | meaning |
|---|---|
| **R** | coverage — how much cloud is here at all. Raised to `coverage_contrast` |
| **G** | stormcloud mask — drives darkening, extra height, reduced detail erosion |
| **B** | cloud *type* / height profile — remapped by `coverage_height_remap` |
| **A** | an optional second tiling-break mask, sampled at a different scale |

Decoding the shipped coverage textures confirms this exactly: `G` is
effectively empty for `clouds_coverage_cumulus` (max **4**/255) and
`clouds_coverage_stratus` (max **1**), but has mean **174** for
`clouds_coverage_nimbostratus` — the rain cloud.

### 2.2 The height gradient — three profiles, blended by the B channel

```c
#define SG_CLOUD_NOISE_0 float4(0.010f, 0.099f, 0.125f, 0.225f)   // thin, low
#define SG_CLOUD_NOISE_1 float4(0.0f,   0.095f, 0.31f,  0.505f)   // mid
#define SG_CLOUD_NOISE_2 float4(0.0f,   0.085f, 0.75f,  1.0f)     // tall

coverage_height_gradient = gradient4(sample_point.w, lerp3(SG_CLOUD_NOISE_0, SG_CLOUD_NOISE_1, SG_CLOUD_NOISE_2, coverage.b));
```

Four control points describing a trapezoid in normalised height — fade in, hold,
fade out — with three presets that the coverage map's blue channel blends
between. This is how one layer holds stratus *and* cumulus at different map
positions. Storm pushes it taller first: `coverage.b = lerpOne(coverage.b, coverage.g * 0.6f)`.

**The early-out is here, and it matters:**

```c
if (coverage_height_gradient * coverage.r < EPSILON)
    return info;                     // no 3D texture fetches at all
```

Two 2D fetches reject most of the volume before any 3D noise is touched.

### 2.3 Base noise — erosion, not addition

```c
low_noises  = TEXTURE_BIAS(TEX_BASE, sample_point.xyz * base_size + animation, mip);
noise       = dot(low_noises * TEXTURE_RAMP(CLOUDS_NOISE_FALLOFF, w, layer), 0.25f) * coverage_height_gradient;
noise       = smoothstep(noise_threshold, noise_threshold + noise_threshold_extent, noise);
noise       = saturate(noise - 1.0f + coverage.r) * coverage.r;
```

Four octaves live in the four channels of one `RGBA` volume and are *weighted
per-channel by a height curve* before being averaged — so an artist can say
"only the coarsest octave near the cloud base". The last line is the standard
`remap(noise, 1-coverage, 1, 0, 1)` erosion: coverage does not add cloud, it
decides how much noise survives.

### 2.4 Detail and curl — the wispy edges

```c
noise_detail_uvw = p * detail_size * 0.001f
                 + (TEXTURE_BIAS(TEX_CURL, dist_uv, mip).xyz * 2.0f - 1.0f) * cloud_distortion * sample_point.w;
detail_modifier  = saturate(lerp(detail_noise, 1.0 - detail_noise, saturate(w * detail_wispy_billowy_gradient)));
noise           -= smoothstep(1.0f, 0.0f, noise) * 0.5f * detail_modifier * detail_affect * (1.0f - coverage.g);
```

Three things to note:

- **The curl distortion is scaled by height (`* sample_point.w`)**, so cloud
  tops shear and bases stay put — one multiply for most of the "wind-blown" read.
- **`lerp(d, 1-d, ...)` is the wispy/billowy switch.** Inverting the detail
  noise turns cauliflower into wisps; the gradient does it with altitude.
- **`(1.0f - coverage.g)` disables detail erosion inside storms** — storm clouds
  stay solid-edged.

`smoothstep(1, 0, noise)` gates all erosion to the *low-density* end, so cores
are never eroded. Same trick is used for the optional extra noise octave.

### 2.5 Wind is three separate effects

| parameter | what it does | where |
|---|---|---|
| `s_noise_animation_offset` | scrolls the noise lookups | `geometry.h:404` |
| `s_coverage_animation_offset` | scrolls the coverage map independently | `common.h:192` |
| `wind_skew_intensity` | offsets the *coverage* lookup by `pow(w, wind_skew_height_gradient)` × wind direction | `geometry.h:376` |

The third is the good one: skewing the coverage fetch by height leans the whole
column downwind, so a cloud's top sits offset from its base without any extra
sampling. `wind_deformation` chooses whether the noise animates with the wind
(clouds boiling) or is dragged rigidly (clouds sliding).

---

## 3. The textures, decoded

Run through [`../tools/unigine/unigine_texture.py`](../../../tools/unigine/unigine_texture.py):

| texture | dims | format | holds |
|---|---|---|---|
| `cloud_noise` | 128³ | RGBA8, 8 mips | 4 noise octaves, one per channel |
| `cloud_noise_detail` | 32³ | RGBA8, 1 mip | detail noise, 3 channels averaged in shader |
| `curl_noise_2d` | 128² | RGBA8, 4 mips | curl field, remapped `*2-1` |
| `curl_noise_3d` | 128×128×16 | RGBA8 | 3D variant, `DISTORTION_3D_TEXTURE` |
| `clouds_coverage_*` | 512²–1024² | DXT1 / DXT5 | 7 types; R/G/B/A as §2.1 |
| `default_coverage` | 512² | RGBA8, 10 mips | the fallback, uncompressed |
| `cumulonimbus{1,2,3}_shape` | 256³ | BC4 | authored single-cloud volumes |
| `clouds_shadows_cumulonimbus*` | 512² | BC4, 10 mips | matching shadow maps for the above |

Two things worth taking from this table alone:

- **The base noise is only 128³ and the detail only 32³.** Both are RGBA8, so
  9.6 MB and 128 KB. The fidelity comes from the erosion model, not resolution.
- **A 3D-texture cloud needs a *separate* shadow map** (`clouds_shadow_texture`
  in the basemat) because the inter-layer shadow path in §7 only reads a 2D
  coverage channel and a volume has no equivalent.

The container itself: 48-byte header, then per mip a `u64` size and either raw
data or a **raw LZ4 block** — compression is opportunistic per mip, so `foam_d`
stores BC4 byte-for-byte while a mostly-empty shadow map packs 131072 → 17624.
Volumes are a stack of independently block-compressed 2D slices, not 3D blocks.

---

## 4. Bounds — three world shapes

`geometry.h` carries three intersection routines and the compositor picks one by
`#define`:

| mode | routine | notes |
|---|---|---|
| flat | `getSkyIntersection` (`:253`) | plane intersections. Cheapest |
| `ROUNDED` | `getSphereSkyIntersectionSmoothed` (`:226`) | ray/sphere against `s_planet_radius`, so the layer curves below the horizon |
| `GEODETIC` | `getCurvedSkyIntersection` (`:115`) | **`double` precision**, real ellipsoid, plus the frustum-displacement volume |

The `Smoothed` wrapper (`:226`) is the detail worth stealing. Flying through a
cloud layer's boundary makes ray bounds change discontinuously and the layer
pops. The fix:

```c
clamped_camera_position.z = clamp(z, altitudes.x + margins, altitudes.y - margins);  // margins = 20 units
// solve twice — once at the real camera height, once at the clamped one —
// then lerp the two bound pairs over lerp_distance
bounds = lerp(clamped_bounds, real_bounds, factor);
```

Solve the intersection twice and cross-fade the *bounds*, not the result.
`lerp_distance` is `min(500, layer_thickness / 2.5)`, the comment noting the
divisor exists "to avoid thin clouds disappearing".

There is a second anti-popping hack at `compositor.comp:296`. Near the horizon
the far bound changes wildly between neighbouring pixels, so when
`bounds.x < bounds.y * 0.1f` it re-solves along a **blue-noise-jittered view
direction** and takes the jittered far bound if it differs by more than the
bound itself. Noise instead of a hard edge, resolved later by the temporal pass.

---

## 5. The raymarch

`compositor.comp:368`. The loop condition alone says a lot:

```c
for (float iteration = 1; iteration <= num_iterations * iterations_reserve_factor &&
        (alpha_trail * depth_test_alpha) > EPSILON &&   // opacity exhausted
        traced_space < bounds.y &&                      // left the layer
        out_color.a < EARLY_ALPHA_THRESHOLD &&          // quality-preset cutoff
        visibility > 0.0001f                            // lost behind haze
        ; iteration++)
```

`max_iterations` is `128 * blue_noise * SAMPLES_COUNT_MODIFER` — so **128 steps
at "high"**, 32 at low, 320 at extreme.

### 5.1 The step schedule

Three step sizes are in play and the code picks between them per iteration:

| step | meaning |
|---|---|
| `search_step` | coarse, used while outside cloud. Lerped from a *near* to a *far* value across the march |
| `dense_step` | fine, used inside cloud |
| `skip_transparent` | an **extra** jump added when a sample comes back empty |

The near/far split (`:375`) is the cheap trick: `far_factor` ramps 0→1 between
40 % and 50 % of the iteration budget, and the far step is *recomputed at that
point* from the distance actually remaining:

```c
far_search_step = lerpFixed(16.0f, 1.0f, s_step_accuracy) * abs(bounds.y - traced_space) / max(num_iterations - iteration - 16, 1);
```

So a ray that is half way through its budget adapts its remaining step size to
what is left, rather than committing to a schedule up front. There is a hard
ceiling of `iteration * iteration * max_step`, which keeps the first few steps
small regardless.

### 5.2 The hint

`sampleCloudDensity` returns a `hint` alongside density (`CloudSampleInfo`), a
cheap 0–1 "am I near cloud" computed from coverage before the expensive fetches:

```c
info.hint = lerpFixed(0.0f, 0.8f, pow(coverage.r * coverage_height_gradient, 0.5f));
// ... and after the base noise, if already > 0.5:
info.hint += lerpFixed(0.0f, 0.2f, pow(noise, 0.5f));
```

It is used two ways: `step_val = lerp(search_step, dense_step, step_hint)` —
a *continuous* blend rather than an in/out branch — and it gates the empty-space
skip (`if (step_hint <= 0.5f) traced_space += skip_transparent`). A sample that
found nothing but is *near* something does not get the extra jump.

### 5.3 Back up and re-march

The one genuinely unusual idea here (`:451`), and it transfers to any raymarcher:

```c
if (sampled_cloud.density > lerp(0.15f, 0.5f, saturate(lod)) && refine_steps <= refine_disabled_steps && refine_limit > 0)
{
    refine_steps = toInt(min(toInt(1.5 * prev_step_val / dense_step), (num_iterations - iteration - 8)));
    traced_space -= (refine_steps + 0.5) * dense_step;   // rewind
    traced_space  = max(bounds.x, traced_space);
    --refine_limit;
    continue;
}
```

When a coarse step lands *deep inside* cloud, the march has overshot the
silhouette. Rather than accept a hard edge, it **rewinds** by roughly the
distance it jumped and re-marches at the fine step, spending enough of the
remaining budget to cover the overshoot. `refine_limit = 1` — once per ray, so
the cost is bounded. `refine_steps` counts back down to re-arm.

This is what stops adaptive stepping from cutting flat facets into cloud edges,
which is the usual failure mode of "big steps outside, small steps inside".

### 5.4 Distance LOD

```c
lod = max(0.0f, traced_space - 5000.0f) / 5000.0f;
```

A mip bias, not a step change: one mip per 5 km, passed into every texture
fetch, and *also* used to raise the refine threshold with distance. Sampling
rate and detail level fall off together.

---

## 6. Lighting

### 6.1 Occlusion — a fixed cone, not a march

`compositor.comp:493`. The classic 6-tap cone toward the sun, but the offsets
are **uniforms, not computed**: `s_lighting_samples[24]`, six per quality
preset, indexed by `SAMPLES_OFFSET`. The shader just adds an offset and
re-samples density:

```c
sample_point_shadow = sample_point.xyz + lighting_sample;
lighting_occlusion += sampleCloudDensity(sample_point_shadow, lod, layer, animation_offset).density;
```

At ultra the *previous* sample position is blended in by blue noise
(`lerp(old_lighting_samples, s_lighting_samples[...], lighting_noise)`), which
jitters the cone between frames for the temporal pass to resolve. At low quality
there is a single sample, scaled by noise.

Note the cost model: each lighting tap is a **full density evaluation**, so
LIGHTING_QUALITY_ULTRA makes each marched sample ~7× more expensive.

### 6.2 Phase — two HG lobes

`shading.h:66`:

```c
float getHenyeyGreensteinPhaseMultiscattering(float cos_angle, float anisotropy, float w0, float w1)
{
    return w0 * getHenyeyGreensteinPhase(cos_angle, anisotropy)
         + w1 * getHenyeyGreensteinPhase(cos_angle, (2.0f / 3.0f) * anisotropy);
}
```

Weights are hardcoded `1.0` and `0.5` and commented "artist handpicked". The
second lobe uses ⅔ of the anisotropy — the same decreasing-`g` octave stack RDR2
uses ([`rdr2_atmospherics.md`](rdr2_atmospherics.md) §3.1), with two octaves
instead of three and no back-scatter floor.

**`g` itself is a curve in density**, not a constant:
`TEXTURE_RAMP(CLOUDS_ANISOTROPY_BY_DENSITY, density, layer) * 2 - 1`, so it
spans −1…1 and dense cores can scatter differently from wisps.

### 6.3 Transmittance — Beer plus a cheap multiscatter term

```c
density_along_light_ray = sun_attenuation * lighting_occlusion;
lighting *= exp(-density_along_light_ray)
          + (exp(-density_along_light_ray * 0.25) * multiscattering_intensity * (1.0 - saturate(dotVL)));
```

Beer's law, plus a second much slower exponential (¼ the extinction) standing in
for multiple scattering — weighted by `(1 - saturate(dotVL))` so it only appears
away from the sun, which is where single scattering under-reads. Default
`multiscattering_intensity` is 0.35, `attenuation_coefficient` 0.08.

### 6.4 Ambient — the sky cube, mip-selected by distance

`shading.h:36`. This is the cheapest good idea in the file:

```c
// view_color depends on distance factor:  near: 8 mipmap  far: 0 mipmap
float3 view_color = TEXTURE_BIAS(TEX_SKY_CUBE, view_direction, (1.0f - distance_shading_factor) * 8.0f).rgb;
float3 result_color = lerp(env_color, view_color, saturate(distance_shading_factor * getCloudHaze()));
```

Near clouds get an 8-mip (nearly uniform) sky sample; distant clouds get mip 0,
the sharp sky in that direction. One fetch delivers "distant clouds take on the
colour of the sky behind them" with no extra pass. `env_color` is the
top/bottom ambient pair blended by `pow(height, 0.45)`, then modulated by two
AO curves keyed on coverage.

The `top_color` it starts from is five taps of the sky cube at mip 8 — up plus
four horizontals, averaged (`compositor.comp:237`).

### 6.5 Haze

Recomputed **only every 5 km along the ray** (`:560`):

```c
if (abs(haze_color_distance - traced_space) > 5000.0f) { ... }
```

The transmittance `physicalHazeVisibility()` is evaluated per step, but the
expensive *coloured* term is not. `visibility` also terminates the loop once
haze has swallowed the ray.

### 6.6 The composite maths

Front-to-back, premultiplied:

```c
alpha        = (1.0 - exp(-density * attenuation_coefficient * step_val)) * alpha_trail + EPSILON;
alpha_trail -= alpha;
out_color.a += alpha;
out_color.rgb += ambient_term * alpha + sun_term * alpha;
```

then, after the loop:

```c
out_color.rgb /= (out_color.a + EPSILON);          // un-premultiply to an average colour
out_color.a    = saturate(out_color.a / EARLY_ALPHA_THRESHOLD);   // rescale for the early cutoff
out_color.a    = pow(out_color.a, 1.5f);           // "Make clouds softer"
out_color.rgb *= out_color.a;                      // re-premultiply
```

The alpha rescale is the load-bearing part: because the loop exits at
`EARLY_ALPHA_THRESHOLD` (0.5 at low quality!) the accumulated alpha is divided
by that same threshold so the cloud still reaches full opacity. The quality
preset changes how early you stop, not how opaque the result is. The `pow(a, 1.5)`
is an unexplained softening curve — a look choice, not physics **[inferred]**.

---

## 7. Layers, and shadows between them

`CLOUDS_NUM_LAYERS` layers, each a `CloudData` in a structured buffer, each with
its own altitude band, textures and full parameter set. Two traversal modes:

- **default** — an outer loop over `s_clouds_order[]` (sorted CPU-side), each
  layer marched separately with its own bounds. Blending is therefore in sorted
  order and correct only if the layers do not interpenetrate.
- **`ACCURATE_SORT`** — one march over the union of all bounds;
  `sampleCloudDensityAllLayers()` (`geometry.h:517`) evaluates *every* layer at
  each step and takes the **maximum** density. Correct for overlapping layers,
  and much more expensive.

Both modes pay for unrolling the layer loop, and the source says so in two
places. The default path only unrolls at `CLOUDS_NUM_LAYERS <= 3`, commented
*"too long compile time (more than minute) with more layers"*
(`compositor.comp:272`). The accurate path unrolls unconditionally and simply
silences the resulting D3D11 warning — *"Warning apears with many layers because
of unroll / It's still better to unroll"*, `##pragma warning(disable : 4714)`,
`geometry.h:526`.

**Inter-layer shadows are not a shadow map.** `common.h:259`:

```c
float3 shadow_sample_point = getShadowSamplePoint(sample_point, data, sun_ray_w);   // ray vs the caster's MID-PLANE
float4 coverage = sampleCoverage(shadow_sample_point, 0.0f, data, ...);
float layer_shadow = saturate(1.0f - (coverage.r * (1.0f + coverage.g * 0.3f)));
```

For each *other* layer, intersect the sun ray with that layer's mid-plane, take
one 2D coverage sample, and turn it into an attenuation. A whole cloud layer is
treated as an infinitely thin sheet for shadowing purposes. Then:

- fade the shadow out toward the caster's own altitude (`height_coef`), so a
  layer does not shadow itself at its boundary;
- fade the whole thing out at low sun (`remap(sun_ray.z, 0.05, 0.15, 0, 1)`),
  because the mid-plane approximation degenerates at grazing angles.

Cost is one texture fetch per layer per step, and the result is the big soft
banding of high cloud over low cloud, which is most of what you actually see.

---

## 8. Making it affordable

Three cooperating tricks, and this is the section that matters most for anything
else this project raymarches.

### 8.1 Interleaved rendering — one pixel in N×N per frame

`compositor.comp:195`:

```c
float ray_placement_noise = TEXTURE_ARRAY_FETCH(TEX_NOISE, fmod(screen_coord, 256), 0).x;
const uint frame_selector = uint(s_frame + toInt(min(ray_placement_noise * interleaved_frame_count, ...))) % interleaved_frame_count;
screen_coord = screen_coord * INTERLEAVED_SIZE + s_interleaved_samples[frame_selector];
```

The dispatch is over the *reduced* grid, and each thread picks which pixel of
its N×N block to trace. Note the ray placement is **jittered per pixel by blue
noise**, not a global frame counter — neighbouring blocks trace different
positions in the same frame, so the pattern never reads as a moving grid. RDR2
§5.2 describes arriving at the same conclusion after two failed attempts; here
it is three lines.

### 8.2 Reprojection

`reprojection.comp`. Per pixel:

1. Reconstruct world position from the **cloud** depth buffer, not the scene's:
   `normalize(screenUVToViewDirection(uv)) * depth`, then `getStaticVelocity()`.
2. **Take the largest velocity in the 3×3 neighbourhood**, not the centre one —
   dilation, so silhouettes do not smear.
3. Build a min/max colour AABB over the 3×3 neighbourhood, but **only include
   neighbours whose scene depth is within `s_depth_reconstruction_threshold`**
   of this pixel's, or where both are near-opaque. Depth-aware neighbourhood
   clamping, which is what stops history bleeding across a depth discontinuity.
4. `color_clamped = clamp(history, neighbor_min, neighbor_max)`.
5. Reject history entirely if the reprojected UV leaves the valid region, or if
   `physicalHazeVisibility(...) < 0.01` — *"decrease haze ghosting"*.
6. Blend: `lerp(reprojected, upsampled, 1 / accumulated_frames)` where
   `accumulated_frames = lerpFixed(4.0f, 1.0f, length(velocity) * 100.0f)`.

That last line is the whole velocity-vs-stability tradeoff in one expression: 4
frames of history when still, 1 (i.e. none) when moving fast.

Traced pixels write their freshly-traced value; every other pixel writes
reprojected history.

### 8.3 Upsample

`scene_copy.frag`. A bicubic filter (`texture2DCubicDepthAware`) where every one
of its five taps goes through `getNearestDepthColor()`, which compares the four
gathered low-res depths against the full-res depth and, **if they disagree by
more than a threshold, abandons filtering and point-samples the nearest-depth
texel instead**. If any tap flagged an edge, the whole bicubic is discarded and
the nearest-depth value is returned.

Smooth in the interior, nearest-depth at silhouettes — the standard fix for
low-res volumetrics haloing against geometry, done at both the neighbourhood
level and the filter level.

---

## 9. The quality presets, in numbers

Every one of these is a compile-time `#define` in `render_clouds_compositor.basemat`:

| preset | low | medium | high | ultra | extreme |
|---|---|---|---|---|---|
| `LIGHT_STEPS` (cone taps) | 1 | 3 | 5 | 6 | — |
| `EARLY_ALPHA_THRESHOLD` | 0.5 | 0.8 | 0.9 | 0.99999 | — |
| `SAMPLES_COUNT_MODIFER` | 0.25 | 0.5 | 1.0 | 1.5 | 2.5 |
| → max march steps | 32 | 64 | 128 | 192 | 320 |

Three independent axes — how many steps, how early to stop, how many lighting
taps — is a better-factored quality model than a single slider, and worth
copying as a *structure* even where the numbers do not apply.

---

## 10. What this means for this renderer

### 10.1 The honest framing

[`rdr2_atmospherics.md`](rdr2_atmospherics.md) §9.2 already settled the big
question: this project's board is ~36 m across and ~9 m tall, the camera is
tactical and looking down, and the sky is analytic. **A volumetric cloud
raymarch is not something this renderer needs**, and nothing in the UNIGINE
source changes that. §2–§7 above are a reference for a system we are not
building.

There is a second gap. UNIGINE's clouds are compute-only —
`compositor.comp`, `reprojection.comp` and `intersection.comp` are all
`MAIN_COMPUTE_BEGIN`, with `INIT_W_TEXTURE` UAV writes. This renderer is
`#version 330` with no compute path
([`rdr2_atmospherics.md`](rdr2_atmospherics.md) §9.3.1). Nothing in §8 ports
without that decision being taken first.

### 10.2 What does transfer, in order

1. **§8.3's depth-aware upsample.** Immediately applicable and independent of
   everything else. Anything rendered at reduced resolution against scene depth
   — and a froxel volume's composite is exactly that — needs this or it halos.
   The two-level structure (per-tap nearest-depth, then discard the whole
   bicubic if any tap flagged an edge) is worth copying literally.
2. **§5.3's back-up-and-re-march.** The single most reusable idea in the file.
   It applies to *any* adaptive-step raymarch, including the SDF work in
   [`re_engine_rendering.md`](re_engine_rendering.md) — whenever a coarse step
   lands inside the surface, rewinding is cheaper and much better-looking than
   accepting the overshoot, and a `refine_limit` bounds the cost.
3. **§6.4's mip-by-distance environment fetch.** One `TEXTURE_BIAS` with a
   distance-driven LOD gives distance-dependent ambient. Directly usable by the
   cubemap work that [`source2_rendering.md`](../valve/source2_rendering.md) §10 ranks
   third, and by smoke.
4. **§6.2/§6.3's phase and multiscatter pair.** A second independent
   calibration of the same model RDR2 §3.1 describes, with concrete defaults
   (`multiscattering_intensity` 0.35, `attenuation_coefficient` 0.08, the ⅔
   second lobe, `1.0`/`0.5` weights). Grenade smoke is the case that wants it.
5. **§9's three-axis quality model.** Steps / early-out / lighting taps as
   separate presets rather than one "quality" number.
6. **§4's double-solve boundary fade.** Whenever a camera crosses the boundary
   of a volume, solve the bounds twice and cross-fade them. The tactical camera
   crossing a smoke volume's edge is the same problem.

### 10.3 What does not

The density model (§2), the layer system (§7), the coverage/shape textures
(§3), the interleaved reconstruction (§8.1–8.2), and the geodetic and rounded
world modes (§4). All are answers to "an infinite sky over a planet", and this
board has a ceiling nine metres up.

The interleaved reconstruction deserves a specific note: it is the same
technique [`rdr2_atmospherics.md`](rdr2_atmospherics.md) §5.1 documents, and
§9.3.2 of that doc already flagged that **this project has no TAA and no motion
vectors by choice**. §8.2 here needs `getStaticVelocity()` and a history buffer.
So it is not "port this later" — it is downstream of a decision that has been
made the other way.

### 10.4 If a sky is ever wanted

If the tactical board ever needs a real sky rather than a backdrop — a cinematic
camera, a strategic layer — then §2 is the recipe, and it is a better starting
point than the papers because the parameter ranges are calibrated: twelve
presets from cirrus to cumulonimbus, in
`materials/base/objects/clouds/presets/`, with UNIGINE's own tooltips saying
what each knob is for. Generate the noise volumes to the specs in §3 rather than
using theirs.

---

## Sources

All primary, all from the SDK on this machine:

```
UNIGINE 2.17.0.1 Community, C:\Users\JMasc\AppData\Local\unigine\browser\sdks\community_windows_2.17.0.1_bin
  data\core\materials\base\objects\clouds\      shaders, basemats, presets
  data\core\textures\clouds\                    noise, coverage, shape textures
```

Shader source is © 2005-2023 UNIGINE and licensed under the UNIGINE License
Agreement. It is quoted here in fragments for study, and nothing from it — code
or texture — is copied into this project. `unigine_extracted/` is gitignored.

Background for the model UNIGINE is implementing, none of it read for this
document but all of it the acknowledged ancestry: Schneider & Vos, *"The
Real-Time Volumetric Cloudscapes of Horizon: Zero Dawn"* (SIGGRAPH 2015);
Häkkinen, *"Nubis"* follow-ups; Bauer, *"Creating the Atmospheric World of Red
Dead Redemption 2"* (SIGGRAPH 2019), covered in
[`rdr2_atmospherics.md`](rdr2_atmospherics.md).
