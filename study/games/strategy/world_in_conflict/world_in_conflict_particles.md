# World in Conflict — the smoke, read from the shipped build

How **World in Conflict** (Massive Entertainment / Sierra, 2007) drew the smoke
it is remembered for: what the particle system is, why the smoke reads as a lit
volume rather than a stack of sprites, and what the whole thing cost.

> **On sources, up front.** Massive published essentially nothing about this.
> There is no GDC talk, no ShaderX chapter, no engine post-mortem; Massive's own
> 2020 retrospective on the game talks about design and says nothing technical,
> and the 2007 developer interviews are about *Red Dawn* and the Cold War. What
> exists in public is **marketing copy** — "in DirectX 10 you get full soft
> particles, in DirectX 9 only against the terrain" — repeated by a dozen
> hardware sites and sourced to none.
>
> That marketing copy turns out to be *exactly true*, and this note can say so
> because **the game ships its entire shader and effect corpus as readable
> text**. WiC's `.sdf` archives hold 2,187 particle-effect definitions in a
> plain-text format and 130 surface files carrying inline HLSL, and its vertex
> shaders are HLSL string literals compiled at runtime from inside `wic.exe`.
> Everything below tagged **[BUILD]** was read out of the retail install on this
> machine — `E:\World in Conflict`, v1.0.1.0, 6.2 GB across 26 `.sdf` archives,
> 35,287 distinct files — with the reader in [`wic_sdf.py`](wic_sdf.py), written
> for the purpose. Identifiers, keywords, constants and comments are Massive's
> own, transcribed.

Tags: **[BUILD]** read from the retail install. **[EXE]** recovered from
`wic.exe`'s string region. **[PRESS]** a 2007-era published claim. **[MODERN]**
where the technique sits in 2026. **[inferred]** our reading.

Companions: [`world_in_conflict.md`](world_in_conflict.md) — the rest of the
renderer (stencil shadows, cloud shadows, light shafts, terrain, the quality
database). [`world_in_conflict_nuke.md`](world_in_conflict_nuke.md) — one effect
taken apart file by file, and the place where §2.3's cluster lighting and §6's
`CASTSHADOWS` (seven of the game's fourteen) are actually spent. Listings beside
this note:
[`particle_sixpointlight.sur`](particle_sixpointlight.sur) (the pixel half,
shipped verbatim), [`sixpoint_particle_vs.hlsl`](sixpoint_particle_vs.hlsl) (the
vertex half, recovered from the executable), [`tank_smoke.pe`](tank_smoke.pe)
(one complete effect).

Related: [`helldivers2_vfx.md`](../../shooters/helldivers2/helldivers2_vfx.md),
[`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md),
[`dcs_clouds.md`](../../flight/dcs/dcs_clouds.md).

---

## 1. The corpus, measured

**[BUILD]** Every particle effect in the game is a text file with a `.pe`
extension. There are **2,187 archive entries** covering 2,177 distinct paths —
the difference being patch re-issues in the later archives — of which 2,171 were
extracted from the seven content archives and counted below. They divide into
two classes by their first token:

| Class | Count | What it is |
|---|---|---|
| `GENERIC` | 2,104 | one emitter: one surface, one texture set, N phases |
| `COMPOUND` | 67 | a list of `PE` lines naming other effects, played together |

There is no node graph, no expression language and no scripting. An effect is a
flat key–value block followed by per-phase blocks, and a compound effect is a
list of filenames. The entire authoring surface is about forty keywords.

**The distribution that matters** — which shader each effect asks for:

| Surface | Effects | Share |
|---|---:|---:|
| `particle_sixpointlight.sur` | 721 | 34.3% |
| `particle_additive.sur` | 527 | 25.0% |
| `particle_transparent.sur` | 521 | 24.8% |
| `particle_normalmap_transparent.sur` | 192 | 9.1% |
| `particle_sixpointlight_additive.sur` | 82 | 3.9% |
| `particle_normalmap_transparent_no_shine_through.sur` | 51 | 2.4% |
| `particle_additive-transparent.sur` | 9 | 0.4% |

**803 effects — 38% of everything that emits — are six-point lit.** That is the
answer to "why does the smoke look like that", and the rest of this note is what
those two words mean.

Everything else in the corpus, for scale: `TYPE` is `CAMERAFACING` 1,995 times,
`STREAK` 77, `STRETCHSTREAK` 32 — there are no mesh particles and no ribbons
beyond stretched quads. `EMITORIGIN` is `POINT` 1,317 times, then `BOX` 329,
`CIRCLE` 290, `SPHERE` 119, `RECTANGLE` 49. 1,597 effects are flipbooks
(`TEXTUREANIMATED`), and of those the frame counts cluster hard: **661 at 128
frames**, 274 at 64, 221 at 16, 217 at 256, 187 at 32.

---

## 2. Six-point lighting

### 2.1 The idea

A smoke billboard is a flat quad showing a photograph of smoke. Light it with a
normal map and you get something that looks like an embossed sheet, because a
normal map describes a *surface* and smoke has no surface. Leave it unlit and it
looks like a decal. What you actually want is: given the sun direction, how much
light emerges from this puff *towards the camera* — and that is a property of
the volume, varying across the image.

Six-point lighting answers it by baking, offline, **how the volume responds to
light arriving from each of six axis directions**, storing those six responses as
six texture channels, and at runtime blending them by how much the real sun
agrees with each axis. Six numbers per texel, six weights per particle, one dot
product each way.

**[MODERN]** This is a first-class feature in 2026: EmberGen's Render node has a
"Six point" output mode, and Unity ships **6-way lighting** in VFX Graph as the
recommended way to light smoke. Both use the same positive/negative axis-pair
layout. WiC shipped it in **2007**, with a `ps_1_1` fallback for hardware that
predates it — see §2.5.

### 2.2 The basis is the billboard's own frame — not the world's

**[EXE]** From `SixPointLightParticleVertexShader`, recovered from `wic.exe`
(full listing in [`sixpoint_particle_vs.hlsl`](sixpoint_particle_vs.hlsl)):

```hlsl
float3 front = pos.xyz * invlen;                     // normalised view vector
float3 up    = float3(v.myUV2.x * (1.0/32767), v.myUV2.y * (1.0/32767), 0);
float3 right = cross(front, up);
right = normalize(right);
up    = cross(right, front);

Out.sun0.x = saturate(dot( right, -mySunVector) * flipV);
Out.sun0.y = saturate(dot( up,     mySunVector));
Out.sun0.z = saturate(dot( front, -mySunVector));
Out.sun1.x = saturate(dot(-right, -mySunVector) * flipV);
Out.sun1.y = saturate(dot(-up,     mySunVector));
Out.sun1.z = saturate(dot(-front, -mySunVector));
```

The six directions are **±right, ±up, ±front of the billboard**, rebuilt per
vertex from the camera. This is the design decision the whole technique rests on
and it is easy to get wrong: if the basis were world-fixed, the baked channels
would have to be re-rendered for every viewing angle, because the *image* rotates
with the camera while the lighting would not. Anchoring the basis to the quad
means the texture and its lighting rotate together, so **one authored texture
pair is correct from every camera angle and every sun angle** — and the baking
tool only ever has to render six orthographic views of the simulation.

The `saturate` matters: only three of the six weights are ever non-zero (one per
axis pair), so the pair is a cheap way of writing "signed" without a sign.

Then the normalisation, and the piece that does most of the visual work:

```hlsl
Out.sun0.w = 1 / (dot(Out.sun0.xyz, 1) + dot(Out.sun1.xyz, 1));
Out.sun0.w *= dot(normalize(pos - myPePos), mySunVector) * myUseClusterLight.x
            + myUseClusterLight.y;
```

### 2.3 Cluster lighting — a plume lit as a sphere, for one dot product

`myPePos` is the **world origin of the whole effect**, not of this particle. So
the second line asks: *where does this particle sit within its own plume,
relative to the sun?* Particles on the sunward side of a column of smoke are
brightened, particles on the far side are darkened, with a scale and bias per
effect (`myUseClusterLight.xy`; setting it to `(0,1)` disables the term).

That is self-shadowing across the plume — the thing that separates a column of
smoke from a cloud of confetti — obtained without sorting, without a depth
prepass, without per-particle occlusion queries and without any interaction
between particles at all. It costs a subtract, a normalise and a dot product,
in the vertex shader, once per particle.

**[BUILD]** In the effect files it is the `CLUSTERLIGHTING` flag (175 effects)
and the `CLUSTERLIGHTINGBONE` key (present in 1,952 of 2,104), the latter naming
the bone whose position is used as the plume origin so that smoke trailing a
moving vehicle self-shadows about the vehicle rather than about the world.

**[inferred]** This is the single most transferable idea in the note. It is not
tied to six-point lighting, to flipbooks, or to 2007 hardware: any billboard
system that knows its emitter's origin can add a per-particle brighten/darken
term along the light vector for the cost of one dot product, and get most of the
read of volumetric self-shadowing. The cost does not scale with particle count,
overdraw or light count.

### 2.4 What the two textures actually hold — verified byte-for-byte

**[BUILD]** Six-point effects always name a matching pair, and the naming
convention states the scheme outright:

```
TEXTURE  ...\SmokeAnimations\New_Nmap\rotatesmokefill_6p_0pos_128x128_128.tga
TEXTURE2 ...\SmokeAnimations\New_Nmap\rotatesmokefill_6p_1neg_128x128_128.tga
```

`6p` = six-point; `0pos` = the three positive axes; `1neg` = the three negative
ones. The archived payloads confirm the layout arithmetically. Decompressed
sizes, with the texel maths:

| File | Payload | = frames × (main + one mip) |
|---|---:|---|
| `rotatesmokefill_6p_0pos_128x128_128` | 10,485,760 | 128 × (128·128·**4** + 64·64·**4**) = 128 × 81,920 |
| `rotatesmokefill_6p_1neg_128x128_128` | 7,864,320 | 128 × (128·128·**3** + 64·64·**3**) = 128 × 61,440 |
| `rotatesmokefill_6p_0pos_64x64_256` | 5,242,880 | 256 × (64·64·**4** + 32·32·**4**) = 256 × 20,480 |
| `rotatesmokefill_6p_1neg_64x64_256` | 3,932,160 | 256 × (64·64·**3** + 32·32·**3**) = 256 × 15,360 |

Exact, in all four cases, across two resolutions and two frame counts. So:

- **`_0pos` is 32-bit** — RGB are the three positive-axis light responses,
  **alpha is the opacity**. One alpha for the pair, held with the first triple.
- **`_1neg` is 24-bit** — three more light responses, no alpha, because the pair
  shares one.
- The sheet carries **exactly one mip level**, not a chain. Enough to stop a
  minified sprite aliasing; no more, because a smoke sprite is never seen at 1/8
  scale and the frames are the memory — and a second level would start bleeding
  neighbouring frames into each other.

**A correction to how that arithmetic was first read.** The row above factors as
`128 × (128·128·4 + 64·64·4)`, and this note originally took that literally: 128
frames, each with its own small mip. It is **one 2048×1024 image, a 16×8 grid of
128×128 frames, with one mip for the whole sheet** — which is the same
10,485,760 bytes, so the arithmetic never could have told them apart. Only
decoding and looking could; read as a frame stack, every frame is a comb of
interleaved rows. Same conclusion about the mip level, different layout. See
[`world_in_conflict_nuke.md`](world_in_conflict_nuke.md) §5.1 for the fix that
went into the reader, and §5.2 for how to regenerate the sheets and look.
- Nothing is block-compressed. **[inferred]** Necessarily so: DXT interpolates
  three colour channels along a single line per block, which is exactly the
  wrong assumption for three channels that hold *independent* directional data,
  and the artefact would be smoke that changes shape as the sun moves.

Sampling the pixels of frame 40 of the 128-frame atlas confirms the reading —
the three colour channels have means of 116, 244 and 206 with independent ranges
(so, three different light responses, not a replicated greyscale and not a normal
map, which would centre on 128), while alpha is non-zero over 46% of the frame
(the smoke silhouette).

**The price.** That pair is **18.3 MB, uncompressed, resident**, for one smoke
animation — on 2007 cards that shipped with 256 MB. §4 is how they afforded it.

### 2.5 The pixel shader, and the same technique in nine instructions

**[BUILD]** From [`particle_sixpointlight.sur`](particle_sixpointlight.sur), the
whole lighting calculation:

```hlsl
half4 tex0 = tex2D(sampler0, aVSOut.myUV0);   // .rgb = +right,+up,+front  .a = opacity
half3 tex1 = tex2D(sampler1, aVSOut.myUV1);   // .rgb = -right,-up,-front

half lightdot = dot(aVSOut.mySun1.rgb, tex0.rgb) + dot(aVSOut.mySun2.rgb, tex1.rgb);
lightdot *= aVSOut.mySun1.a;                                    // normalise + cluster
half3 lightcolor = lightdot * setConstant1.rgb + setConstant2.rgb;  // sun colour + ambient
```

Two texture fetches, two dot products, a multiply-add. That is the entire cost of
volumetric-looking smoke lighting per pixel.

What is genuinely remarkable is the fallback in the same file, `Config Normal
PS1.1`, which runs the identical technique on **pixel shader 1.1** hardware —
2001-era parts — in nine instructions, with Massive's own comments:

```
dp3 r0.rgba, t0, t3      ; light contribution from first 3 dirs
dp3 r1.rgba, t1, v1      ; light contribution from next 3 dirs
add r0.rgb, r0.a, r1.a   ; total light from sun
mul r0.rgb, r0, t3.a     ; multiply with InvTotalSun value (also contains cluster lighting if active)
mad r0.rgb, r0, c1, c2   ; multiply with suncolor and add ambience
mul r0.rgb, r0, v0       ; multiply with diffuse color
+mul r0.a, t0.a, v0.a    ; alpha from texture * vertex alpha
```

**[inferred]** The lesson is that six-point lighting is not an expensive
technique that got cheap enough. It was always cheap. What it costs is *memory*
and *authoring* — six baked channels instead of one — and the reason it took the
industry another decade to adopt widely is that the offline volumetric renderers
needed to bake those channels (EmberGen and friends) did not exist yet. Massive
paid that cost by hand in 2007, and §4 shows they could only afford to do it a
handful of times.

---

## 3. Soft particles, two different ways — and the marketing was true

**[PRESS]** Every 2007 preview carried the same line: DX10 gets soft particles
against everything, DX9 gets them only against the terrain. **[BUILD]** Both
paths are in one shader, in one `#ifdef`, and the claim is exact:

```hlsl
#ifdef ZFEATHER
    half zfeather = 1;
    @ifdef DX9_RENDER
        half4 groundy = tex2D(sampler2, aVSOut.myPos01.xy);
        half hsample = dot(groundy, half4(0.25,0.25,0.25,0.25)) * 128.0;
        zfeather = saturate((aVSOut.myPos.y - hsample) * setConstant4.x);
    @else
        @ifdef DX10_RENDER
            float sceneDepth = texload2D(sampler4, int3(aVSOut.myPosition.x, aVSOut.myPosition.y, 0));
            float myDepth = aVSOut.myPos.w;
            zfeather = saturate(10000 * setConstant4.x * (sceneDepth - myDepth));
        @endif
    @endif
    alpha *= zfeather;
#endif
```

The DX10 branch is the textbook one: read the scene depth at this pixel, fade by
the difference. The DX9 branch does something better than a workaround —
`myPos01` is the particle's **world XZ normalised to map size**, and `sampler2`
is `ex_heightmap`, a **top-down terrain height texture**. So it compares the
particle's world Y against the terrain height directly underneath it and feathers
on that.

**[inferred]** Worth noticing what that buys. It needs **no depth buffer access
at all** — no depth prepass, no resolve, no second render target, none of the
things that were awkward or impossible on DX9 hardware. The heightmap is a small
texture that already exists, sampled once. It is wrong wherever a particle
overlaps a vehicle or a building, which is precisely what the marketing admitted,
but it is right for the case that dominates an RTS camera: smoke sitting on the
ground. **A cheap approximation aimed at the common case beat the general
solution by enough that they shipped both.**

Two more notes on the same block. There are **four pass variants** —
`NormalPass`, `TexDetailPass`, `ZFeatherPass`, `ZFeatherTexDetailPass` — because
the two optional features are compiled out rather than branched, and the renderer
picks the pass. And `DISABLEZFEATHER` appears in **276 effects**: soft particles
are opt-out per effect, because feathering a sharp-edged spark or a decal-like
scorch just blurs it.

---

## 4. How they afforded it: sharing, detail texturing, and LOD

**[BUILD]** 18 MB per smoke animation is only survivable if there are very few
animations. There are:

| Texture pair | Effects using it |
|---|---:|
| `rotatesmokefill_6p_*_128x128_128` | 224 + 60 |
| `rotatesmokefill_6p_*_64x64_256` | 113 + 60 + 15 |
| `rotatesmokethin_6p_*_128x128_128` | 89 |
| `billowingsmokethick_1/2_128x128_64f` | 59 |
| `smokestreak_2_6pointl_pos/neg` | 21 |

**One texture pair is shared by 224 effects.** Across the whole six-point set,
803 effects draw on roughly a dozen authored pairs. The variety the player sees
— tank smoke, house fires, helicopter dust, artillery, wreck plumes — is
produced almost entirely by the *parameters* around a shared image: colour,
size curves, velocity, spawn rate, phase count, lifetime.

**[inferred]** This is the trade the format is built around, and it is the
opposite of the modern default. Authoring is centralised and expensive; instancing
is free and unlimited. A pipeline where each artist bakes their own flipbook
would have needed 200× the memory to look the same.

**Detail texturing** is how they stop a shared 128×128 sprite looking soft when
it fills the screen. 1,811 of 2,104 effects carry a `DETAILTEX` line, and **703
of them carry literally the same one**:

```
DETAILTEX 0 0 2.5 100 200
```

— a shared noise texture at 2.5× tiling, faded in between 100 and 200 units of
distance. In the shader it modulates colour and alpha separately:

```hlsl
half2 detail = tex2D(sampler3, aVSOut.myDetUV * setConstant6.x).xy;
detail = setConstant5.xy + detail * setConstant5.zw;
...
half4 retValue = half4(aVSOut.myDiffuse.xyz * lightcolor.xyz * detail.x, alpha * detail.y);
```

High-frequency break-up applied only where it can be seen, at the cost of one
extra sample in one extra pass variant.

**Distance and quality LOD** are per-effect and blunt. Every effect carries
`RADIUS` and `REMOVEDIST` (median **100** and **600** units respectively); 120
effects carry `DISABLEONLOWEND` and simply do not exist below a quality
threshold; and **[EXE]** the renderer's global knobs are
`myGlobalParticleEmitRate` (a float multiplier on every spawn rate in the game)
and `myParticlesPerfSetting`. The engine also tracks `LargestParticleMem` and
`LargestParticleName` as live counters — **[inferred]** someone had been hunting
a memory spike and left the instrument in.

---

## 5. The effect format, annotated

**[BUILD]** [`tank_smoke.pe`](tank_smoke.pe) in full is the burning-tank plume —
the effect the game is remembered for. The header:

```
GENERIC
FLAGS TEXTUREANIMATED RANDOMFLIPV
CLUSTERLIGHTINGBONE
FLIPVPROBABILITY 0.5
RADIUS 10.0 REMOVEDIST 500.0 POSTFX 0 WINDAFFECT 0.00 SPREAD 1.000
FADEOUTTIME 9999.0 1.0
THICKNESS 1.00
SHADEHARDNESS 0.0
EMITORIGIN POINT
TYPE CAMERAFACING
SURFACE surfaces/particle_sixpointlight.sur
TEXTURE  ...BillowingSmokeThick_1_128x128_64F.tga
TEXTURE2 ...BillowingSmokeThick_2_128x128_64F.tga
NUMTEXTURES 64
ANIMTIMEMIN 0.04  ANIMTIMEMAX 0.05
DETAILTEX 0 0 2.5 100 200
TEXTUREROTATION RANDOMONCEINTERVAL 0 360
TOTALTIME 0.2   SPAWNTIME 10
```

Then `NUM_PHASES 2`, each phase a block of `VELX/VELY/VELZ`, `SIZE`, `ROTVEL`,
`ALPHA` and `ADDALPHA` with a `LENGTH` (duration) and a mode word — `SET`, `ADD`
or `MOD` — saying whether the phase replaces, adds to, or multiplies the running
value. A particle's life is a piecewise curve with three composition rules, and
`NUM_PHASES` is 2 or 3 for 79% of the corpus.

Four things in that header are worth calling out because they are cheap and
frequently omitted elsewhere:

- **`RANDOMFLIPV` + `FLIPVPROBABILITY 0.5`** — mirror half the sprites
  vertically. Free variety from one image; 730 effects use it. It is also the
  reason for the `flipV` multiply in the vertex shader (§2.2): flipping the
  image without flipping the left/right lighting weights would detach the
  lighting from the picture. **A cheap variety trick almost always has a
  correctness tail, and this is what paying it looks like.**
- **`TEXTUREROTATION RANDOMONCEINTERVAL 0 360`** — a random fixed roll per
  particle, so a plume of the same sprite has no visible repeat.
- **`RANDTEXTUREANIMSTART 1`** and `MAXSTARTFRAME` — start each particle at a
  random frame of the flipbook, so a burst does not animate in lockstep.
- **`MATERIAL`** — 2,001 effects say `ANY_MATERIAL`, but 85 name a ground type
  (`SNOW`, `GROUND_DIRT`, `GROUND_GRASS`, `GROUND_ROCK`, `WATER`, `METAL`), and
  the `hit_set_effects/` tree is organised by weapon × surface. Impacts pick
  their debris from what was hit. This is gameplay-facing: the plume tells you
  what the shell landed on.

Other flags in the corpus worth knowing: `SORT` (650 — per-effect opt-in to
back-to-front sorting, *not* the default), `PHYSICAL` (87 — particles that
collide), `WORLDSPACEALIGNEDX/Y` (310 — billboards constrained to an axis, for
ground dust and wall scorch), `NOFOG` (35), `CASTSHADOWS` (14).

---

## 6. Smoke that casts shadows — 14 effects, and one channel

**[BUILD]** `CASTSHADOWS` is set on **14** of 2,104 effects, and
[`particle_shadows.sur`](particle_shadows.sur) is nine lines of shader:

```hlsl
float4 main (VSOutput aVSOut) : COLOR0
{
    half4 tex = tex2D(sampler0, aVSOut.myUV0.xy);
    return float4(0, tex.a * aVSOut.myColor.a, 0, aVSOut.myUV1.x);
}
```

with `blend = one invsrccolor`. The particle's opacity is accumulated into a
single channel of the shadow buffer, multiplicatively, and joins the terrain and
stencil shadow terms in the screen-space composite described in
[`world_in_conflict.md`](world_in_conflict.md) §3.

**[inferred]** Fourteen. Casting shadows from smoke was affordable enough to
build, and the artists were allowed it on the big set-piece plumes and nowhere
else. That ratio — a capability shipped and then used 0.7% of the time — is a
better guide to what it cost than any performance number in the build.

**Where the fourteen went**, since the question is obvious once the number is:
**seven are the tactical nuke**, and the other seven are the daisy cutter, the
fuel bomb, carpet bombing and one smoke marker. Every shadow-casting particle in
the game is a tactical aid — see [`world_in_conflict_nuke.md`](world_in_conflict_nuke.md)
§2.3.

---

## 7. What to take

**Take the cluster-lighting term.** One dot product between (particle − emitter
origin) and the light vector, scaled and biased per effect, applied as a
brightness multiplier. It is independent of everything else here, it works with
ordinary alpha billboards and one texture, and it is most of the difference
between smoke that reads as a volume and smoke that reads as sprites. §2.3.

**Take the shared-flipbook economy.** A dozen authored animations behind two
thousand effects, with per-effect parameters doing the differentiating and a
shared detail texture restoring high frequencies up close. §4.

**Take the DX9 z-feather.** Feathering particles against a top-down terrain
heightmap needs no depth buffer, no prepass and no render target, and it covers
the case that dominates a ground-level camera. Worth having as the cheap path
even now. §3.

**Take six-point lighting only if something bakes it for you.** The runtime is
nine instructions; the cost is 18 MB per animation and an offline volumetric
render. **[MODERN]** In 2026 that bake is a checkbox in EmberGen and a supported
workflow in Unity's VFX Graph, which is a different proposition from 2007 — but
it is still six channels of unblock-compressible texture per animation, and the
memory is what will decide it. §2.4.

**Do not take the flat phase model** unless the constraint suits you. Two or
three phases of set/add/mul over a fixed parameter list is a small, fast,
inspectable format, and it is why 2,187 effects fit in a few megabytes of text —
but every behaviour outside the list is unavailable, which is why 67 compound
effects exist to glue simple ones together. §1, §5.
