# Source 2 glass — five different problems wearing one word

Working notes on how Source 2 draws glass, written because "glass" in that
engine is not one feature. A window, a frosted partition, a shattering pane, a
drinking glass and the vodka inside it are solved by five different mechanisms
that share almost nothing, and the interesting part is *which* of them Valve
chose to make cheap and which they left expensive.

Companion to [`source2_rendering.md`](source2_rendering.md) §12.1, which
established the baseline — CS2 glass is a Fresnel opacity ramp with no
refraction, and that is physically right for a flat pane. This note takes the
other four cases seriously, and it has evidence the earlier note did not: a
**live Source 2 install with plain-text shader source on disk**.

---

## 0. Where the evidence comes from, and how much to trust it

**[SBOX]** is the s&box retail install at
`E:\SteamLibrary\steamapps\common\sbox`. This matters more than it sounds.
s&box is Source 2 licensed to Facepunch, and it ships the shader library **as
readable text**, which no Valve title does:

| Path | What it is | Whose code |
|---|---|---|
| `core/shaders/*.fxc` | the engine's shader library — `vr_lighting.fxc`, `vr_common_ps_code.fxc`, `vr_shared_standard_ps_code.fxc`, `common.fxc` | **Valve's**, Alyx-era, with Facepunch edits |
| `addons/base/Assets/shaders/*.shader` | the game-side shaders — `glass.shader`, `water_simple.shader`, `terrain.shader` | **Facepunch's**, written on top |
| `bin/win64/*.dll` | `engine2`, `materialsystem2`, `meshsystem`, `physicsbuilder`, `rendersystemvulkan` | Valve's engine binaries |

So when this note quotes `vr_lighting.fxc` it is quoting Source 2's own lighting
code; when it quotes `glass.shader` it is quoting a Source 2 *client's* glass
shader, which is a different claim. Both are labelled. Facepunch's glass is not
Valve's glass — but it is glass written against Valve's material model, Valve's
lighting terms, Valve's frame-buffer-copy facility and Valve's render-state
syntax, by people who have the engine source. It is the closest readable thing
to how Source 2 wants glass written, and where it and CS2 disagree the
disagreement is itself informative.

**[CS2]** is the retail Counter-Strike 2 install *with its authoring content* —
`content/csgo/.../breakable_glass/` ships Valve's own `.vmdl` and `.vmat` as
**uncompiled KV3 text**. This is Valve's authoring source, and it outranks
everything else here.

**[VRF]** is ValveResourceFormat's reimplementation of Valve's shipped shaders —
accurate in parameter names and structure, not Valve's source.
**[VDC]** is Valve's own wiki: the CS2 FGD, the CS2 workshop-tools tutorial for
breakable windows, the `VR Standard` shader reference. Behind an Anubis
proof-of-work; fetched as wikitext via `action=raw`.
**[press]** is journalism quoting a named Valve developer. **[COMMUNITY]** is
widely-repeated but not first-party. **[inferred]** is our reading.

> **Status, 2026-08-16.** No frame capture behind this. s&box is fully
> installed and is where every quoted `.fxc`/`.shader` line comes from; two
> revisions of `glass.shader` are on disk (2024-08-31 under `download/`, and the
> current 2026-07-29 one) and §6 leans on the diff between them.
>
> **Updated the same day: CS2 is now installed, with the authoring content.**
> `content/csgo/workshop/content_examples/breakable_glass/` holds Valve's own
> **uncompiled** `.vmdl` and two `.vmat` files — KV3 text, not a compiled
> resource — which settles §7 outright and corrects one claim in §6.5.
> `pak01_dir.vpk` was then opened with ValveResourceFormat's CLI and **all 53
> shipped `csgo_glass.vfx` materials** read, which settles §4 and §5 and adds
> §2.1b. Everything tagged **[CS2]** is Valve's shipped authoring or shipped
> material data — not a reimplementation, not a tutorial's paraphrase — and it
> outranks every other source here.
>
> Three claims written before that read did not survive contact with it, and are
> marked where they sit: CS2 *does* ship refraction (§4), its frosted glass has
> nothing to do with blur (§5.0), and its glass casts shadows about half the
> time rather than never (§6.5). The compiled **shaders** remain unread — VRF
> 19.2 cannot parse CS2's VCS version 71 — so every statement about what a
> Source 2 shader *computes* is still VRF- or s&box-derived.

---

## 1. Five problems, one word

The taxonomy first, because the rest of the note is organised by it and because
getting this wrong is how a renderer ends up with one enormous glass shader that
is bad at everything.

| # | Case | What it physically is | What it actually needs | Cost |
|---|---|---|---|---|
| 1 | **Architectural pane** — window, partition, shopfront | one thin slab, two parallel interfaces | Fresnel-driven opacity + specular + a reflection probe. **No refraction.** | ~free |
| 2 | **Frosted / obscured** — bathroom glass, opal shade | the same slab, rough on one face | a blurred read of what is behind it, blur driven by roughness | needs a **scene-colour copy with mips** |
| 3 | **Breakable** — the pane that stops being one | a topology change, not a shading change | build-time fracture geometry + a per-shard vertex attribute the material can address | content pipeline, not renderer |
| 4 | **Vessel** — drinking glass, jar, bottle, windscreen | 4+ interfaces along one view ray, non-parallel | genuine refraction, correct **multi-layer compositing**, backface shading | needs the copy *and* an order-independent blend |
| 5 | **Liquid in a vessel** | a second medium with a free surface | a plane in object space, absorption by path length, and a spring | a shading problem — **not** a physics one |

The load-bearing observation, and it is the reason this note exists: **1 and 3
are the ones a tactical game needs, they are the two cheapest, and they are
solved by completely different subsystems** — one is six lines of pixel shader,
the other is a mesh operator in the model compiler and no renderer work at all.
Cases 2, 4 and 5 all queue behind one shared prerequisite (a scene-colour grab),
which is why they should be decided together or not at all.

---

## 2. The baseline, and the better version of it

### 2.1 What CS2 and Alyx ship

Recapped from [`source2_rendering.md`](source2_rendering.md) §12.1 so this note
stands alone. `GetGlassMaterial()`, VRF's reimplementation of the shared path in
`common/texturing.slang` **[VRF]**:

```glsl
// GLASS
// Edge fresnel glass, as CS2 and Alyx do it. Deadlock reuses the F_GLASS name for a
// transmission model that has none of these parameters, so it opts out.
#if ((F_GLASS == 1) || defined(glass_vfx_common)) && !defined(GLASS_IS_TRANSMISSIVE)

    uniform bool  g_bFresnel             = true;
    uniform float g_flEdgeColorFalloff   = 3.0;
    uniform float g_flEdgeColorMaxOpacity= 0.5;
    uniform float g_flEdgeColorThickness = 0.1;
    uniform vec3  g_vEdgeColor           = vec3(0.5, 0.8, 0.5);
    uniform float g_flRefractScale       = 0.1;

    vec4 GetGlassMaterial(MaterialProperties_t mat)
    {
        float viewDotNormalInv = clamp(1.0 - (dot(mat.ViewDir, mat.Normal)
                                              - g_flEdgeColorThickness), 0.0001, 1.0);
        float fresnel = saturate(pow(viewDotNormalInv, g_flEdgeColorFalloff))
                      * g_flEdgeColorMaxOpacity;
        vec4 fresnelColor = vec4(g_vEdgeColor.xyz, g_bFresnel ? fresnel : 0.0);

        return mix(vec4(mat.Albedo, mat.Opacity), fresnelColor, g_flOpacityScale);
    }
#endif
```

Valve documents the same three knobs on `VR Standard`, in artist language
**[VDC]**:

> **Glass** — Adds glass or water-like Fresnel transparency effects to the
> material. The shallower a the view angle, the more opaque the surface will be.
> *This effect can lead to increased sorting issues over standard translucency.*
>
> **Glass Fresnel exponent** — Controls how much the transparency decreases with
> shallower angles. Larger values pull the opaque area further out.
> **Glass Fresnel thickness** — F0 value. Control how opaque the material is
> when viewed head on.
> **Glass Fresnel max opacity** — Opacity multiplier.

Same maths in the VR shader and the CS2 shader, so this is a Source-2-wide
decision rather than a VR compromise. Note the parenthetical warning about
sorting — Valve are telling artists, in the shader reference, that glass is the
material most likely to sort wrong. §6 is about what happens when you take that
seriously.

### 2.1a A shipped `csgo_glass.vfx` material, in full

Valve's own example glass, uncompiled, from the CS2 authoring content **[CS2]**:

```
Layer0
{
	shader "csgo_glass.vfx"

	//---- Rendering ----
	F_DO_NOT_CAST_SHADOWS 1

	//---- Glass ----
	g_flTranslucencyRemap "[0.250 1.000]"
	GlassMaskColor        "materials/de_inferno/glass/inferno_glass_01_color.tga"
	GlassMaskTranslucency "materials/de_inferno/glass/inferno_glass_01_trans.tga"
	GlassMaskTransmission "[0.300000 0.300000 0.300000 0.000000]"
	GlassTintColor        "[1.000000 1.000000 1.000000 0.000000]"

	//---- Normal ----
	TextureNormal    "materials/de_inferno/glass/inferno_glass_01_normal.tga"
	//---- Roughness ----
	TextureRoughness "materials/de_inferno/glass/inferno_glass_01_rough.tga"

	SystemAttributes { PhysicsSurfaceProperties "glass"  WorldMappingWidth "128"  WorldMappingHeight "128" }
}
```

Five things fall out of a file this short:

- **There is no refraction parameter at all.** `g_flRefractScale` exists in the
  shader's parameter block **[VRF]** and is simply not written here. §12.1 of the
  rendering note inferred refraction was off for flat glass; this is the
  shipped material saying so. Confirmed rather than deduced.
- **`g_flTranslucencyRemap "[0.250 1.000]"`** — the min/max pair VRF describes,
  with real values. The translucency texture's alpha never reaches full
  transparency: 0.25 is the floor. A pane is always at least a quarter opaque.
- **`GlassMaskColor` / `GlassMaskTranslucency` / `GlassMaskTransmission` /
  `GlassTintColor`** are the artist-facing parameter names, and VRF does not
  have them — its reimplementation exposes the `g_v*`/`g_fl*` uniforms. So the
  authoring layer has a *mask* concept (dirt, frosting, printed patterns) with
  its own colour and translucency textures, separate from the tint.
- **`GlassMaskTransmission [0.3 0.3 0.3 0]`** — a neutral 30% transmission,
  which is the `TransmissiveMask` term in §2.3's lighting, not the alpha.
- **A normal map and a roughness map on flat architectural glass.** Both are
  present and both are per-texel. So CS2 glass is not the uniform sheet the
  Fresnel-ramp model on its own suggests — the variation carrying most of the
  realism is in the maps.

**`F_DO_NOT_CAST_SHADOWS 1` is the one to notice**, and §6.5 has to be read
against it. See there.

### 2.1b The whole shipped set, counted

All 53 `csgo_glass.vfx` materials in `pak01_dir.vpk`, decompiled **[CS2]**:

| Feature / parameter | Count |
|---|---|
| `g_tGlassDust` | **53 / 53** |
| `g_tGlassTintColor` | **53 / 53** |
| `F_DO_NOT_CAST_SHADOWS` | 28 / 53 |
| `TextureRoughness` | 25 / 53 |
| `GlassMaskTransmission` | 24 / 53 |
| `g_flTranslucencyRemap` | 22 / 53 |
| `F_RENDER_BACKFACES` | 13 / 53 |
| `F_VERTEX_BREAK_ANIMATION` | 5 / 53 |
| any refraction parameter | **3 / 53** (§4) |
| `F_TRANSLUCENT`, `F_SPECULAR_INDIRECT`, `F_TRANSMISSIVE_BACKFACE_NDOTL`, `F_SELF_ILLUM` | **0 / 53** |

Three readings.

**The dust layer is not optional — it is the shader.** Every single glass
material binds `g_tGlassDust` and `g_tGlassTintColor`. There is no clean-glass
path. [`source2_rendering.md`](source2_rendering.md) §12.1 identified "grime as a
layer, not a texture" as the thing making Alyx's glass read as real, inferred
from `vr_glass_markable`'s existence; this is CS2 shipping the same conclusion as
a **mandatory** texture slot on every pane in the game. If you build one thing
from this note's shading half, build the dust layer.

**Zero materials set `F_TRANSLUCENT`.** Transparency is intrinsic to
`csgo_glass.vfx` rather than a feature flag — which is what makes it a separate
shader from `csgo_complex` at all.

**Zero use `F_TRANSMISSIVE_BACKFACE_NDOTL`.** The engine's light-through-a-
surface term exists and is used elsewhere in the same set (17 materials on other
shaders), but never on glass. Coloured glass glowing where the sun is behind it
is a thing Source 2 can do and CS2 does not.

### 2.2 What the same engine does when someone writes it properly

s&box's `glass.shader` does not use a hand-tuned Fresnel power. It uses the
split-sum environment BRDF **[SBOX]**:

```hlsl
float  flNDotV  = saturate(dot(-m.Normal, vViewRayWs));
float3 vEnvBRDF = CalcBRDFReflectionFactor(flNDotV, m.Roughness, 0.04);
```

and `CalcBRDFReflectionFactor` is Valve's, in `common/BRDF.hlsl` **[SBOX]**:

```hlsl
float3 CalcBRDFReflectionFactor(float flNDotV, float flRoughness, float3 vSpecularColor)
{
    float2 vBRDFTerms = SampleBRDF(float2(flNDotV, flRoughness)).rg;
    vBRDFTerms.xy *= vBRDFTerms.xy;
    return vSpecularColor * vBRDFTerms.x + vBRDFTerms.y;
}
```

That is the same DFG lookup the opaque PBR path uses for image-based lighting,
evaluated with F0 = 0.04 — the dielectric value glass actually has. Everything
downstream is then `1 − vEnvBRDF`.

**Why this is the better model, and it is not aesthetics.** `pow(1 − N·V, k)`
is a curve someone picked; the DFG lookup is *the integral of the specular lobe
over the hemisphere*, so `1 − vEnvBRDF` is the fraction of energy that was not
reflected, correct at every roughness. Three consequences fall out for free:

- **Roughness affects transmission.** In the CS2 model the Fresnel opacity ramp
  is roughness-independent, so a rough pane and a polished pane transmit
  identically and only their highlights differ. That is wrong, and it is exactly
  the case frosted glass lives in.
- **Reflectance and transmittance sum to one by construction**, rather than by
  the artist not setting `g_flEdgeColorMaxOpacity` too high.
- **The grazing behaviour is not a tuning parameter.** It comes out of the LUT.
  You lose `g_vEdgeColor` — the grazing tint — but that was always a stylisation
  rather than a physical term.

**Our read [inferred]:** the CS2 model is the right *shape* and the wrong
*source* for the number. If a project already has a split-sum DFG LUT for IBL —
and any PBR renderer does — then glass opacity should be `1 − DFG(N·V,
roughness)` and the three `EdgeColor` knobs collapse into the roughness map that
already exists. This project's `transparent.fs.glsl` currently ships the CS2
form, with `edgeFalloff` / `edgeMaxOpacity` / `edgeThickness` in `window.mat`;
swapping the ramp for the LUT is a two-line change and deletes three material
parameters. See §10.

### 2.3 The one Alyx-only subtlety, still worth taking

Repeated here because it is load-bearing for everything below. Ordinary alpha
blending multiplies the *whole* shaded result by opacity, which scales the
specular highlight by transparency — so the clearer the glass, the weaker its
reflection. Backwards. Alyx's `F_TRANSLUCENT == 2` "membrane" mode premultiplies
only the diffuse and forces `a = 1.0`, leaving specular at full strength
**[VRF]**. s&box's layered mode (§6) reaches the same conclusion by a different
route and states it in a comment: *"refraction is pre-scaled by alpha,
reflections stay additive."* **[SBOX]**

Two independent implementations converging on "the reflection is not attenuated
by the transparency" is about as strong a signal as this kind of archaeology
produces.

---

## 3. Reflections — a three-rung ladder, and glass rides all three

The user-visible question "why does Source 2 glass look like it is *in* the
room" has a boring answer: because the reflection is a room, not a sky. Source 2
composites three sources of specular reflection, in this order.

### 3.1 Rung one — cubemaps, iterated and feathered

Source 2 does not pick the nearest cubemap. It walks a list of environment maps
and accumulates coverage until the pixel is full, from `vr_lighting.fxc`
**[SBOX]**:

```hlsl
vCubeMapTexel_Specular = lerp( vCubeMapTexel_Specular, vCubeMapTexel, 1.0 - flDistAccumulated );
…
flDistAccumulated += RemapValClamped( flDistance, min( -flEdgeFeathering, 0.0f ),
                                                  max( -flEdgeFeathering, 0.0f ), 0.0, 1.0 );

// Keep iterating until we've accumulated enough to fill this pixel
if( flDistAccumulated >= 1.0 )
    break;
```

Each cubemap has a signed distance to its bounds and an **edge-feathering**
width; a pixel inside two overlapping probes blends them by how deep inside each
it sits, and only stops when it has full coverage. Box projection is applied to
the *specular* reflection vector but deliberately **not** to the diffuse IBL —
there is a Facepunch comment in the file saying parallax on diffuse IBL "causes
issues with non-uniform scaled envmaps for minimal gains" **[SBOX]**.

This is why Valve's cubemap placement guidance for glass is as tight as it is
(a probe serving a pane goes *in* the room the pane looks into) — the feathering
means a badly placed probe does not simply lose, it bleeds.

### 3.2 Rung two — SSR, composited by confidence

```hlsl
if( DynamicReflections::IsEnabled() )
{
    float4 vDynamicReflection = DynamicReflections::Sample( vPositionSS.xy, vSQRTRoughness.x );
    float  flCompositeConfidence = vDynamicReflection.a;
    vDynamicReflection.rgb *= CalcBRDFReflectionFactor( flNDotV, flIsotropicRoughness, … );
    vCubeMapTexel_Specular.rgb = lerp( vCubeMapTexel_Specular.rgb,
                                       vDynamicReflection.rgb, flCompositeConfidence );
}
```

Three things worth stealing verbatim **[SBOX]**:

1. **SSR never stands alone** — it is lerped *over* the cubemap result by the
   alpha the SSR pass wrote, which encodes per-pixel confidence (ray hit / ray
   left the screen / ran out of steps). Identical structure to
   `csgo_water_fancy`'s `F_REFLECTION_TYPE` ladder in
   [`source2_rendering.md`](source2_rendering.md) §12.2, arrived at from the
   other side.
2. **Roughness selects a mip of the SSR buffer**, and the accessor is written to
   cope with the buffer having no mips at all: `flLevel = Roughness *
   (nLevels − 1)`. The comment names the intended second consumer — *"Eg Planar
   Reflections with mip chain"*.
3. The whole thing hides behind `DynamicReflections::IsEnabled()`, which is a
   bindless-slot test — the shader does not know or care whether the buffer came
   from SSR or from something else.

**The problem nobody solves, and it applies to us [inferred].** SSR is computed
from the *opaque* depth and normal buffers. Glass draws in the forward
translucent pass, after that. So a glass pixel samples the SSR buffer at its own
screen position — where the buffer holds the reflection computed for whatever
opaque surface is *behind* the glass. It is wrong, it is cheap, and at glass's
typical roughness (near zero) it is wrong in the most visible possible way.
Nothing in the code guards against it. Treat "SSR on glass" as a known lie
rather than a feature to reproduce carefully.

### 3.3 Rung three — planar, and an honest caveat

CS2's FGD still carries **[VDC]**:

```
@SolidClass base(func_brush) = func_reflective_glass :
	"Used to produce perfectly reflective glass that renders world + entities. "
	"Typically, you want your glass brush to have nodraw on all non-reflective surfaces "
	"and you want to use a shader like lightmappedreflective in your material applied "
	"to the non-nodraw surfaces. See hl2/materials/glass/reflectiveglass001.vmat for an example. "
	"NOTE: currently, you cannot use reflective glass in scenes with water, and you can only "
	"have 1 reflective glass in your view frustum ( + pvs ) at a time."
```

Read this carefully before believing it. The text names `lightmappedreflective`
and an `hl2/materials/...` path — that is **Source 1 documentation carried
across into the Source 2 FGD**, with `.vmt` mechanically renamed to `.vmat`. The
entity is present in CS2's entity definitions; whether it is *implemented* in
CS2 is not established by this evidence. The constraint it states is the real
one for planar reflections in any engine — **one per frustum, mutually exclusive
with water** — because each one costs a full scene re-render from the mirrored
camera.

Corroborating hint on the other side: s&box's SSR accessor explicitly
anticipates planar reflections feeding the same buffer with a mip chain (§3.2).
So the *slot* for planar reflection exists in modern Source 2 even if
`func_reflective_glass` is a fossil. **[inferred]**

### 3.4 The ladder, as a decision

| Rung | Cost | What it is right for |
|---|---|---|
| Reflection probe | one cubemap fetch, already paid for by opaques | **everything.** A pane with a probe reflection already reads as glass. |
| SSR over probe | a screen-space trace pass | large flat panes at grazing angles, where the probe's parallax error shows |
| Planar | a whole extra scene render | one hero mirror, and only if the design names it |

For a tactical camera looking down at a board, rung one is the entire answer and
the other two are not close to earning their cost.

---

## 4. Refraction, when it is actually warranted

§12.1 of the rendering note argued CS2 is right to omit refraction on flat
panes: two parallel interfaces cancel the bend, leaving ~2 mm of displacement at
3 m — about a pixel — and *uniform* displacement is invisible because there is
no reference to judge it against. That argument holds. It also tells you exactly
when it stops holding: **curvature, varying thickness, or a moving surface**.

**And the shipped material set proves the rule by its exceptions.** Of CS2's
**53** `csgo_glass.vfx` materials, exactly **three** carry a refraction
parameter **[CS2]**:

| Material | What it is |
|---|---|
| `models/cs_italy/interior/kitchen/wine_bottles_glass_1` | **wine bottles** — `PhysicsSurfaceProperties "glassbottle"` |
| `materials/models/props_junk/glass_objects01` | jars and bottles |
| `models/vehicles/airplane_medium_01/airplane_medium_01_glass` | an **aircraft canopy** |

Bottles, jars and a windscreen. §1's tier-2 list was written before this was
read and named "bottles, jars, windscreens, domes"; the shipped set is that
list, including literal wine bottles. The other fifty — every window, partition,
shopfront and safety pane — have no refraction parameter at all.

**The surprise is how they do it.** Not screen-space:

```
	m_intParams   [ { m_name = "F_OPAQUE_CUBEMAP_REFRACTION"  m_nValue = 1 } ]
	m_floatParams [ { m_name = "g_flRefractMinRoughness"      m_flValue = 0.1 } ]
```

**CS2 refracts through the cubemap, not the frame buffer.** The reflection probe
is sampled a second time along the *refracted* ray, and `g_flRefractMinRoughness`
gates the effect below a roughness threshold. That buys three things a
screen-space grab cannot: it works at any bend angle because a cubemap has no
edges, it needs no scene-colour copy, and it is correct for a *closed* object
where the frame buffer has nothing useful behind the glass anyway. What it
cannot do is show the actual room geometry displaced — you get the probe's
version of the world, which for a hand-sized bottle is indistinguishable.

Two more details from the same three materials: `glass_objects01` sets
**`F_RENDER_BACKFACES 1` together with `F_DONT_FLIP_BACKFACE_NORMALS 1`** — the
vessel case (§6), and CS2 explicitly *disables* the backface normal flip that
s&box's 2026 revision explicitly *added*. Opposite calls on the same problem,
which is a fair signal that the right answer depends on how the artist authored
the mesh rather than on physics.

**This also rewires §8.** Valve's account of Alyx's bottles is that they use
cubemaps to fake both reflection *and* refraction; `F_OPAQUE_CUBEMAP_REFRACTION`
is that idea shipping in a desktop title, on wine bottles, as a shader feature.
The Alyx liquid shader is much less of an outlier than it looked.

**A fourth case, on a different shader.** CS2's glass *blocks* —
`de_cache/glass/ch2_glassblock_02` and four siblings — are not
`csgo_glass.vfx` at all. They are **`csgo_simple_3layer_parallax.vfx`**, with
`g_flLayer1RefractionAmount` / `g_flLayer2RefractionAmount` and
`g_flLayer{1,2}RoughnessBlurMin/Max` **[CS2]**. Thick distorting glass is
handled as a *parallax-layered* material — depth faked by layer offset — rather
than by either refraction path. Three different techniques for three thicknesses
of glass, chosen per material.

s&box implements the screen-space case, and the implementation is compact enough
to read in full **[SBOX]**:

```hlsl
float  flDepthPs   = 1.0f - Depth::GetNormalized( i.vPositionSs.xy );
float3 vRefractionWs = RecoverWorldPosFromProjectedDepthAndRay(flDepthPs, vViewRayWs)
                     - g_vCameraPositionWs;
float  flDistanceVs = distance(i.vPositionWithOffsetWs.xyz, vRefractionWs);

float3 vRefractRayWs      = refract(vViewRayWs, m.Normal, 1.0 / g_flRefractionStrength);
float3 vRefractWorldPosWs = i.vPositionWithOffsetWs.xyz + vRefractRayWs * flDistanceVs;

float4 vPositionPs = Position4WsToPs(float4(vRefractWorldPosWs, 0));
float2 vPositionSs = vPositionPs.xy / vPositionPs.w;
vPositionSs = vPositionSs * 0.5 + 0.5;
vPositionSs.y = 1.0 - vPositionSs.y;
```

Four things in there are worth more than the technique itself.

**1. The refracted ray is marched to the actual depth of what is behind.** Most
screen-space refraction is `uv += normal.xy * scale` — a 2D smear with no
geometry in it. Here the shader reads the depth buffer, reconstructs the world
position of the surface behind the glass, measures the distance to it, walks the
refracted ray *that far*, and re-projects. So the screen-space offset scales
with how far away the background is: a wall pressed against the pane barely
shifts, a corridor receding behind it shifts a lot. **That distance-proportional
offset is the entire reason it reads as a solid transparent object rather than a
wobble filter.**

**2. The index of refraction is a lie, on purpose.** `g_flRefractionStrength`
has `Default(1.005)` and `Range(1.0, 1.1)`. Real glass is 1.52; water 1.33.
A range topping out at 1.1 is a tenth of the way to glass. The reason is
structural: a big bend sends the sample off-screen or behind an occluder, where
a screen-space technique has no data. So the artist gets a knob that can only be
turned to "slightly", and the shader stays inside the region where the frame
buffer can answer the question. Anyone implementing screen-space refraction
should copy the *clamp*, not the physics.

**3. Orthographic projection is a special case, and it is handled twice.**

```hlsl
bool bOrtho = g_matViewToProjection[3].w != 0;
float3 vViewRayWs = bOrtho ? g_vCameraDirWs : normalize(i.vPositionWithOffsetWs.xyz);
…
if (bOrtho)
    vPositionSs = i.vPositionSs.xy * g_vInvViewportSize.xy;   // no offset at all
```

Under ortho every view ray is parallel to camera forward, so the perspective
`normalize(positionRelativeToCamera)` is simply wrong; and re-projecting the
refracted point produces artefacts, so the shader **gives up and samples
straight through**. Worth knowing before wiring refraction into an isometric
game: for the projection this project uses, the honest implementation of
screen-space refraction is *to not do it*.

**4. Off-screen is handled by mirroring, not clamping.** The frame-buffer copy
is sampled through `g_sTrilinearMirror`; the 2024 revision set `AddressU(MIRROR)`
/ `AddressV(MIRROR)` explicitly. Clamp streaks the edge pixel into a smear that
reads as a bug; mirror folds plausible content back in and reads as nothing.
**[SBOX]**

### 4.1 The engine-level hook: `bWantsFBCopyTexture`

```hlsl
BoolAttribute(bWantsFBCopyTexture, S_GLASS_QUALITY != GLASS_CHEAP );
Texture2D g_tFrameBufferCopyTexture < Attribute("FrameBufferCopyTexture"); SrgbRead(false); >;
```

A shader **declares** that it needs a copy of the scene colour, and the engine
arranges one before drawing anything using that shader. The scale/size uniform
lives in Valve's own `common.fxc`:

```hlsl
float4 g_vFrameBufferCopyInvSizeAndUvScale;   // core/shaders/common.fxc:85
```

Two facts follow. The copy is an **engine facility, not a game one** — it is
Valve's uniform in Valve's header. And it carries a *UV scale* separate from its
inverse size, meaning the copy is not assumed to be viewport-sized: it can be a
half-res target or a sub-rectangle, and shaders must scale their UVs into it.
The same texture is what the UI's backdrop-blur shader reads
(`ui_backdropfilter.shader`, `BoolAttribute(bWantsFBCopyTexture, true)`) — so
frosted glass and CSS `backdrop-filter` are the same resource in this engine.
**[SBOX]**

---

## 5. Frosted glass — two answers, and CS2's is the cheap one

### 5.0 What CS2 actually ships

Before the clever version, the shipped one. `ar_baggage/baggage_glass_frosted_01`
**[CS2]**, in full — this is CS2's frosted glass:

```
	m_shaderName = "csgo_glass.vfx"
	m_intParams    [ F_RENDER_BACKFACES = 1, g_bFogEnabled = 1, … ]
	m_floatParams  [  ]
	m_vectorParams [ GlassMaskTranslucency = [0.25, 0.25, 0.25, 0]
	                 TextureRoughness      = [0.039216, …] ]
	m_textureParams
	[
		g_tGlassDust      → materials/glass/hr_g/hr_glass_frosted_001_color…
		g_tGlassTintColor → materials/glass/hr_g/hr_glass_frosted_001_color…
		g_tNormal         → …baggage_glass_frosted_01_vmat_g_tnormal…
	]
```

**Same shader as clear glass. No blur. No refraction. No frost feature.** The
frost is a *texture* in the dust slot, plus a constant `GlassMaskTranslucency`
of 0.25 pushing the pane toward opaque, plus a normal map, plus
`F_RENDER_BACKFACES` so you see the far side of the volume through it.

That is worth sitting with. The entire visual of frosted glass, in a shipping
Valve title, is **an authored texture in the grime channel**. Nothing behind the
pane is blurred at all — the pane is simply opaque enough that nobody notices
there is a sharp image behind it. The `csgo_glass.vfx` shader has no blur path
to disable, because it was never built.

**[inferred]** This is the same trade as §4: CS2 declines the expensive
mechanism and buys the *perception* with authoring. It works because frosted
glass is mostly opaque, and a mostly-opaque surface hides the fact that its
small transmitted component is wrong. It stops working the moment the glass is
clear enough to see shapes through — which is exactly where the next section's
machinery starts earning its cost.

### 5.1 The version that actually blurs

This is the case with the smallest gap between "looks impossible" and "is six
lines", and it is worth writing out in full because every term is doing
something **[SBOX]**:

```hlsl
{
    float flAmount = g_flBlurAmount * m.Roughness * (1.0 - (1.0 / flDistanceVs));

    // Isotropic blur based on grazing angle
    flAmount /= flNDotV;

    const int nNumMips = 7;
    float2 vUV = float2(vPositionSs) * g_vFrameBufferCopyInvSizeAndUvScale.zw;

    vRefractionColor = g_tFrameBufferCopyTexture.SampleLevel( g_sTrilinearMirror, vUV,
                                                              sqrt(flAmount) * nNumMips );
}
```

Term by term:

- **`m.Roughness`** — the physical driver. A rough interface scatters the
  transmitted ray into a lobe instead of a line; the lobe's width is the blur
  radius. This is the BTDF's roughness, and using the same roughness map that
  drives the reflection is correct: one surface, one microfacet distribution,
  both lobes widen together.
- **`(1 − 1/flDistanceVs)`** — distance to what is behind, from §4's depth
  reconstruction. A scattering lobe of fixed angular width covers more *area*
  the further it travels, so background further from the pane blurs more. This
  is the term that makes frosted glass read as having depth behind it rather
  than as a smeared decal. It goes to zero as the background approaches the
  glass, which is right: something pressed against frosted glass is sharp, and
  everyone has seen that.
- **`/= flNDotV`** — at grazing incidence the ray's path *through* the slab is
  longer by `1/cos θ`, so it scatters more. One divide for a real effect. (It is
  also a divide by a `saturate`d value that can reach zero at the silhouette;
  the `sqrt` and the mip clamp save it, but it is the kind of thing that shows
  up as a fireflying edge pixel on some hardware.)
- **`sqrt(flAmount) * 7`** — mip level is `log2` of the footprint, so mapping a
  linear "amount" to a mip index needs *some* compressive curve; `sqrt` is the
  cheap one that puts most of the artist's slider range in the visible part.
  Seven mips is a 128× reduction — enough that the top of the chain is
  effectively "the average colour of the room", which is exactly what heavily
  frosted glass shows you.

### 5.2 The other frosting knob, which is not blur

Blur alone gives you *obscured* glass. It does not give you *milky* glass. The
second knob does:

```hlsl
m.Emission  = lerp( vRefractionColor.xyz, 0.0f, vEnvBRDF );
m.Emission *= m.Albedo * (1.0 - m.Roughness * AlbedoAbsorption);
m.Albedo   *=            m.Roughness * AlbedoAbsorption;
```

Read it as an energy split. `m.Albedo` is the glass tint. The first line is
transmission = background × (1 − reflectance) — the DFG complement from §2.2.
The next two lines then *divide the tint between two roles* by
`Roughness × AlbedoAbsorption`: the fraction that stays as transmission tint,
and the fraction that is handed back to `m.Albedo` so the standard shading model
lights it **as an ordinary diffuse surface**.

At `AlbedoAbsorption = 0` the glass is clear and tinted. At `1` with high
roughness the tint has migrated entirely into a lit diffuse term and the surface
is opal — it *catches light on itself* rather than passing light through. One
parameter, and it is the difference between a tinted window and a frosted
bathroom pane. **[SBOX]**

The two knobs are orthogonal and both are needed: blur is what happens to what
is behind, absorption is what happens on the surface. Frosted glass with only
blur looks like a dirty window; with only absorption it looks like plastic.

### 5.3 Where the mip-chain cheat fails

Honest list, because it will fail the same way in any engine **[inferred]**:

- **No depth awareness.** A mip is a box filter over the frame buffer, so a
  bright object *in front of* the glass, or a silhouette edge, bleeds across
  into the blurred read. The standard fix (depth-weighted separable blur) costs
  a real pass, and nobody does it for glass.
- **The copy is pre-glass.** Everything drawn in the translucent pass before
  this pane is *not* in the copy. Two frosted panes in a row: the second one
  blurs the scene as if the first were not there. §6 has the same problem with
  the same cause.
- **Mips are viewport-resolution, so the blur radius is resolution-dependent** —
  the same material looks different at 1080p and 4K unless the mip index is
  compensated. Nothing in the shader compensates it.

None of these matter for one bathroom window seen from four metres, which is
what the technique is for.

---

## 6. The drinking glass — two panes, and the blend mode that makes it work

This is the case the user asked about that has the most interesting answer, and
the one where having **two dated revisions of the same shader on disk** turns
guesswork into a record.

### 6.1 Why a vessel is a different problem

A window is one interface pair. A drinking glass, along a single view ray from
the outside in, is **four** surfaces: outer-front, inner-front, inner-back,
outer-back. Every one of them reflects, every one of them tints, and they are
not parallel — the front wall's bend does not cancel against the back wall's,
which is precisely the condition §4 says makes refraction visible.

Alpha blending handles that badly, and specifically:

- **The background is counted more than once.** Each layer that samples the
  frame-buffer copy re-introduces the scene behind the whole object, then blends
  it over a destination that already contains it.
- **Tints add instead of multiplying.** Two panes of green glass should multiply
  to a darker green (Beer–Lambert). `SRC_ALPHA, INV_SRC_ALPHA` interpolates
  toward the second tint instead.
- **Order matters, and there is no order.** A single mesh drawn two-sided
  produces its front and back faces in whatever order the index buffer and the
  rasteriser feel like. Per-triangle sorting is not on the table.

### 6.2 What changed between 2024 and 2026

`diff` of the two revisions on disk **[SBOX]**:

```diff
-Feature( F_GLASS_QUALITY, 0..1( 0 ="Default Glass ( Refractive, Tinted )",
-                                1 = "Simple Glass ( Faster To Render )" ), "Glass");
+Feature( F_GLASS_QUALITY, 0..2( 0 ="Default Glass ( Refractive, Tinted )",
+                                1 = "Simple Glass ( Faster To Render )",
+                                2 = "Layered Glass ( Multi-Layer Compositing )" ), "Glass");
```

```diff
+    #if ( PROGRAM == VFX_PROGRAM_PS )
+        bool bIsFrontface : SV_IsFrontFace;
+    #endif
…
+RenderState( CullMode, F_RENDER_BACKFACES ? NONE : DEFAULT );
…
+        // refractive glass freaks out on backfaces if normals aren't flipped
+        if ( !i.bIsFrontface )
+        {
+            m.Normal = -m.Normal;
+        }
```

An engine's glass shader growing a two-sided path, a backface normal flip and a
multi-layer compositing mode in one revision is a shader discovering vessels.
The 2024 version is a window shader; the 2026 version is a window shader that
has been asked to draw a bottle.

### 6.3 The layered blend, and why it is nearly order-independent

The render state:

```hlsl
#if (S_GLASS_QUALITY == GLASS_LAYERED)
    // Premultiplied alpha: refraction is pre-scaled by alpha, reflections stay additive.
    // This allows multiple glass layers to composite their tints correctly.
    RenderState(BlendEnable, true);
    RenderState(SrcBlend, ONE);
    RenderState(DstBlend, INV_SRC_ALPHA);
    RenderState(SrcBlendAlpha, ONE);
    RenderState(DstBlendAlpha, INV_SRC_ALPHA);
    RenderState(BlendOpAlpha, ADD);
#endif
```

and the shading:

```hlsl
float3 vTransmission = saturate( ( 1.0f - vEnvBRDF ) * vOriginalAlbedo
                                 * ( 1.0f - m.Roughness * AlbedoAbsorption ) );
float  flTransmittanceFloor = min( vTransmission.x, min( vTransmission.y, vTransmission.z ) );
float  flGlassAlpha         = 1.0f - flTransmittanceFloor;

// Move only the residual (channel-specific) transmission into source;
// the common transmission floor is handled by destination blend weight.
m.Emission = vRefractionColor.xyz * max( vTransmission - flTransmittanceFloor.xxx, 0.0f );
m.Opacity  = saturate( flGlassAlpha );
```

**The trick, stated plainly.** The blend evaluates to

```
dst' = src + dst · (1 − alpha)  =  src + dst · transmittanceFloor
```

`transmittanceFloor` is a **scalar multiply** applied to everything already in
the frame buffer — including any glass layer drawn earlier. Multiplication
commutes. So the grey (channel-common) part of each layer's transmittance
composites **correctly regardless of draw order**, which is precisely the
Beer–Lambert behaviour §6.1 says alpha blending gets wrong. Stack four surfaces
of a drinking glass in any order and the darkening is right.

The colour that is *not* common to all three channels — the residual — cannot be
expressed as a scalar destination weight, so it is moved into the source term
and added using this layer's own read of the frame-buffer copy. That part is
approximate, and the approximation has a name: **for a neutral tint the residual
is zero and the composite is exact; the more saturated the glass, the more the
background is double-counted across layers.** Green bottle glass will be
brighter than it should be through four surfaces. Clear or lightly-tinted glass
is essentially correct. **[inferred]**, from reading the algebra.

This is a genuinely clever piece of engineering and it is the single most
transferable idea in this note: **you can buy order-independent transparency for
the multiplicative part of a translucent surface for free, by putting the
transmittance in the destination blend weight instead of in the source colour.**
No sorting, no per-pixel linked list, no depth peeling. It only fails on the
chromatic residual, which is small for the material everyone actually wants.

### 6.4 What layered mode still does not fix

- **One frame-buffer copy for the whole pass.** Every layer refracts the
  *pre-glass* scene. The back wall of a bottle does not see the front wall's
  refraction, so you get correct *tinting* through four surfaces and only
  first-surface *distortion*. Depth peeling is the real answer and nobody pays
  for it.
- **No sorting inside a mesh.** With `CullMode NONE` the two faces of one
  triangle strip come out in index order. The blend makes the tint
  order-independent, which is what makes this survivable; the additive
  reflection is order-independent too. What is left order-dependent is the
  residual and anything the shading model does non-linearly.
- **Reflection is per-layer and additive**, so four surfaces produce four
  Fresnel rims. That is physically right — a real glass has a bright rim at
  every silhouette, inner and outer — but it means the material's overall
  brightness scales with how many surfaces the artist modelled. Modelling a
  drinking glass as a single-walled cylinder versus a double-walled one changes
  its look in a way nothing in the material can compensate for.

### 6.5 The depth pass — glass casts a shadow derived from its own BRDF

Not obvious, and easy to miss because it sits behind `#if S_MODE_DEPTH` at the
top of the pixel shader **[SBOX]**:

```hlsl
#if S_MODE_DEPTH
{
    float flOpacity = CalcBRDFReflectionFactor(dot(-i.vNormalWs.xyz, g_vCameraDirWs.xyz),
                                               m.Roughness, 0.04).x;
    flOpacity = pow(flOpacity, 1.0f / 2.0f);
    flOpacity = lerp(flOpacity, 0.75f, sqrt(m.Roughness));                                    // Glossiness
    flOpacity = lerp(flOpacity, 1.0 - dot(-i.vNormalWs.xyz, g_vCameraDirWs.xyz),
                     ( g_flRefractionStrength - 1.0f ) * 5.0f );                              // Refraction
    flOpacity = lerp( 1.0f, flOpacity , ( length(m.Albedo) * 0.5f ) + 0.5f );                 // Albedo absorption
    OpaqueFadeDepth(flOpacity, i.vPositionSs.xy);
    return 1;
}
#endif
```

In the shadow pass the "camera" is the light, so `dot(-N, g_vCameraDirWs)` is
N·L. Read the four lines as a policy: **glass blocks the light it reflects**
(the DFG term), **rough glass tends toward mostly-opaque** (0.75), **refractive
glass blocks more at grazing angles**, and **dark tint pushes toward fully
opaque**.

Then `OpaqueFadeDepth` — and this one *is* Valve's, in `vr_lighting.fxc`:

```hlsl
void OpaqueFadeDepth( float flOpacity, float2 vPositionSs )
{
	float flNoise = g_tBlueNoise.Load( int3( vPositionSs.xy % TextureDimensions2D( g_tBlueNoise, 0 ).xy, 0 ) ).g;
	clip( mad( flOpacity, 2.0, -1.5 ) + flNoise );
}
```

Stochastic transparency against a screen-tiled blue-noise texture. Work the
algebra: the fragment survives when `noise > 1.5 − 2·opacity`, so coverage is
`2·opacity − 0.5` — **the usable range is opacity ∈ [0.25, 0.75]**, saturating
outside it. That is why the roughness line above lerps toward exactly `0.75`:
0.75 is this function's "fully opaque".

The payoff: **glass casts a partial shadow, for free, with no sorting, no
translucent shadow map and no second pass** — the shadow map's own PCF filtering
turns the dithered coverage into a grey shadow. Valve's own standard shader uses
the same function to emulate alpha-to-coverage when MSAA is off
(`vr_shared_standard_ps_code.fxc:314`), so this is an engine idiom being reused,
not a one-off.

For a game with a sun and a lot of windows this is a better answer than anything
involving a coloured translucent shadow map, and it is about eight lines.

**And CS2 treats it as a per-material decision, roughly half and half.**
`F_DO_NOT_CAST_SHADOWS 1` is set on **28 of 53** shipped glass materials — both
of the breakable-glass example's, but not, for instance, the frosted baggage
pane **[CS2]**. So CS2 glass does cast shadows about half the time, and an
artist turns it off per material.

That is the useful shape, and it is more informative than either extreme
**[inferred]**. A faint shadow costs a depth-pass draw per pane and can put a
noisy grey patch where a player model needs to be readable — so in a competitive
shooter it is switched off wherever it does not pay. **Take the technique from
s&box and take CS2's off switch just as seriously**: this belongs in the material
as a flag, not in the shader as a property. Our `.mat` format already has the
right shape for it.

---

## 7. Breakable glass — the renderer does nothing, the model compiler does everything

The most surprising finding in this note: **Source 2's breakable glass is not a
rendering feature.** There is no crack shader, no runtime fracture, no dynamic
mesh generation. There is an entity, a mesh operator that runs at asset build
time, and a vertex attribute.

### 7.1 The entity

`func_shatterglass` exists in **Dota 2, SteamVR Home, Half-Life: Alyx** **[VDC]**
and s&box, which makes it engine-level rather than a per-game addition. CS2's
FGD, verbatim **[VDC]**:

```
@SolidClass base(PhysicsTypeOverride_Mesh, Targetname, Parentname, Global, EnableDisable, Shadow)
    = func_shatterglass : "A procedurally-shattering glass panel."
[
	input  Hit(void)      : "Damages the panel"
	input  Shatter(void)  : "Completely breaks the panel"
	input  Restore(void)  : "Restore the panel"
	output OnBroken(void) : "Fires when the panel first breaks."

	GlassNavIgnore(boolean)     : "Nav Ignore"             : "1"
	GlassThickness(float)       : "Thickness"              : "0.6"
	GlassInFrame(boolean)       : "Is in a frame"          : "1"
	SpawnInvulnerability(float) : "Spawn Invulnerability"  : "3"
	StartBroken(boolean)        : "Start Broken"           : "0"
	BreakShardless(boolean)     : "Start Break Shardless"  : "0" : "If starting broken, have no shards"
	DamageType(choices)         : "Damage Type"            : 1 = [ 0:"Bullet" 1:"Melee" 2:"Thrown" 3:"Script" 4:"Explosive" ]
	DamagePositioningEntity  … 01 through 04 …             : "If set, this helper position is used for Hit input damage."
	surface_type(choices)       : "Surface Type"           : 0 = [ 0:"Glass" 1:"Concrete" ]
]
```

Every default in there is a design decision worth reading:

- **`GlassNavIgnore` defaults to 1.** Glass is not an obstacle for pathfinding.
  A bot walks the route as though the window were not there, which is correct
  for a material that stops existing when shot, and saves regenerating the nav
  mesh on break.
- **`SpawnInvulnerability : 3`** — three seconds of immunity at spawn. That
  parameter exists because round-start grenades and spawn-adjacent gunfire were
  destroying map glass before anyone could see it. It is a gameplay fix living
  in a rendering-adjacent entity.
- **`GlassInFrame` defaults to 1** — a framed pane and an unframed sheet break
  differently at the boundary (a frame holds the edge shards in place; a free
  sheet loses them). Two break behaviours, one boolean.
- **Four `DamagePositioningEntity` slots.** A mapper can script exactly where
  the cracks originate — a scripted break looks staged if the damage centre is
  the pane's centroid, and this is the escape hatch.
- **`Restore`** as an input. The panel is a state machine (whole → damaged →
  shattered) that can be driven backwards, which means the shard geometry is
  *persistent data*, not something spawned and forgotten.
- **`surface_type` includes Concrete.** The "shattering panel" machinery is not
  glass-specific; it is a generic brittle-panel entity.

`Damage Type` defaulting to **Melee** with `Bullet` at 0 is a small tell about
which title's needs the defaults were tuned for.

### 7.2 The content pipeline, from Valve's shipped model

`breakable_glass_01.vmdl` **[CS2]** is KV3 text on disk, so this is the pipeline
exactly as Valve authored it rather than as a tutorial describes it. The tree,
with every value that matters:

**The intact pane** — `RenderMeshList` → `RenderMeshFile "window"` → `Mesh
Operation Stack` → **Box Primitive** `box_dimensions = [0.75, 40.0, 76.0]` →
**UV Map Planar Custom** into `texcoord$0`. No source file
(`filename = ""`) — the geometry is generated inside the model compiler.
Collision is `PhysicsHullFromRender`, `surface_prop = "glass"`,
`collision_prop = "window"`. `prop_data` carries `base = "Glass.Window"`,
`health = 1.0`, `allowstatic`, `bakelighting = true`, and
`dmg.bullets / dmg.club / dmg.explosive / dmg.fire = -1.0`.

**The broken pane** — `BreakPieceList` → `BreakPieceEmbedded` → `RenderMeshFile
"window_broken"`, whose operation stack is a **nest**, not a list. Each operator
is the *child* of the previous one, so it applies to that operator's output:

| Operator | Values as shipped | What it does |
|---|---|---|
| **Voronoi Plane Primitive** | `box_dimensions [0, 40, 76]` (x = 0 — a flat plane), `num_points 64`, `rnd_seed 2`, `weld_distance 0.5`, `bias_to_center 0.1`, `shrink_pieces 0`, `extra_border_points 1`, `add_center_vert false`, material → `..._broken.vmat` | slices the pane into 64 Voronoi cells |
| ↳ **Inflate** | `amount 0.75`, `build_sides true`, `build_backfaces true`, `invert_normals false` | turns each flat cell into a 0.75-unit-thick closed solid |
| ↳ **Select in Box** | `box_dimensions [10, 39.094, 74.569]`, `select_vertices true`, `select_edges false`, `selection_mode "selection_add"` | selects vertices **inside** a box slightly smaller than the 40 × 76 pane |
| ↳ **Vertex Sine Wave** | `wave_length [0, 0.4, 0.4]`, `wave_amplitude [0, 1.0, 1.0]` | buckles the selected vertices |
| **Select All** → **UV Map Planar Custom** | into `texcoord$0` | re-UVs the shards against the broken texture |
| **Assign Vertex Pivot Data** | `output_stream_name = "texcoord$4"` | writes each shard's pivot into **UV channel 4** |

The tutorial page left *"Box Select / Vertex sine wave for distortion"* as a
`{{todo}}` **[VDC]**. Here it is, and it is the detail that makes the technique
work: the sine wave has zero amplitude on X (the thin axis) and 1.0 on Y and Z
at a 0.4-unit wavelength, and the selection box is **inset from the pane's
edges** — 39.09 × 74.57 inside 40 × 76. So the shards in the *middle* of the
pane are buckled and the ones at the *border* are not. That is what stops
shattered glass reading as a flat mosaic of tiles, and it is why the border
shards still line up with the frame.

`BreakPieceEmbedded` itself: `spawn_entity = "prop_physics"`,
`collision_group = "debris"`, `fadetime = 3.0`, `piece_motion_disabled = true`,
`piece_health = 0`, `is_essential_piece = true`,
`can_player_pickup_piece = false`, `material_group_mode = "inherit_owner_index"`.

Its collision is the sharpest economy in the file: **`PhysicsHullFromPoints`
with a single `PhysicsHullPoint` of `point_type = "tetra"`**, and
`collision_prop = "default_player_movement_exclude"`. Every shard — whatever
shape the Voronoi gave it — collides as **one tetrahedron that players walk
through**. Not a hull of the shard. Not a box. One tetra, excluded from player
movement.

### 7.3 The runtime: a vertex shader, four numbers, and no physics

The broken material settles what "dynamic expressions" meant **[CS2]**:

```
	shader "csgo_glass.vfx"
	F_DO_NOT_CAST_SHADOWS 1
	F_VERTEX_BREAK_ANIMATION 1
	…
	DynamicParams
	{
		g_flBreakBounceFloor "($ent_renderbounds.z / 2.0) + 0.25"
		g_flBreakShotSize    "DamageSize"
		g_flBreakTime        "$ent_age"
		g_vBreakShotPosition "DamagePositionObjectSpace"
	}
```

**`F_VERTEX_BREAK_ANIMATION` is a shader feature, and the whole break animation
is four uniforms.** Read them:

- **`g_flBreakTime = $ent_age`** — time since the broken entity spawned. The
  animation is a pure function of elapsed time. No integration, no state.
- **`g_vBreakShotPosition = DamagePositionObjectSpace`** and
  **`g_flBreakShotSize = DamageSize`** — where the bullet hit and how big the
  hit was, in the model's own space. Combined with each shard's pivot from
  `texcoord$4`, the vertex shader has everything it needs to throw shards
  outward from the impact, nearer ones faster.
- **`g_flBreakBounceFloor = ($ent_renderbounds.z / 2.0) + 0.25`** — a **virtual
  floor plane** at the bottom edge of the model's own bounds. The shards fall
  and bounce off the bottom of their own window frame. That is a sill, computed
  from the render bounds, in a material expression.

So the earlier inference (§7.3 conclusion 2, as first written) was right in
architecture and understated in degree. It is not that a material decides which
shards are present. **The entire post-break behaviour — throw, tumble, fall,
bounce, settle — is a closed-form vertex-shader animation over one static
pre-fractured mesh, parameterised by time, impact point, impact size and a
floor height.** There is no per-shard simulation and no per-shard entity. The
`prop_physics` spawn and the one-tetra hull exist so the pane has *some*
physical presence and a place to hang the surface properties; the motion you
watch is not coming from them, which is exactly what `piece_motion_disabled =
true` is saying.

Four more conclusions:

**1. The fracture is baked, not computed.** Voronoi generation runs in the asset
compiler with `rnd_seed = 2`. Every copy of that window in every match shatters
into the same 64 shards. The runtime cost of "procedurally-shattering" is zero,
because the procedure ran at build time. The word "procedural" in the FGD
description refers to the *authoring*, not the frame.

**2. The per-shard channel is `texcoord$4`.** A spare UV set carries the pivot.
Nothing exotic — no structured buffer, no instance stream, no bone per shard.
That is why this survives being one draw call, and why it costs a vertex format
change and nothing else.

**3. Debris collision group + 3-second fade + one tetra + player-movement
exclusion** is a stack of four independent economies on a system that could
easily have been 64 rigid bodies. Each one alone would be sensible; together
they are the difference between a feature that ships in a competitive shooter
and one that does not.

**4. `Inflate` with *Build Backfaces* is doing double duty.** Shards need
thickness so their edges catch a highlight — a zero-thickness shard is invisible
edge-on and reads as a hole. But it also means every shard is a closed volume,
which is what §6's layered glass wants.

**5. Source 1 did the opposite thing.** For contrast **[VDC]**: `func_breakable_surf`
+ `$crackmaterial` + a dedicated `ShatteredGlass` shader (used, per VDC's audit
of `hl2_misc_dir.vpk`, in **exactly one material**: `glasswindowbreak070b.vmt`)
— i.e. a *crack texture* swapped onto a flat brush face, plus `material` =
"Glass" selecting a gib list. The pane never changed shape. Source 2 replaced a
texture swap with real geometry, and paid for it entirely at build time. The
2017 Source 2 shader inventory still lists `vr_shatterglass` as a shader
**[COMMUNITY]**, so an early Source 2 kept a dedicated one; the CS2 pipeline
documented above uses an ordinary glass material with dynamic expressions
instead.

One live bug worth recording, from the same page: *"func_breakable does not
spawn gibs on break in CS2. It's recommended to use prop_dynamic instead"*
**[VDC]**. The legacy path is carried but not maintained.

### 7.4 Why this is the most copyable section of the note

A destructible-cover tactics game wants exactly this and wants nothing else in
this document. **[inferred]** The pattern generalises past glass:

- fracture at **asset build time** into a fixed shard set, with a seed;
- **buckle the interior pieces and leave the border ones flat** — the inset
  selection box plus a sine wave (§7.2). This is the cheapest single thing that
  stops fractured geometry looking like tiling;
- ship intact and broken as **two meshes in one model**;
- put the per-piece pivot in a **spare UV channel** so the broken state stays
  one draw call;
- drive the whole animation from **time, impact position, impact size and a
  floor height**, evaluated in the vertex shader as a closed form — not from
  physics bodies;
- give the pieces a **debris group, a one-tetra hull, player-movement exclusion
  and a fade timer**, so the physics cost is bounded by construction rather than
  by tuning.

None of that needs a new pixel shader, a compute pass, or runtime geometry
manipulation. It needs a mesh operator in whatever bakes assets, one vertex
stream, and about forty lines of vertex shader. That is a much better place for
this project to spend effort than any of §§4–6.

**The transferable idea, stated once:** *if the destruction is deterministic
from the moment of impact, it does not need to be simulated — it needs to be
evaluated.* Time-since-break is a uniform; everything else is a function of it.
That holds for glass, and it holds for any debris that nobody is going to
interact with after it lands.

---

## 8. Liquid in a vessel — Alyx's bottles, and what is actually known

The user's fifth case, and the one with the widest gap between how it looks and
what it is.

### 8.1 The facts, such as they are

Half-Life: Alyx shipped in March 2020 with **opaque** bottles. The liquid
arrived in an update in **late May 2020**, built by **Matt Wilde**, a VFX
developer at Valve, during Washington State's lockdown — he had done preliminary
work before ship and could not finish it in time **[press]**. The load-bearing
sentence, from UploadVR's write-up of Polygon's video interview with Wilde
**[press]**:

> The liquid within the bottles looked and acted so realistic that you would be
> forgiven for thinking Valve had added some form of liquid simulation. However,
> it's all smoke and mirrors — **the entire effect is a visual change to the
> surface of the bottle and there's nothing actually inside them at all.**

Wilde has never published the shader. What is on the record beyond the above:
the effect leans on **cubemaps to fake both the reflection and the interior**,
projected onto the bottle's own surface **[press]**; and Wilde publicly cited
**MinionsArt's Unity liquid shader** as key research while commenting on the
community's reimplementation attempts **[COMMUNITY]**.

That named lineage is the most useful thing here, because MinionsArt's technique
*is* documented, and it explains the parts the interview does not.

### 8.2 The technique, reconstructed

From the MinionsArt lineage and what the interview constrains **[COMMUNITY]**,
with the physics reasoning ours **[inferred]**:

**The fill line is a plane test in object space.** A script feeds the shader a
fill height and two wobble angles. The shader takes the fragment's position,
brings it into the object's local frame (relative to the centre of the mesh
bounds, so the artist's pivot placement does not matter), rotates it about X and
Z by the wobble angles, and compares against the fill height. Above the line:
empty glass. Below: liquid. One `step`.

**The wobble is a spring, not a simulation.** The script computes the object's
linear and angular velocity, accumulates them into a wobble value, and lerps it
back toward zero — a damped oscillator, two floats, evaluated on the CPU. That
is the whole "physics". It is why the liquid settles when the bottle is still
and sloshes when it is not, and why it costs nothing: **the fluid state is two
angles.**

**The liquid's top surface is the back faces.** With culling off, the polygons
you see through the front of the glass, below the fill line, are the *inside of
the far wall*. Shade those as the liquid's surface — flat, with the fill plane's
normal rather than the mesh's — and the elliptical disc of the meniscus appears
for free, correctly foreshortened, at any camera angle, with no extra geometry.
This is the trick that makes the whole thing work and it is the one that is hard
to guess.

**Depth is Beer–Lambert.** The distance from the entry point to the fill plane
along the view ray gives an optical path length; `exp(−σ·d)` on the tint gives
the darkening at the bottom of the bottle and the glow at the shoulder. This is
also the *only* part of the effect that needs the vessel to be a volume, and it
needs it only as a distance, not as geometry.

**Everything else is §§2–6.** The bottle's outer surface is layered glass (§6);
its reflection is a cubemap (§3.1); its distortion is refraction (§4). The
liquid adds a plane, an absorption term and a spring. Stated that way the effect
is much less mysterious than its reputation — which is exactly what "there's
nothing actually inside them at all" was telling us.

### 8.3 Where this belongs

**[inferred]**, and stated bluntly so it does not get built: this is a *hand-held
object in VR* effect. It exists because in Alyx the player picks a bottle up,
holds it near their face, and rotates it. Every term above is a payoff for
close inspection under player-controlled motion.

On a tactical camera looking down at a board, none of it is visible. If liquid
in glass is ever wanted here it will be for a cinematic or an inspect view, and
at that point the reconstruction above is a day's work — but it should be built
*then*, for *that shot*, and not speculatively.

---

## 9. Cost and coverage, all five cases

| Case | Renderer work | Content work | Prerequisite | Verdict for this project |
|---|---|---|---|---|
| **Architectural pane** | ~10 lines in the translucent shader | a `.mat` | reflection probes | **already have it** — see §10 |
| **Glass shadow** | ~8 lines in the depth pass + a blue-noise texture | none | a shadow map | **cheapest real win in this note** — but ship the per-material off switch CS2 uses (§6.5) |
| **Breakable** | ~40 lines of vertex shader | a mesh operator in the asset bake | a spare UV channel for pivots | **the one worth building** |
| **Frosted** | ~6 lines | a roughness map | **scene-colour copy + mips** | only with the copy |
| **Vessel (layered)** | blend state + backface flip + ~10 lines | double-walled models | scene-colour copy | only if hero glass exists |
| **Liquid** | ~40 lines + a CPU spring | a fill-height parameter per model | vessel glass | no |

The dependency graph looks like it has one bottleneck — the **scene-colour copy
with a mip chain**, which unlocks frosted glass, screen-space refraction, layered
vessels and a UI backdrop blur. Source 2 treats all four as the same customer,
which is why `bWantsFBCopyTexture` is a shader attribute rather than four
separate mechanisms.

**But CS2 routes around it entirely, and that is the cheaper plan.** Its
frosted glass is an authored texture (§5.0) and its bottle refraction is a second
cubemap sample (§4). Neither needs the copy. Since this renderer already has
reflection probes, **vessel refraction is available today at the cost of one
extra cubemap fetch along a refracted ray** — no new render target, no pass
reordering, no sorting. That is a much smaller commitment than the table's
"needs the copy" row implies, and it is the route to take first if hero glass
ever turns up.

---

## 10. What this means here

**[inferred]** throughout.

### 10.1 What already exists

`src/cromwell/assets/shaders/rhi/scene/transparent.fs.glsl` is already the CS2
model, and its header already contains the flat-pane argument from
[`source2_rendering.md`](source2_rendering.md) §12.1. `assets/materials/window.mat`
and `overlay.mat` carry `baseOpacity` / `edgeFalloff` / `edgeMaxOpacity` /
`opacityScale` / `edgeThickness` / `edgeColour` — parameter-for-parameter
`GetGlassMaterial`. Case 1 is done, and it is done from the right source.

### 10.2 Three changes, in order of value

**1. Replace the Fresnel ramp with the DFG lookup (§2.2).** The renderer already
has a split-sum BRDF LUT for image-based lighting. `opacity = 1 − DFG(N·V,
roughness).x` is more correct at every roughness, makes rough glass transmit
less without an artist noticing they need to, and deletes `edgeFalloff`,
`edgeMaxOpacity` and `edgeThickness` from the material format. Keep `edgeColour`
if the stylisation is wanted; it is the one part of the CS2 model that is not
reproducible from physics.

*Caveat worth stating:* this makes glass opacity depend on the roughness map,
which on this board is mostly 0.8. Check `window.mat` and `ladder.mat` after the
change — they are the two surfaces smooth enough for the difference to show, and
the ladder is not glass.

**1b. Add the dust layer.** 53 of 53 shipped CS2 glass materials bind
`g_tGlassDust` (§2.1b), and the same conclusion arrives independently from
Alyx. A second texture modulating opacity, albedo and — critically —
**roughness**, so dirt breaks the mirror reflection into a haze. It is the
cheapest thing in this document that changes how *real* a pane looks, it costs
one texture fetch, and it doubles as frosted glass (§5.0) with no shader
changes at all. Arguably this should be item 1.

**2. Add the glass depth pass (§6.5).** A blue-noise texture, a coverage
computed from the same DFG term, and `clip()`. Windows start casting a faint
shadow that darkens at grazing angles and with tint, and the shadow map's
existing filtering does the rest. No new pass, no new target, no sorting. This
is the highest ratio of visible change to code in the note, and it composes with
change 1 because both read the same DFG value.

**3. Design the breakable-glass path around §7.3's five rules, before any
renderer work.** Bake the fracture, ship intact and broken in one model, put
per-shard pivots in a vertex attribute, drive presence from material state, cap
the physics with a debris group and a fade. This project's asset tooling
(`tools/ba/`) is where the Voronoi operator belongs — it is a bake-time
transform on a mesh, and it never needs to run at 60 Hz.

The one adaptation: a lattice of destructible cells is not a pane in a frame, so
the *entity* model (`func_shatterglass`'s state machine, `Restore`,
`SpawnInvulnerability`) does not transfer. The *asset* model transfers exactly.

### 10.3 What not to build

- **Screen-space refraction, under this projection.** §4's own implementation
  gives up under orthographic and samples straight through. Believe it.
- **SSR on glass.** §3.2 — it samples a reflection computed for the surface
  behind the glass. At glass roughness that is wrong in the most visible way.
- **Anything from §8.** Not until there is a shot that holds a glass object
  close enough to look at.

### 10.4 The one idea worth remembering past this document

§6.3. **Putting a translucent surface's transmittance in the destination blend
weight instead of the source colour buys order-independent compositing for the
multiplicative part, for free.** No sorting, no depth peeling, no linked lists.
It is exact for neutral tints and degrades gracefully for saturated ones. That
applies to smoke, to layered decals, to the visibility overlay plates, and to
anything else this renderer stacks in the translucent pass — not just glass.

---

## 11. Sources

**s&box install** **[SBOX]** — `E:\SteamLibrary\steamapps\common\sbox`:

- `addons/base/Assets/shaders/glass.shader` (2026-07-29) — §§2.2, 4, 5, 6
- `download/assets/materials/shaders/glass.fd89ab5e85a158aa.shader` (2024-08-31) — the earlier revision diffed in §6.2
- `core/shaders/vr_lighting.fxc` — `OpaqueFade`/`OpaqueFadeDepth`, cubemap iteration, SSR composite, transmissive terms
- `core/shaders/common.fxc` — `g_vFrameBufferCopyInvSizeAndUvScale`
- `core/shaders/vr_shared_standard_ps_code.fxc` — `D_OPAQUE_FADE` in the standard shader
- `addons/base/Assets/shaders/common/BRDF.hlsl`, `shadingmodel.hlsl`, `blendmode.hlsl`, `material.hlsl`, `classes/DynamicReflections.hlsl`
- `addons/base/Assets/shaders/ui_backdropfilter.shader` — the other consumer of the frame-buffer copy

**CS2 install** **[CS2]** —
`E:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive`, with the
authoring content installed. These three are **uncompiled KV3 text**, readable
with `cat`, and are the strongest evidence in the note:

- `content/csgo/workshop/content_examples/breakable_glass/breakable_glass_01.vmdl` — the ModelDoc tree quoted in §7.2
- `…/breakable_glass_01.vmat` — the shipped `csgo_glass.vfx` material in §2.1a
- `…/breakable_glass_01_broken.vmat` — `F_VERTEX_BREAK_ANIMATION` and the `DynamicParams` block in §7.3

And from `game/csgo/pak01_dir.vpk`, via **ValveResourceFormat 19.2's
`Source2Viewer-CLI`** (`cli-windows-x64.zip`, released 2026-05-29):

- all **53** `csgo_glass.vfx` materials, plus 123 other glass-named materials — the counts in §2.1b, the three refracting vessels in §4, the frosted pane in §5.0
- invocation: `Source2Viewer-CLI -i <pak01_dir.vpk> -f <path> -b DATA`. **Not** `-d`/`--decompile`, which fails on CS2's VCS-71 shaders (§12)

Still unread: `game/core/shaders_pc_dir.vpk` and `shaders_vulkan_dir.vpk`
(compiled shaders — blocked on VCS 71), and `game/core/models_base_breakables.fgd`
(the `@ModelBreakCommand` set — `break_uniform_burst`, `break_flip_pieces`,
`break_twist_pieces`, `break_create_particle`, `break_change_material_group`,
each with force scale and randomisation, which is the authored-force layer on
top of §7's vertex animation and deserves its own pass).

**Valve Developer Community** **[VDC]** — behind an Anubis proof-of-work;
`WebFetch` 403s. Solve it in Python and fetch `?action=raw` (see the
`vdc-wiki-anubis-proof-of-work` note):

- [Counter-Strike 2 Workshop Tools/BreakableWindows](https://developer.valvesoftware.com/wiki/Counter-Strike_2_Workshop_Tools/BreakableWindows) — the ModelDoc fracture pipeline in §7.2
- [Base.fgd/Counter-Strike 2](https://developer.valvesoftware.com/wiki/Base.fgd/Counter-Strike_2) — `func_shatterglass` and `func_reflective_glass` verbatim
- [Func shatterglass (Half-Life: Alyx)](https://developer.valvesoftware.com/wiki/Func_shatterglass_(Half-Life:_Alyx)) — the same entity in the Alyx tools
- [VR Standard (Source 2 Shader)](https://developer.valvesoftware.com/wiki/VR_Standard_(Source_2_Shader)) — the Glass parameter group, Cube map blur
- [Breakable Glass](https://developer.valvesoftware.com/wiki/Breakable_Glass) and [Shaders in Glass folder](https://developer.valvesoftware.com/wiki/Shaders_in_Glass_folder) — the Source 1 contrast in §7.3

**ValveResourceFormat** **[VRF]**:

- [`Renderer/Shaders/common/texturing.slang`](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/master/Renderer/Shaders/common/texturing.slang) — `GetGlassMaterial`, and the `GLASS_IS_TRANSMISSIVE` opt-out comment naming Deadlock
- [`Renderer/Renderer/Shaders/ShaderLoader.cs`](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/master/Renderer/Renderer/Shaders/ShaderLoader.cs) — the Valve-shader-name → renderer-shader mapping; `csgo_glass.vfx` and `vr_glass.vfx` are not special-cased and fall through to `complex`
- [`Renderer/Shaders/complex_features.slang`](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/master/Renderer/Shaders/complex_features.slang) — the per-feature shader lists, which is where the shader name `liquid_fx` appears

**Press and community**:

- [UploadVR — *Valve Developer Breaks Down Half-Life: Alyx's Incredible Liquid Shaders*](https://www.uploadvr.com/alyx-liquid-shaders/) **[press]** — the Wilde quote in §8.1, reporting Polygon's video interview
- [Polygon — *Why the liquids in Half-Life: Alyx look so dang good*](https://www.youtube.com/watch?v=9XWxsJKpYYI) **[press]** — the primary interview; not transcribed here
- [Geimund, *Half Life: Alyx Liquid Shader*](https://geimund.wordpress.com/2020/07/06/major-project-h-m-s-reprise-week-seven-forefront-half-life-alyx-liquid-shader/) **[COMMUNITY]** — records that Wilde cited MinionsArt's shader as key research
- [MinionsArt, *Unity Liquid Shader*](https://www.patreon.com/minionsart/posts/unity-liquid-18245226) **[COMMUNITY]** — the fill-amount / wobble technique reconstructed in §8.2
- [Source 2 Shaders List, 2017-04-11](https://gist.github.com/AlpyneDreams/efada34762e5f4f337c11c6a6cbd99b5) **[COMMUNITY]** — `glass`, `refract`, `vr_shatterglass`, `water`, `simple_water` as named shaders in an early Source 2

---

## 12. What would settle the open questions

Question 4 below is **closed** — see §7.3, answered from Valve's shipped
material. What remains:

1. ~~**Does any shipping CS2 map material turn refraction on?**~~ **Closed,
   2026-08-16.** Three of 53, all vessels, and via
   `F_OPAQUE_CUBEMAP_REFRACTION` rather than a scene grab — §4. The flat-pane
   claim is confirmed and the mechanism was a surprise.
2. **What is Deadlock's `GLASS_IS_TRANSMISSIVE` model?** VRF gates the entire
   edge-Fresnel block on `!defined(GLASS_IS_TRANSMISSIVE)` and says Deadlock
   reuses `F_GLASS` for *"a transmission model that has none of these
   parameters"* **[VRF]**. The symbol's definition was not found in the files
   read. Deadlock is a **desktop** Source 2 title, so if Valve have a
   physically-grounded transmission glass anywhere, that is where it is.
3. **What is `liquid_fx.vfx`?** The name appears in VRF's per-feature shader
   lists for `F_PAINT_VERTEX_COLORS` **[VRF]** — so a Source 2 title ships a
   shader called `liquid_fx` that takes vertex colours. Which title, and whether
   it is Alyx's bottle shader, is not established. Worth chasing before
   reconstructing §8 from community sources.
4. ~~**Does the shard material really address pieces through `Assign Vertex
   Pivot Data`?**~~ **Closed, 2026-08-16.** Yes — `output_stream_name =
   "texcoord$4"`, and the material drives `F_VERTEX_BREAK_ANIMATION` from four
   dynamic parameters. §7.3. The architecture was inferred correctly and the
   *degree* was understated: the entire break animation is closed-form in the
   vertex shader, not just per-shard visibility.

Two new questions the install opened:

5. **What does `F_VERTEX_BREAK_ANIMATION` actually compute?** The four inputs
   are known; the vertex shader is inside `shaders_pc_dir.vpk` as compiled
   bytecode. VRF can dump a `.vcs`'s features, parameters and combos, which
   would at least confirm the input set and may recover the DXBC.
6. ~~**Is `csgo_glass.vfx`'s mask layer the grime layer?**~~ **Closed,
   2026-08-16.** Yes. `GlassMaskColor` compiles to **`g_tGlassDust`**, and it is
   bound on 53 of 53 shipped glass materials — §2.1b.

7. **What does `F_OPAQUE_CUBEMAP_REFRACTION` compute?** New, and the most
   interesting one left. The three vessel materials (§4) prove it exists and that
   `g_flRefractMinRoughness` gates it, but not whether it refracts through one
   interface or two, nor whether it accounts for thickness. Needs the shader
   itself — see 5.

**Tooling note for all of the above.** ValveResourceFormat 19.2's CLI reads
CS2's VPKs and `.vmat_c` files, but **cannot parse CS2's current compiled
shaders** — `Only VCS file versions 59 through 70 are supported … 71`. That
aborts a full `--decompile` of a material, because the extractor tries to
reflect texture inputs from the shader. The workaround used throughout this note
is `-b DATA`, which prints the material's own KV3 block and never touches the
shader. Every parameter table in §§2.1a–2.1b, §4 and §5.0 came out that way.
Questions 5 and 7 stay open until VRF supports VCS 71.
