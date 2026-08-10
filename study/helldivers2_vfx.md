# Helldivers 2 particle rendering

Reconstructed from the game's own shader bytecode, decompiled to GLSL by
filediver, plus the material parameter blocks that drive it. Every number and
every line of logic below is traceable to a specific shader — nothing here is
inferred from how it looks on screen.

Source material for one representative billboard material:

```
workbench/hd2/mat_folder_probe/0xc53e54cb356e3edc.dir/
    shaders/program-0/*.glsl.frag        depth/discard prepass
    shaders/program-18-0/*.glsl.frag     full shading (19 KB)
    shaders/program-18-1/*.inst.glsl.vert instanced billboard vertex
    0xe3623d15ec0a9471.png               1024x1024 flipbook (4x4 cells)
    0xd158a9b38449b855.png               128x1 colour LUT
```

The whole set for every particle-referencing material is in
`hd2_extracted/particle_materials/`, and `hd2_extracted/particles/particles_index.csv`
maps each of the 2839 particle systems to the materials it uses.

## Read this first: there is no single particle shader

Grouping all 1450 particle-referenced materials by their parameter schema gives
**189 distinct shader families**. `c_billboard` is a *generated* struct — two
materials both declare one and the layouts have nothing in common. So there is
no "the Helldivers 2 particle shader" to copy.

Ranked by how many of the 14,646 particle→material references each family
accounts for:

| # | materials | refs | character |
|---|---|---|---|
| 1 | 113 | 2052 (14%) | **smoke with subsurface scattering** — `sss_*`, two-tone colour, terrain albedo pickup, flipbook blending |
| 2 | 63 | 591 (4%) | as 1, plus an emissive LUT (`lut_emissive`, `lut_exp`, `emissive_mult`) |
| 3 | 5 | 402 (3%) | minimal luminosity-remap billboard |
| 4 | 27 | 368 (3%) | scrolling/curvature cards — `scroll_distance`, `curvature`, `rotation_speed` |
| 5 | 42 | 353 (2%) | beams — `is_beam`, `color_lut`, `no_taa_history_intensity` |
| 6 | 21 | 346 (2%) | explicit flipbook animation — `FPS`, `looping_animation`, `use_atlas` |

**The family documented in the rest of this file is a small one.** It was the
first material sampled, and it is worth reading because it is the simplest
complete example — but the workhorse is family 1, covered immediately below.
The long tail is real: 189 schemas over 1450 materials means most are one-offs.

Family 1's parameter block, in declaration order, is the honest picture of what
an Arrowhead VFX artist actually tunes:

```glsl
vec3  weather_effect_color;   float ao_lowest_value;
float sample_terrain_albedo;  vec3  smoke_color_secondary;
float depth_distance_fade;    float discard_below_objects;
vec3  smoke_color;            float sss_over_life;
float dist_scale_min;         vec2  global_size_mult;
float sss_intensity;          float dist_offset;
vec2  rows_and_columns;       float use_flipbook_blending;
float angle_fade_exp;         float normal_strength;
float emissiveness_base;      float luminocity_curve;
vec2  pivot_point;            float sss_gradient;
float luminocity_max;         float sss_wrap;
float terrain_color_lerp;     float camera_fade_distance;
float sss_diffusion;          float sss_intensity_start;
float rnd_nrm_strength;       float use_particle_color;
float dist_fade_offset;       float alpha_mult;
float alpha_exp;              float lifetime_exponent;
float use_two_colors;         vec3  sss_color;
float color_mult_down;        float sss_smoke_color;
float luminocity_min;         float weather_effect_color_lerp;
float max_dist;               float dist_scale_max;
float fade_dist_divide;       float shrink_near_camera;
```

Smoke here is a **lit volume approximation**, not a sprite: it scatters light
(`sss_*`), samples the ground it sits on (`sample_terrain_albedo`,
`terrain_color_lerp`), tints toward the weather state
(`weather_effect_color_lerp`) and has normals (`normal_strength`,
`rnd_nrm_strength`).

### Family 1 mechanics, from `0x828024d633ee1210`

**Flipbook driven by normalised lifetime, with sub-frame cross-blending:**

```glsl
float life  = pow(age / lifetime, lifetime_exponent);
float total = rows_and_columns.x * rows_and_columns.y;
float fpos  = total * life;
uint  i0    = uint(fpos);
float a0    = texture(tex_texture_rgba_map, cell_uv(i0)).a;

if (use_flipbook_blending > 0.5) {
    uint  i1 = min(uint(total) - 1u, i0 + 1u);
    float a1 = texture(tex_texture_rgba_map, cell_uv(i1)).a;
    a0 = mix(a0, a1, fract(fpos));      // no frame popping
}
float alpha = clamp(pow(a0, alpha_exp) * alpha_mult, 0.0, 1.0) * particle_alpha;
```

Two things the simple family lacks: the frame index is driven by
`pow(life, lifetime_exponent)` — so animation can ease in or out rather than
running linearly — and consecutive frames are **cross-faded**, which is what
stops low-framecount sheets from strobing.

Note the mask is in the **alpha** channel here (`.a`), not `.r`; the sheet is
`tex_texture_rgba_map` and the RGB carries something else.

**Angle fade, smoothstep then exponent:**

```glsl
float d = clamp(dot(normal, view_dir), 0.0, 1.0);
float s = d * d * (3.0 - 2.0 * d);
alpha *= pow(s, angle_fade_exp);
```

**Camera proximity fade** — the thing that stops walking into a smoke plume
from filling the screen:

```glsl
float t = clamp((dist - dist_fade_offset) / max(camera_fade_distance, 0.001), 0.0, 1.0);
alpha *= t * t;
```

**Soft particles at full resolution** — `tex_linear_depth`, not the mip-6
pyramid the other family uses, and the fade distance is per-particle
(`particle_fade * depth_distance_fade`) rather than a constant. Different
family, different trade-off; neither is "the" answer.

### The subsurface model

Family 1 materials only dump prepass permutations — every one of
`0x828024d633ee1210`'s 19 programs writes constant white and discards. The
colour pass is not in their shader sets, and it is not in the named
`shader_library` assets either.

It *is* present in sibling materials. `0x0347831766f06cd1` dumps a 21.7 KB
fragment shader with four G-buffer outputs (adding
`ambient_diffuse_light_xyzw`) and thirteen live `sss_*` references. Reading it:

```glsl
// Wrapped diffuse - the whole translucency model, in one mix.
float wrapped = mix(ndl, 1.0 - ndl, clamp(sss_wrap, 0.0, 1.0));
```

At `sss_wrap = 0` this is plain Lambert. At `1` it is pure back-lighting, so the
plume lights up where the sun is behind it. In the bytecode it appears as
`r0.z = sss_wrap * (1.0 - 2.0*ndl) + ndl`, which is the same expression
expanded.

The SSS strength is then smoothstepped over a density window and masked:

```glsl
float t   = saturate((density * sss_intensity - alpha_low) / (alpha_high - alpha_low));
float sss = saturate(mask * (t*t*(3.0 - 2.0*t) - sss_intensity) + sss_intensity);
vec3  tint = use_sss_color ? sss_color : albedo;
```

so thin wisps transmit light and dense cores do not — which is physically the
right way round, and is what stops the whole plume glowing uniformly.

**Caveat worth keeping in mind:** that sibling spells its parameters
`diffuse_alpha_low/high`, `use_sss_color`; family 1 spells them `sss_gradient`,
`sss_over_life`, `sss_smoke_color`. The *model* is the same wrapped-diffuse
translucency, but the exact remap expression for family 1's spelling was not in
any dumped permutation. The reference implementation marks which lines are
transcribed and which are reconstructed.

## What kind of particle system this is

Not a forward-rendered blended sprite. Particles write into the **deferred
G-buffer**, three targets:

```glsl
layout(location = 0) out vec4  base_color_rgb_material_id_w;
layout(location = 1) out float normal_or_shell_direction_xyz_roughness_w;
layout(location = 2) out vec4  ao_x_metallic_density_cloth_clearcoat_shellnormal_y_velocity_zw;
```

That last target carries **velocity**, so particles are motion-vector correct
for TAA and motion blur — they are first-class scene geometry, not a
transparent afterthought. It also means their soft-particle and fog work has to
happen in the same pass rather than being composited later.

## The material parameter block

`c_billboard`, std140, binding 19. This is the entire tunable surface of a
billboard effect — the artists' whole vocabulary:

```glsl
float discard_below_objects;           // 0
float particle_color;                  // 4    bool switch
float opacity_exp;                     // 8
vec2  pivot_point;                      // 16
float DistScale;                        // 24
float resolution_setting;               // 28
float MaxScale;                         // 32
float use_lut;                          // 36   bool switch
float spot_light_visibility_distance;   // 40
vec2  global_size_mult;                 // 48
float spot_light_visibility_multiplier; // 56
vec2  angle_fade_range;                 // 64
vec2  Frames;                           // 72   flipbook grid
float emissive_intensity;               // 80
float fade_distance;                    // 84   soft-particle depth
```

Values for the sampled material (`0xc53e54cb356e3edc`), from the exported glb
`extras`: `Frames [4,4]`, `emissive_intensity 22`, `fade_distance 0.1`,
`angle_fade_range [0.7,1]`, `opacity_exp 1`, `pivot_point [0.5,0.5]`,
`DistScale 100`, `MaxScale 18`, `use_lut 1`, `particle_color 1`.

`Frames [4,4]` against a 1024x1024 sheet means **256 px flipbook cells**, and
`use_lut 1` is why a 128x1 texture ships beside it.

## Vertex stage

### 1. TAA jitter, applied in the shader

```glsl
vec2 j = fract(frame_number * vec2(0.754878, 0.569840) + 0.5) * 2.0 - 1.0;
j *= clamp(upscaling_method, 0.0, 1.0);      // 0 disables it
j  = (debug_rendering != 0.0) ? vec2(0.0) : j;
j /= vp_render_resolution;
...
vec4 ndc = clip / clip.w;
ndc.xy += j;
oSV_POSITION = ndc * clip.w;                 // re-multiply, keep w for depth
```

Those two constants are **the R2 low-discrepancy sequence**: `1/φ₂` and `1/φ₂²`
where φ₂ = 1.324717957 is the plastic number. It is a two-dimensional
generalisation of the golden-ratio sequence, and it beats Halton for this
purpose because every prefix is well distributed — you never get a bad frame
early in the sequence.

Worth copying verbatim. Note the jitter is applied *after* the perspective
divide and then scaled back by `w`, which keeps the depth value untouched.

### 2. Distance-compensated size

```glsl
float dist  = length(cb_camera_pos - particle_pos);
float scale = clamp(dist / DistScale, 1.0, MaxScale);   // 100, 18
vec2  size  = scale * in_size * global_size_mult;
```

Particles **grow with distance**, from 1x at 100 m up to 18x. This is a
readability decision, not physics: it keeps distant smoke and explosions
legible on a screen where the action is often hundreds of metres away, and it
stops sub-pixel particles from aliasing into shimmer. It is also why HD2's
distant explosions read as huge.

### 3. Pivot, rotation, billboard basis

```glsl
vec2 corner = in_corner * 0.5 + pivot_point - 0.5;      // pivot in [0,1]
vec2 ext    = size * corner;

float s = sin(in_rotation), c = cos(in_rotation);
vec3 right =  in_tangent  * c - in_binormal * s;
vec3 up    =  in_tangent  * s + in_binormal * c;
vec3 world = particle_pos + right * ext.x + up * ext.y;
```

The basis comes from the **vertex stream** (`TANGENT`/`BINORMAL`), not from the
camera. One shader therefore covers camera-facing, velocity-aligned and
fixed-axis billboards; which one you get is decided by whatever fills those
attributes. Rotation is per-particle and applied in that basis.

### 4. Angle fade — smoothstep on facing

```glsl
float d = dot(view_dir, particle_normal) - angle_fade_range.x;
float t = clamp(d / (angle_fade_range.y - angle_fade_range.x), 0.0, 1.0);
float angle_fade = t * t * (3.0 - 2.0 * t);             // smoothstep
```

Fades a billboard out as it turns edge-on. With `[0.7, 1.0]` it is invisible
below 0.7 facing and full at 1.0. This is what stops flat cards from betraying
themselves as cards when the camera swings past — the single cheapest fix for
the classic "paper explosion" look.

### 5. Colour decode

```glsl
oTEXCOORD0 = in_color.zyxw * in_color.zyxw;
```

`.zyx` is a BGRA→RGBA swizzle, and the square is a **gamma-2.0 sRGB→linear
approximation**. Cheap, and close enough for particle tint.

## Fragment stage

### 1. Alpha shaping

```glsl
float a = pow(texture(tex_base_color, uv).r, opacity_exp);
a *= particle_alpha;      // per-particle, from vertex colour w
a *= angle_fade;          // from the vertex stage
```

The mask is a **single channel** (`.r`) — the flipbook is a greyscale density
sheet, and all colour comes from the LUT and tint. The `pow` appears in the
bytecode as `exp2(log2(abs(x)) * opacity_exp)`, which is just how the compiler
spells it.

### 2. Soft particles against a depth *pyramid*

```glsl
vec2  suv        = gl_FragCoord.xy / (vp_render_resolution * 0.5);
float scene_z    = textureLod(tex_linear_depth_mip6, suv, 1.0).x;
float soft       = clamp((scene_z - particle_z) / fade_distance, 0.0, 1.0);
float alpha      = clamp(a * soft, 0.0, 1.0);
if (alpha < 0.001) discard;
```

Two details worth stealing:

* The depth source is **`linear_depth_mip6` sampled at LOD 1**, i.e. a heavily
  downsampled linear depth pyramid, not full-resolution depth. Soft particles
  do not need precision — they need a smooth intersection — and a small mip is
  both cheaper to sample and naturally smoother across the edge.
* `fade_distance` is **0.1 m** on this material. That is a tight fade; the
  effect is to kill the hard intersection line, not to produce a broad haze.

The prepass variant (`program-0`) runs the same maths but with
`if (alpha < 0.5) discard;` and writes constant white — an alpha-tested depth
or shadow pass.

### 3. Colour from a LUT indexed by alpha squared

```glsl
vec3 grad = texture(tex_gradient_map, vec2(a * a, 0.0)).rgb;
vec3 col  = use_lut       == 1.0 ? grad * tint : a * tint;
     col  = particle_color == 1.0 ? col        : grad;
col *= emissive_intensity;                     // 22 here
```

This is the most transferable idea in the whole shader. The 128x1 gradient is
indexed by **alpha squared — the density of the sprite at that pixel — not by
particle age**. Dense core samples the hot end of the ramp, thin edges sample
the cool end, so a single greyscale puff becomes a fire with a white core, an
orange body and a dark smoky rim, and it stays consistent as the sprite
deforms. Squaring biases the ramp toward the thin end, widening the cool
region.

Age-indexed ramps give you an effect that changes colour over time. This gives
you an effect that is *internally* coloured. They are not interchangeable, and
this one costs a single texture fetch on a 128-pixel texture.

`emissive_intensity` of 22 is an HDR value: these are bloom sources by
construction.

### 4. Fog, in two regimes

Under 200 m, the particle samples the **volumetric fog froxel volume**, with a
logarithmic depth mapping:

```glsl
float slice = log2(depth * 0.145 + 1.0) * 0.203795;
slice = min(slice, 1.0 - 0.5 / float(textureSize(tex_volumetric_fog_3d_image, 0).z));
float fog = textureLod(tex_volumetric_fog_3d_image, vec3(suv, slice), 0.0).w;
```

The `min` is a half-texel clamp against the last slice — without it, distant
particles sample past the end of the volume and flicker.

Beyond 200 m it switches to **analytic exponential height fog**, evaluated with
the standard closed-form integral of density along the ray:

```glsl
float k = 1.0 / max(fog_parameters.x, 1e-4);
float h = exp2(clamp(-(end_height + fog_parameters.y) * k, -40.0, 40.0) * 1.442695);
float t = (1.0 - exp2(clamp(dist * k, -40.0, 40.0) * 1.442695)) / dir_y;
fog = clamp(t * h * fog_parameters.z, 0.0, 65504.0);
```

`65504` is the max finite half-float — the target is fp16, and they clamp to it
explicitly rather than risking an inf. The `1.442695` is `1/ln(2)`, converting
`exp` to `exp2`.

Splitting at 200 m is a sensible economy: the froxel volume only has useful
resolution near the camera, and past it an analytic height fog is both cheaper
and more stable.

## Replication checklist

Items 8-11 come from family 1 (the workhorse); the rest from the simple family.
In rough order of visual payoff per unit of effort:

8. **Cross-blended flipbook frames** (`mix(frame_n, frame_n+1, fract)`). Kills
   strobing on low-framecount sheets; costs one extra fetch.
9. **Frame index driven by `pow(life, lifetime_exponent)`** so the animation can
   ease rather than run linearly.
10. **Camera proximity fade** (`t*t` over `camera_fade_distance`). Stops walking
    into a plume from filling the screen.
11. **`pow(smoothstep(facing), angle_fade_exp)`** — the exponent gives far more
    control than the bare smoothstep in item 2.


1. **Density-indexed colour LUT** (`gradient[alpha²]`). Biggest single win.
   Turns a greyscale sheet into a rich effect with one 128x1 texture.
2. **Smoothstep angle fade** on facing. Removes the flat-card tell.
3. **Soft particles against a low mip of linear depth.** Cheaper and smoother
   than full-res depth.
4. **Distance-compensated scale**, clamped to `[1, MaxScale]`. Readability at
   range.
5. **`pow(mask, opacity_exp)`** for cheap density shaping without new textures.
6. **R2 jitter** (`0.754878, 0.569840`) if you are doing TAA at all.
7. **G-buffer + velocity output**, if your renderer is deferred and you want
   particles to survive TAA and motion blur without ghosting.

Two reference implementations, readable GLSL rather than decompiler output:

* [`helldivers2_particle.vert`](helldivers2_particle.vert) +
  [`helldivers2_particle.frag`](helldivers2_particle.frag) — the simple family.
  Items 1-6. Small, complete, everything transcribed from bytecode. **Start
  here**: it runs, and the density-indexed LUT alone changes how your effects
  look.
* [`helldivers2_smoke.frag`](helldivers2_smoke.frag) — **family 1, the
  workhorse.** Flipbook cross-blending, per-particle soft-particle range, angle
  fade with exponent, camera proximity fade, two-tone density colour,
  wrapped-diffuse SSS, luminosity remap, terrain albedo pickup and weather tint.
  Every block is marked `[BYTECODE]`, `[DERIVED]` or `[RECONSTRUCTED]` so you
  know which parts are Arrowhead's and which are a considered reconstruction.

Pair the smoke fragment shader with `helldivers2_particle.vert`; the vertex work
(billboard basis, distance scaling, R2 jitter) is common to both, and the smoke
shader needs `v_life` and `v_fade` added to the varyings.

## The `.particles` container — partially decoded

filediver dumps these raw (`.particles.main`) because it has no extractor for
them. They are compact binary with no strings. This is how far a first pass
gets; it is enough to index and rank all 2838 systems, and not enough to
recover emitter behaviour.

### Header

```
0x00  u32    version        115 in ALL 2838 files - a good format check
0x04  float  ~10.0
0x08  float  ~15.0
0x0C  float  cull/bounds    0.0 .. +inf across the set
0x10  u32    flags          {0,16,32,33,...}, 12 values observed
0x14  u32                   {0,1,2,3}
0x18  u32    correlates 0.845 with material count - NOT the emitter count
0x1C  u32    {0,1}
0x20  u64    a hash, unique per file (self/name?)
0x28  u32    small enum
0x2C  float
0x30..0x4F   zero in every file sampled
```

**A caution, because I got this wrong first time.** `u32@0x18` looks exactly
like an emitter count on the file you happen to open first — 9 emitters, 9
material references, header says 9. Across all 2838 files it agrees with the
true emitter count only **18%** of the time. It correlates strongly (0.845)
with complexity without *being* the count. Verify structural guesses across the
whole corpus, not one convincing example.

### Emitter slots

Each emitter's material reference is preceded by a fixed 28-byte signature:

```
01 00 00 00  00 00 00 00  10 00 00 00  ff ff ff ff
00 00 00 00  00 00 00 00  01 00 00 00
<u64 material hash>  <u32 kind>
```

Only the 4 bytes *before* the signature vary. Searching for it locates every
emitter slot reliably:

* **15,422 emitter slots** across 2838 systems
* `kind` takes four values: **5** (6232), **6** (5741), **7** (3376), **8** (73)
* Blocks are variable-size, roughly 2.0–2.7 KB each
* The most complex system, `0x43c61bb4b8c35c99`, has **28 emitters** in 82 KB

The `kind` enum is unidentified. Four values with that distribution is
suggestive of a render-mode (billboard / mesh / ribbon / light) but nothing has
been proven, so it is recorded and not interpreted.

`particles_index.csv` carries all of this: one row per system with version,
flags, byte size, emitter count, per-emitter material hashes and kind codes.
Join it against `material_families.csv` to get from any system to its shader
family and parameters.

### What this does and does not buy you

You can now **rank and filter all 2838 systems** — by emitter count, by shader
family, by whether they use subsurface scattering — and jump straight from a
system to the flipbooks, LUTs and shaders it draws with. For choosing what to
study, that is most of the way there.

What is still opaque is everything *inside* an emitter block: spawn rate,
lifetime, velocity and size curves, forces, sub-emitters. Those are the 2 KB
between one signature and the next, and decoding them means identifying curve
encodings without a single string to anchor on. Feasible — the signature gives
clean block boundaries to work within — but a project in its own right.

## What is not here

The **emitter** side. Spawn rates, lifetimes, velocity and size curves, forces
and sub-emitters live in the `.particles` files, which are compact binary with
no strings and no extractor in any public tool. `particles_index.csv` records
which materials each system references, and nothing more.

That means this document tells you how a Helldivers 2 particle is *shaded*, not
how it is *born and moves*. For replication that split is less painful than it
sounds: the rendering is where the distinctive look lives, and emitter
behaviour is the part you would want to author to taste anyway.

Reversing the emitter format would be a project of the same shape as the
DSAR/DSAA work in [`helldivers2_formats.md`](helldivers2_formats.md) — feasible,
but a real undertaking rather than an afternoon.
