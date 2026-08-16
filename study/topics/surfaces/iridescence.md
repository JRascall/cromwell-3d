# Iridescence: thin film and diffraction grating

Whether the Cycles node graph in
[the BlenderArtists thread](https://blenderartists.org/t/a-complex-approach-iridescence-in-cycles/613211)
can be made real time, and what it would cost as a `.mat` in this engine.

**Short answer: yes, and it was real time first.** The thread's author names his
source in post #32 — chapter 8 of *GPU Gems* — and that chapter is a **Cg vertex
program from 2004 that ran on GeForce FX hardware**. What the thread did was port
a real-time technique *into* an offline path tracer, where it got slower by two
or three orders of magnitude for reasons that are entirely about Cycles' shading
architecture and not at all about the physics. Bringing it back out is not
research. §4 is the ladder of three implementations, cheapest first, and the
cheapest one is about fifty ALU instructions with no texture at all.

The parts that are genuinely hard are in §7, and none of them is the shader.

---

## 0 Sources, and how to read them

| Tag | Meaning |
|---|---|
| `[GEMS]` | *GPU Gems* ch. 8, Jos Stam, "Simulating Diffraction" (2004) — full Cg listing recovered, [NVIDIA's online edition](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-8-simulating-diffraction) |
| `[PAPER]` | Peer-reviewed. Toisoul & Ghosh, TOG 2017; Belcour & Barla, TOG 2017; Stam, SIGGRAPH 1999; Kandel & Dhillon, arXiv 2025 |
| `[COMMUNITY]` | The BlenderArtists thread itself, read from Discourse's JSON export — all 109 posts, 2014-06 to 2026-06. Also Alan Zucconi's ten-part Unity series (2017) |
| `[UE-SRC]` | **Unreal Engine 5.7, read from this machine** — `C:/Program Files/Epic Games/UE_5.7/Engine/Shaders/Private/ThinFilmBSDF.ush`, `.../Substrate/Substrate.ush`, and the C++ in `Engine/Source/Runtime/Engine/`. Code, names and comments are Epic's. §6 |
| `[ENGINE]` | Shipped engine features claimed from documentation rather than source — Unity HDRP, Blender's Principled BSDF |
| `[inferred]` | My reasoning, flagged as such |

Where a number is quoted it is from the source, not remembered. Where I have
reconstructed something, it says so.

---

## 1 Two different effects wear the same word, and they need different shaders

This matters before anything else, because the thread's title says *iridescence*
and its most-linked result is a **diffraction grating**, and they are separate
phenomena with separate maths and separate costs.

| | **Thin-film interference** | **Diffraction grating** |
|---|---|---|
| Cause | Light reflects off the **top and bottom of a thin layer**; the two reflections are out of step by twice the film thickness, so some wavelengths cancel | Light reflects off **many parallel ridges**; reflections from adjacent ridges are out of step by the ridge spacing, so each wavelength is reinforced in its own direction |
| Governing length | Film **thickness**, 100–1000 nm | Ridge **spacing** `d`, 400–2000 nm |
| Looks like | Soap bubble, oil on a puddle, heat-tinted steel, beetle shell | CD, DVD, holographic foil, security hologram, sequins, butterfly wing |
| Depends on | View angle only (one lobe, tinted) | View angle **and** light angle **and** surface tangent direction (several rainbow lobes, in a fan) |
| Real-time status | **Shipped, in three engines.** Do not write your own — §6 | **Not shipped as an engine feature anywhere I found.** But solved, three ways — §4 |

The thread does both. Post #31 (2014-06-29) is a soap bubble driven by a
thickness map varying 30 nm at the top to 700 nm at the bottom — that is thin
film. Post #32 onward is the CD/holographic-tape work — that is grating, and it
is what "diffraction grating" means.

`[inferred]` **If the target is holographic foil, sequins, security markings or a
CD, you want the grating.** If it is oiled metal, a soap bubble, a fuel slick, or
tempered steel, you want thin film and you want somebody else's implementation.
They do not substitute for each other: a thin-film shader on a CD gives a colour
that slides with view angle but does not fan out into orders, and it reads as
"tinted metal", not as a CD.

---

## 2 What the thread built, and why it is the shape it is

### 2.1 The grating equation is one line

Secrop states it directly in post #38, and it is the standard result:

```
d · sin(θ_light) − d · sin(θ_camera) = m · λ
```

`d` is the ridge spacing, `m` an integer (the **order**), `λ` the wavelength. For
a given camera angle, each wavelength is bright when the light sits at one of a
small set of angles — one per order. Order 0 (`m = 0`) is the ordinary specular
reflection, where every wavelength coincides and the highlight is white. Orders
±1, ±2 … fan out on either side as rainbows, each one wider and dimmer than the
last.

His diagram caption gives the one fact that governs authoring: *"the grating
distance is 2000nm. Smaller values make the order separation wider, and bigger
values make the orders closer to each other (producing more visible orders)."*

His authored values, from post #35 — these are the useful table:

| Material | `d` | Orders needed |
|---|---|---|
| CD, metallic part | ~1600 nm | 3–4, mixed with an anisotropic lobe |
| Holographic film | ~700 nm | 1 is enough, 2 max; **tangent varies per patch** |
| Feathers, butterfly wings | 400–900 nm | 1. Low `d` reads as *"a blueish velvet specularity"* |

`[PAPER]` Toisoul & Ghosh independently model a CD as a 1D multislit grating with
**spacing 2.0 µm, slit width 0.5 µm, and 21 slits illuminated** — the last
inferred from a photographic measurement. Their spacing disagrees with Secrop's
1600 nm, and with the CD Red Book track pitch of 1.6 µm; 2.0 µm is what they
found fit their measured pattern. Take that as the honest range: `d` is a knob
you tune to a reference photograph, not a value you look up.

### 2.2 The expensive part is a Cycles limitation, not physics

Secrop's own pseudocode in post #43 is eight lines:

```
for (wl = 400; wl < 720; wl += wlstep)
  for (order = −OrderMax; order <= OrderMax; order++) {
      N' = NormalSolver(d, wl, order, N, T);
      Cl += reflect(N') * wavelength(wl);
  }
```

That is the whole algorithm, and it is cheap. What makes the Blender version
cost 12,000 samples and still show noise (post #24) is that **Cycles has no way
to say "add a coloured reflection lobe in direction X."** So he inverts the
problem: solve the grating equation for the *incoming* direction, build a fake
normal exactly halfway between the incoming and outgoing directions (post #39 —
`Φ = (β + θ)/2`, rotated about the tangent), hand that normal to a glossy BSDF,
and let Monte Carlo find the light. Every wavelength and every order needs its
own BSDF instance. He hits Cycles' ~32-closure ceiling (post #68), and he has to
scavenge uniform random numbers out of a **brick texture** because no noise node
in Blender gives a flat distribution (post #75).

`[inferred]` Every one of those problems is an artefact of expressing a BRDF as a
node graph in a path tracer. **A rasteriser has the light direction in hand.**
There is no sampling, no closure limit, and no need for random numbers at all —
the double loop above is just a loop, and you can collapse the inner one
analytically. This is the single most important observation in this note: the
Blender graph is not evidence that the effect is expensive, it is evidence that
Cycles is the wrong tool for it.

The thread reaches the same conclusion repeatedly and never acts on it — posts
#25, #27, #41, #43, #45, #59, #89 all say some version of *"this should be
hardcoded as a new BRDF"*, and twelve years later it never was.

---

## 3 The punchline: the source was a real-time shader

`[GEMS]` Post #32: *"based on chapter 8 from GPU Gems, i've build a shader that
can replicate this effect."*

Chapter 8 is Jos Stam's simplification of his own SIGGRAPH 1999 *Diffraction
Shaders* paper, written as a **Cg vertex program**, shipped with a real-time
compact-disc demo. The whole listing, recovered verbatim:

```cg
float3 blend3(float3 x) {
    float3 y = 1 - x * x;
    y = max(y, float3(0, 0, 0));
    return y;
}

void vp_Diffraction(...)
{
    float3 L = normalize(lightPosition - P);
    float3 V = normalize(eyePosition - P);
    float3 H = L + V;                    /* NOT normalised — see below */
    float u = dot(T, H) * d;             /* T is the tangent, d the spacing */
    float w = dot(N, H);

    /* order 0: Ward's anisotropic highlight, r is its spread */
    float e = r * u / w;
    float c = exp(-e * e);
    float4 anis = hiliteColor * float4(c.x, c.y, c.z, 1);

    if (u < 0) u = -u;
    float4 cdiff = float4(0, 0, 0, 1);
    for (int n = 1; n < 8; n++) {
        float y = 2 * u / n - 1;
        cdiff.xyz += blend3(float3(4*(y-0.75), 4*(y-0.5), 4*(y-0.25)));
    }
    colorO = cdiff + anis;
}
```

Read against §2.1: `u = dot(T, H) · d` **is** `d·(sin θ_L − sin θ_V)`, because
the unnormalised half-vector projected on the tangent gives exactly that
difference. `u/n` is the wavelength satisfying the grating equation at order `n`.
`blend3` is a rainbow map — three parabolic bumps peaking in blue, green and red
— so the wavelength never becomes a number, it becomes a colour directly.

**The whole wavelength integral is replaced by three parabolas, and the order sum
by a fixed loop of 8.** Stam is explicit that the fixed 8 is a compiler
limitation of the day, not a modelling choice, and that a texture lookup would be
better. Both of those are now free.

Two details from the chapter worth keeping:

- **Order 0 is handled separately**, as a Ward anisotropic highlight
  (`exp(−(r·u/w)²)`), not as part of the sum — because at `u = 0` every
  wavelength coincides and the sum degenerates. Secrop reaches the same
  conclusion in post #106, ten years later: *"The 0th order is a typical glossy
  reflection… for simplicity and speed, a simple glossy can also be used."*
- **The tangent can come from a texture.** Figure 8-8 in the chapter is a surface
  with the anisotropy direction texture-mapped, which is exactly how you get
  holographic foil rather than a CD — Secrop's post #35 says the same thing
  (*"Tangent is variable. One can use a voronoi cell for the rotation"*).

`[COMMUNITY]` Alan Zucconi's 2017 Unity series ports precisely this and improves
the weakest part. `blend3`'s constants were, in Stam's words, chosen
experimentally, and they map poorly to the real spectrum. Zucconi refit them by
least-squares against a linear visible spectrum image and published both
versions, branchless:

```hlsl
inline fixed3 bump3y(fixed3 x, fixed3 yoffset) {
    float3 y = 1 - x * x;
    y = saturate(y - yoffset);
    return y;
}

fixed3 spectral_zucconi6(float w) {   /* w in [400, 700] nm */
    fixed x = saturate((w - 400.0) / 300.0);
    const float3 c1 = float3(3.54585104, 2.93225262, 2.41593945);
    const float3 x1 = float3(0.69549072, 0.49228336, 0.27699880);
    const float3 y1 = float3(0.02312639, 0.15225084, 0.52607955);
    const float3 c2 = float3(3.90307140, 3.21182957, 3.96587128);
    const float3 x2 = float3(0.11748627, 0.86755042, 0.66077860);
    const float3 y2 = float3(0.84897130, 0.88445281, 0.73949448);
    return bump3y(c1 * (x - x1), y1) + bump3y(c2 * (x - x2), y2);
}
```

Six bumps instead of three; the difference shows in violet and orange, which is
where three parabolas visibly fail. **It is authored in sRGB** — Zucconi confirms
this in a 2018 comment on his own post and says to raise the result to the power
2.2 for a linear pipeline. Ours is linear, so that correction is mandatory and
skipping it will read as "the rainbow is washed out" rather than as a colour-space
bug.

His CD shader is the whole thing in eighteen lines, and it computes the tangent
from UVs rather than from the mesh — for a disc, `T = (−v.y, 0, v.x)` after
remapping UV to `[−1, 1]`.

---

## 4 Three implementations, cheapest first

### Tier 0 — analytic, no data at all

`[GEMS]` + `[COMMUNITY]`. The listing above. Per pixel: one unnormalised
half-vector, two dot products, a loop of 8 with two multiply-adds and six
parabola evaluations each, plus one `exp` for order 0.

Call it **50–80 ALU, zero texture fetches, zero bandwidth, zero authoring.** On
any GPU this project targets it is free — it is comparable to a single extra
GGX lobe and cheaper than one shadow tap. The loop trip count can be derived from
`d` and clamped, which cuts it further for small `d` (Secrop's post #33 makes
exactly this point about not paying for orders that cannot exist).

**This is what to build first, and it may be all that is ever needed.**

### Tier 1 — one-dimensional lookup table

`[inferred]` from `[GEMS]`'s own suggestion (*"a better solution might be to use
a one-dimensional texture map"*) and Zucconi's paywalled part 8, whose public
comment thread describes it as a fake BRDF that *"bakes the reflection into an
easily editable [texture]"*.

Bake the whole order sum — everything downstream of `u` — into a 1D RGB texture
indexed by `u`. Per pixel: two dot products and **one texture fetch.** The table
is authorable, which is the real win: an artist can paint the fan of orders
directly, warm it, desaturate the high orders, or clip them to a plausible range,
without touching a shader. It also removes the loop's dependence on `d`, so
one shader serves every grating.

### Tier 2 — two-dimensional measured lookup table

`[PAPER]` Toisoul & Ghosh, *Practical Acquisition and Rendering of Diffraction
Effects in Surface Reflectance*, TOG 36(5), 2017. This is the current practical
state of the art and the paper to read if tier 0 is not enough.

Their derivation collapses Stam's model, under a first-order (Born)
approximation, to:

```
f_diffraction(ωi, ωo) = 4π² · F²(ωi,ωo) · G(ωi,ωo) · Sd(h)
Sd(h) = ∫ (1/λ²) · I( (2π/λ)(h·t), (2π/λ)(h·b) ) dλ
```

`h = ωo − ωi` is the unnormalised half-vector; `t` and `b` are tangent and
bitangent. **The entire spectral integral collapses into `Sd`, a 2D table indexed
by `(h·t, h·b)`** — one texture fetch, and the diffraction term is a
multiplicative factor on the standard Fresnel and geometry terms, so it slots
into an existing microfacet BRDF rather than replacing it.

Their numbers, stated in §6 of the paper:

- **65 FPS on an NVIDIA GTX 960M**, entry-level laptop hardware of 2017, in a
  GLSL fragment shader. A 2026 desktop GPU has one to two orders of magnitude
  more to spend.
- The table is 2048², built from a 1000² HDR photograph, and computing it takes
  *"only a few seconds"* on an i7-4720HQ. It is a build step, not a runtime cost.
- The lookup table is **captured, not simulated** — photograph the diffraction
  pattern on the real sample under an LED flash with a narrow-band filter. Which
  means it handles things nobody wants to model: Bragg diffraction from an LCD's
  crystal lattice, blazed (asymmetric) gratings whose orders are brighter on one
  side, LED TV screens, holographic paper.
- Non-linear sampling of the table (`u^n` with `n` typically 5) is needed when
  the pattern hugs the specular direction.

And when you cannot photograph the sample, §6.1 gives the **analytic multislit**
form to bake instead — no capture rig required:

```
I(θ) = sinc²(π·w·sinθ / λ) · [ sin(N·π·Δ·sinθ / λ) / (N·sin(π·Δ·sinθ / λ)) ]²
```

with `w` the slit width, `Δ` the spacing and `N` the number of illuminated slits.
Their CD is `Δ = 2.0 µm`, `w = 0.5 µm`, `N = 21`, modulated by an anisotropic
Gaussian window to soften the pattern edges. This is a strictly better generator
for a baked table than Stam's rainbow-sum, because it gives correct **relative
brightness** between orders, which the parabola sum does not.

### Tier 3 — don't

`[PAPER]` Kandel & Dhillon, *GratNet: A Photorealistic Neural Shader for
Diffractive Surfaces*, arXiv:2506.15815 (June 2025, revised July 2025) — an MLP
fitted to measured diffractive reflectance, framed explicitly as a **data
compression** problem, reducing dataset footprint by two orders of magnitude
against wave-optical forward modelling. Real, current, and the right answer if
you have dense measured data for many materials and cannot afford to ship it.
For one or two authored surfaces in a tile game it is answering a question we do
not have.

---

## 5 Environment lighting is where it gets awkward

`[PAPER]` Toisoul & Ghosh handle it by **pre-convolving each colour channel of
the environment map with the corresponding channel of the diffraction table**,
then reading the result as an ordinary reflection map, indexed by the half-vector
— the same procedure as any pre-filtered specular probe. Measured RMSE against a
full hemispherical integration reference: **7.6%**, which is close enough that
the comparison figure is hard to call.

Two limitations they state outright, and both bite here:

- The pre-filter **assumes axis-aligned rendering and does not model rotation of
  the sample about the view vector.** A diffractive surface spun in place should
  spin its pattern with it, and under a pre-convolved environment it will not.
- The paraxial approximation used to index the table degrades for **near-field
  lights** subtending a large angle.

`[inferred]` For this project that is a smaller problem than it sounds. The
probe system already exists, and the effect's whole appeal is its response to a
*bright* light — the sun, a lamp — not to ambient. Running tier 0 against the sun
only, and letting the probe supply an ordinary untinted specular, is a defensible
place to stop and is what I would ship.

`[PAPER]` For completeness, the equivalent work on the thin-film side is Kneiphof
et al., *Real-time Image-based Lighting of Microfacet BRDFs with Varying
Iridescence*, CGF 2019; and Liu et al., *Real-time polygonal lighting of
iridescence effect using precomputed monomial-Gaussians*, CGF 2023, for area
lights. Both post-date and build on Belcour & Barla.

---

## 6 Thin film is already solved — and how Unreal solved it is the useful part

`[PAPER]` Belcour & Barla, *A Practical Extension to Microfacet Theory for the
Modeling of Varying Iridescence*, TOG 2017. The contribution that made it
shippable is not the Airy summation — that was known — it is **analytic
pre-integration of the spectral response**, which is what makes an RGB renderer
agree with a spectral one instead of producing a different set of colours. Their
own abstract claims it as the first model to do so.

`[ENGINE]` It is in **Unity's HDRP** (the authors say so; Unity blogged it), and
in **UE5's Substrate**. Blender's Principled BSDF gained an iridescence layer
too, twelve years after this thread asked for one.

Unreal's is on this machine, so the rest of this section is read from it rather
than from the docs — `[UE-SRC]`, `C:/Program Files/Epic Games/UE_5.7/`.

### 6.1 It is Belcour & Barla verbatim, truncated at three orders

`Engine/Shaders/Private/ThinFilmBSDF.ush` is the paper, transcribed. The
give-away is `ThinFilmEvalSensitivity` (line 72), which is the paper's Gaussian
fit of the **CIE XYZ colour-matching curves in Fourier space** — the whole trick
that turns a spectral integral into three cosines:

```hlsl
float3 ThinFilmEvalSensitivity(float opd, float shift)
{
    float phase  = 2 * PI * opd * 1.0e-6;
    float3 val   = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos   = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var   = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2*PI*var) * cos(pos*phase + shift) * exp(-var*phase*phase);
    xyz.x += 9.7470e-14 * sqrt(2*PI*4.5282e+09) * cos(2.2399e+06*phase + shift) * exp(-4.5282e+09*phase*phase);
    return xyz / 1.0685e-7;
    // As pointed by the suplemental material the oupout is not in the correct space, we need to tranfser
}
```

Constants unchanged from the paper, including the extra red lobe on `xyz.x`
because the X curve is bimodal. **The Airy summation is truncated at `m <= 3`**
(three orders past the DC term), and the result is converted with a fixed
XYZ→CIE-1931-RGB matrix under a **neutral E illuminant**.

That trailing comment is Epic's, unfinished mid-sentence, and it ships: a known
colour-space discrepancy flagged in the paper's supplemental material, left
uncorrected. Worth knowing before treating the output as ground truth.

### 6.2 Two implementations ship and the good one is dead code

`ThinFilmBSDF.ush` defines **`F_ThinFilmRef`** — the honest version. Gulbrandsen's
*Artist Friendly Metallic Fresnel* to recover a complex IOR `n + ik` from F0 and
edge tint; separate dielectric and conductor Fresnel routines; **s- and
p-polarisation carried separately as `float2` throughout**, with their phase
shifts, and a total-internal-reflection branch. Then it depolarises for natural
light at the end.

A grep of the entire shader tree finds **exactly one occurrence of
`F_ThinFilmRef`: its own definition.** Nothing calls it. It is kept as a
reference to compare against.

What actually runs is `F_ThinFilm`, whose own header comment states the
simplification: *"relies on Schlick's Fresnel and de facto does not take into
account Fresnel phase shift & polarization."* The first interface becomes one
`F_Schlick(DielectricIorToF0(IOR), VoH)`, the second becomes `R23 = F0` directly
— with a comment admitting the shortcut, *"Ideally we should recompute F0 (R23)
by taking into account the thin layer IOR"* — and `phi12 = phi23 = 0`.

`[inferred]` **That is the real lesson about shipping this model.** The
peer-reviewed version was implemented, kept, and then not used. The polarisation
bookkeeping is most of the code and evidently did not pay for itself.

### 6.3 The shipped design: it is not a BSDF, it is an F0/F90 modifier

This is the part worth stealing, and it is invisible from the documentation.

Despite the filename, the material-graph node is
`UMaterialExpressionSubstrateThinFilm : public UMaterialExpressionSubstrate**UtilityBase**`
with **two outputs, `F0` and `F90`** — not a slab, not a BSDF. Epic's own tooltip:
*"Compute the resulting material specular parameter F0 and F90 according to input
surface properties as well as the thin film parameters."*

The translator (`HLSLMaterialTranslator.cpp:15262-15270`) emits:

```cpp
AddCodeChunk(MCT_Float3, TEXT("SubstrateGetThinFilmF0F90(%s, %s, %s, %s, %s)%s"),
    Dot(NormalCodeChunk, CameraVectorCode),   /* <- NoV, hard-wired */
    SpecularColorCodeChunk, EdgeSpecularColorCodeChunk,
    ThicknessCodeChunk, IORCodeChunk,
    OutputIndex == 0 ? ".F0" : ".F90");
```

and `Substrate.ush:3257`:

```hlsl
void SubstrateGetThinFilmF0F90(float NoV, float ThinFilmNormalizedThickness, float ThinFilmIOR,
                               inout float3 OutF0, inout float3 OutF90)
{
    const float NoL = NoV;          // <-- the whole approximation
    const float VoH = NoV;
    float3 RThinFilm = F_ThinFilm(NoV, NoL, VoH, F0, F90, ThinFilmIOR, ThinFilmNormalizedThickness);

    // Compute a F0 which match evaluation Thin-Film reflectance for the current
    // view angle using Schlick's Fresnel
    const float FReference = min(0.99f, F_Schlick(0, 1.0f, NoV).x);
    RThinFilm = max(RThinFilm, FReference);
    float3 F0_Thin = (RThinFilm - F90 * FReference) / (1 - FReference);

    F0_Thin  = max(GetF0MicroOcclusionThreshold(), F0_Thin);   /* no micro-occlusion */
    F0_Thin *= F0RGBToMicroOcclusion(F0);                      /* ...then put it back */

    OutF0  = F0_Thin;
    OutF90 = max(F0_Thin, F90);
}
```

Three things are happening, and each is a decision:

1. **`NoL = NoV; VoH = NoV;`** — the iridescent Fresnel is evaluated *as though
   the light were at the eye*. The colour is a function of view angle alone. It
   never sees a light direction or a half-vector.
2. **Schlick is inverted to re-encode the answer.** `FReference` is Schlick's
   Fresnel with F0=0, F90=1 at this `NoV`; the algebra solves "what F0 would
   reproduce `RThinFilm` at this angle through the ordinary Schlick curve". The
   iridescence is smuggled into the existing specular parameters.
3. **The energy clamps are not incidental.** `max(RThinFilm, FReference)` stops
   the reconstruction from producing a negative F0, and the micro-occlusion pair
   exists because Substrate encodes micro-occlusion *in F0's magnitude* — so the
   thin-film pass has to strip it and re-apply it, or the surface would brighten.

A sweep of the whole of `Engine/Source/Runtime` for `ThinFilm` returns **five
files, and every one of them is the material-graph node or its translator**
(`MaterialExpressionSubstrate.cpp/.h`, `HLSLMaterialTranslator.cpp/.h`,
`MaterialCompiler.h`). **Nothing in the renderer knows thin film exists** — no
lighting path, no G-buffer field, no shading model, no cvar. That is the claim
below, confirmed negatively.

**Why this is the right shape:** F0 and F90 are already fields the slab writes.
So thin film costs **one evaluation per pixel in the base pass and nothing per
light** — and it works, unchanged, through deferred shading, forward, mobile
forward, area lights, reflections and Lumen, none of which needed a line of code.
The alternative (an iridescent Fresnel inside the lighting loop) would have meant
touching every one of those paths and paying per light.

Parameters, from the header tooltips: **`Thickness` 0–1 maps to 0–10 µm**
(`Dinc = Thickness * 10`, and `0 means disabled`), IOR clamped to 1–3, and
`eta2 = lerp(1.0, IOR, smoothstep(0.0, 0.03, Dinc))` forces the film's IOR toward
air as thickness → 0 so the effect fades out cleanly instead of popping.

### 6.4 What that means for the grating

`[inferred]` **The Substrate trick does not transfer to §4, and the reason is the
whole answer to why thin film ships in engines and gratings do not.**

Thin film's colour depends only on the angle between the view and the normal, so
it can be folded into a per-pixel constant — a G-buffer field — before lighting
begins. A diffraction grating's colour depends on `dot(T, L + V)`: **it needs the
light direction, so it cannot be precomputed per pixel.** It has to live inside
the lighting loop, once per light, and it needs a tangent carried alongside the
normal.

That is a structural difference, not a difficulty difference. It is why the
grating is cheap in this engine *today* — one sun, one evaluation (§7.2) — and
why it gets linearly more expensive the moment clustered forward lands, which is
exactly the point at which tier 1's single texture fetch starts to earn its slot.

`[inferred]` Epic's documentation line about thin film producing *"iridescent
effects like those found on CDs"* is marketing. There is no order sum, no
grating spacing and no tangent anywhere in `ThinFilmBSDF.ush`; a CD's rainbow is
not reachable from it.

### 6.5 So: do not write a thin-film shader

`[inferred]` Belcour's GLSL is published, Epic's is readable at the path above,
and both are closed-form functions of thickness, IOR and view angle with no
sampling. The thread's post #12/#23 node graph — described by its own author as
*"the complexity of the node setup. It's ridiculous"* — is a 2014 reconstruction
of a problem properly solved in 2017 and shipped by 2022.

---

## 7 What it would take here, and what would actually go wrong

### 7.1 The engine already has the hard prerequisite

A grating shader needs **a tangent per fragment**, and this is the piece that
usually costs a vertex-format change. It does not here:

- [`MeshVertexBuffer.hpp:39-42`](../../../src/cromwell/geometry/MeshVertexBuffer.hpp#L39-L42)
  already stores a `vec4` tangent with handedness in `w`.
- [`lit.vs.glsl:14`](../../../src/cromwell/assets/shaders/rhi/scene/lit.vs.glsl#L14)
  already declares it — `layout(location = 2) in vec4 inTangent; /* declared for
  stride; unused here yet */`.

So the change is **one interpolator** (a fifth `out` in the vertex stage, after
`vShadowClip` at location 3), not a data change and not a rebake. The world's box
geometry takes its UVs from a world-space planar projection, so its tangents are
whatever the emitter writes — which is fine for a flat foil and wrong for a disc;
a disc wants the tangent derived from UV as Zucconi does, or from a map.

### 7.2 It fits the BRDF where Toisoul says it does

[`common/brdf.glsl`](../../../src/cromwell/assets/shaders/common/brdf.glsl)
already computes `H = normalize(L + V)` and returns a `SurfaceResponse { diffuse;
specular; }`. The diffraction term multiplies the same `F` and `G` the specular
lobe uses (§4 tier 2), so it belongs beside `specular`, computed from the
**unnormalised** `L + V` — normalising it destroys the magnitude the grating
equation depends on, which is a bug that produces a plausible-looking rainbow of
the wrong scale.

[`lit.fs.glsl`](../../../src/cromwell/assets/shaders/rhi/scene/lit.fs.glsl) is
sun-only today, so this is **one evaluation per pixel**, not one per light. When
clustered forward lands it becomes one per light touching the surface, which is
the point at which tier 1's single fetch starts to matter over tier 0's loop.

### 7.3 It is a `.mat`, and the format already fits

Per [`MaterialDefinition.hpp`](../../../src/cromwell/material/MaterialDefinition.hpp),
a material is `key value` lines and the parser warns on unknown keys. The full
parameter set for tier 0 is small:

```
# holographic foil
gratingSpacing      700     # nm between ridges; 1600 for a CD
gratingOrders       2       # how many rainbows either side of the highlight
gratingStrength     0.6     # scales the whole term
gratingTangentMap   ...     # optional; per-texel ridge direction
```

`[inferred]` Four scalars and an optional map. `MaterialBlock` in
[`material_block.glsl`](../../../src/cromwell/assets/shaders/rhi/include/material_block.glsl)
is six `vec4`s and would take one more; `PbrMaterial`'s slot comment says
**material map indices 4, 5 and 6 are free**, which is where a tangent-direction
map or a baked 1D table would go. Nothing about this needs a new shading model,
a new pass, or a branch in C++ — which is the same argument the header already
makes for `blend translucent`.

Per CLAUDE.md, it does **not** earn its own profiler zone: it is arithmetic
inside `lit scene`, not a system that runs per frame on its own.

### 7.4 The four things that will actually make it look wrong

`[inferred]`, and these are the reason to prototype before committing.

**One — with a single directional light and a fixed camera, nothing moves.** The
effect *is* its motion. `u = dot(T, L + V) · d`, so the pattern only changes when
`L`, `V` or `T` changes. This game has one sun and a camera that mostly looks
down at a tilted angle. Secrop's own demonstration is a *video*, and post #32
says so — *"I'll post an animation tomorrow, where the effect is best seen."*
A static rainbow smear on a roof panel is not what anyone is picturing when they
ask for this. **The mitigation is `T`**: vary the ridge direction per texel or per
patch (foil, sequins, a voronoi rotation) so the fan breaks up spatially and the
camera's own motion animates it. That is a content decision, and it is the one
that decides whether this is worth doing at all.

**Two — it aliases, badly.** `u` is high-frequency in the half-vector, so a
distant surface undersamples the order fan and shimmers under any camera motion.
This is exactly the failure
[`material_detail.md`](material_detail.md) §2 is about — detail destroyed by
undersampling, not detail missing — and the fix is the same shape: widen the
term with distance or with `NormalCurvatureToRoughness`-style derivative
feedback, and accept that a CD at 30 metres is a grey disc. **This will be the
first bug and it will be misread as a shader error.**

**Three — additive rainbow blows out.** Both `[GEMS]` and Zucconi just add the
result to the shaded colour, and Secrop's post #50 explains why that is
physically backwards: *"in Diffraction, RGB are not separated. The light still
gets reflected, in the same quantities, but the wave is simply destroyed."*
Diffraction **redistributes** energy, it does not create it. He also gives the
artistic escape hatch in the same post — set the colour above white for a
brighter, deliberately non-physical result. Fine, but it should be a knob that is
known to be a lie, not the default.

**Four — colour space.** `spectral_zucconi6` is fitted to an sRGB image. Our
pipeline is linear. Raise it to 2.2 or the rainbow is pale and the mistake looks
like a tuning problem.

---

## 8 Worked recipe: the holographic trading card

`[inferred]`, assembled from `[GEMS]` §3, `[COMMUNITY]` post #35 and `[PAPER]`
Toisoul §6.1. Written out because a foil card is the **best case** for everything
above and the exception to §7.4's main objection: the camera is close, the
surface is flat, there is one strong light, and **the card is tilted by the
user**, which is the motion the whole effect is defined by. A CD on a table is a
dull object; a CD being turned is the demo.

### 8.1 Get the sign right first, because this is where it goes wrong

The two published listings disagree, and only one of them is checkable.

`[GEMS]` uses the **unnormalised half-vector**: `u = dot(T, L + V) · d`.
`[COMMUNITY]` Zucconi uses the **difference**: `u = |dot(T,L) − dot(T,V)|`.

Derive it. Put the surface in the xz plane, `N = +y`, `T = +x`. Light at angle θ₁
on the +x side, so `L = (sin θ₁, cos θ₁, 0)` and `dot(T,L) = sin θ₁`. The mirror
direction is on the −x side: `V = (−sin θ₁, cos θ₁, 0)`, so `dot(T,V) = −sin θ₁`.

- `dot(T, L+V) = 0` at the specular direction. **Correct** — order 0 must sit on
  the mirror direction, by definition.
- `dot(T,L) − dot(T,V) = 2 sin θ₁` at the specular direction. Non-zero, so the
  rainbow fan is centred on the wrong place.

**Use `dot(T, L + V)`, unnormalised.** Zucconi's convention only works if his
`viewDir` points into the surface rather than away — and the comment thread under
that very post has a reader querying a `N·V` / `cos θ_L` mix-up which the author
acknowledges and corrects. This is an error-prone corner; the check above takes
two minutes and settles it.

Normalising `H` is the other version of the same bug. It destroys the magnitude
the grating equation is *made of*, and the result still looks like a rainbow, so
it survives review.

### 8.2 The shader

```glsl
/* Diffraction grating, after Stam (GPU Gems ch. 8) with Zucconi's refit rainbow.
 * N, V, L are unit; V and L point AWAY from the surface. T is the ridge
 * direction, in the surface plane. spacingNm is the ridge pitch in nanometres. */
vec3 gratingReflectance(vec3 N, vec3 V, vec3 L, vec3 T,
                        float spacingNm, int maxOrders, float sheenSpread)
{
    /* NOT normalised — see 8.1. u is dimensionless, 0..2. */
    vec3  H = L + V;
    float u = abs(dot(T, H));

    /* ORDERS THAT CANNOT EXIST COST NOTHING. Order n is only visible while its
     * wavelength u*d/n is still inside the visible band, so the highest useful n
     * is u*d/400. Secrop makes this point in post #33: at small spacings there is
     * no point carrying a loop for orders the geometry rules out. */
    int nMax = min(maxOrders, int(u * spacingNm / 400.0));

    vec3 rainbow = vec3(0.0);
    for (int n = 1; n <= nMax; ++n)
        rainbow += spectral_zucconi6(u * spacingNm / float(n));

    /* THE FIT IS AUTHORED IN sRGB and this pipeline is linear. Skipping this
     * reads as "the rainbow is washed out", which gets misdiagnosed as a
     * strength or exposure problem. */
    rainbow = pow(rainbow, vec3(2.2));

    /* ORDER 0 IS A SEPARATE TERM, not part of the sum: at u = 0 every wavelength
     * coincides and the sum degenerates. Ward's anisotropic lobe, straight from
     * GPU Gems. Both Stam (2004) and Secrop (post #106, 2024) arrive here. */
    float w     = max(dot(N, H), 1e-4);
    float e     = sheenSpread * u / w;
    float sheen = exp(-e * e);

    return rainbow + vec3(sheen);
}
```

`spectral_zucconi6` is the six-parabola branchless fit quoted in §3. It saturates
outside 400–700 nm, so orders that fall off the end contribute zero on their own
— which is why a fixed loop was ever acceptable, and why the `nMax` clamp is an
optimisation rather than a correctness fix.

### 8.3 The four things that make it a *card* and not a CD

**One — pin the light in view space.** This is the most important line in the
whole recipe and it is not physics, it is design. A real card works because you
tilt it under a *fixed room light*. If the virtual light is fixed in world space
and the card rotates with the world, the sweep is weak. Fixing `L` in view space
— a constant direction like up-and-left of the camera — means every degree of
card rotation moves `u` at full rate. **Tilt becomes the input to the effect.**

**Two — the foil is masked.** Real foil sits under specific art regions, not the
whole card. One greyscale mask multiplying the term is the difference between "a
card" and "a laminated disc". It also gives the art director the only control
they will actually ask for.

**Three — the pattern lives in the tangent, not the spacing.** Uniform `T` gives
straight rainbow bands, which reads as cheap foil. Everything premium is a
*varying* `T`:

```glsl
/* Rotate the mesh tangent about the normal by a per-texel angle. A voronoi or
 * cracked-ice map here is "cosmos" foil; a smooth swirl is a lens pattern. */
float  a = texture(uFoilPattern, uv).r * 6.28318530718;
vec3   B = cross(N, T);
vec3   Tr = normalize(T * cos(a) + B * sin(a));
```

`[COMMUNITY]` post #35 says exactly this — *"Tangent is variable. One can use a
voronoi cell for the rotation of the tangent"* — and `[GEMS]` fig. 8-8 is a
texture-mapped anisotropy direction for the same reason. Toisoul §6.1 reaches it
a third way, masking a radial pattern to narrow stripes.

**Four — sparkle is a separate, cheaper layer.** The glitter in a rainbow-rare is
not diffraction, it is a sparse population of individually-resolvable facets.
Threshold high-frequency noise against the half-vector and it costs one fetch:

```glsl
float g = texture(uGlitter, uv * uGlitterTiling).r;
float sparkle = smoothstep(uGlitterCut, 1.0, g * (0.5 + 0.5 * dot(N, normalize(H))));
```

Crude, but it is the right *kind* of crude — §4 tier 3 notes that the principled
answer here is a glint BRDF, and a card does not need one. Unreal ships a
licensed glint model for surfaces with countably few facets per pixel
([`material_detail.md`](material_detail.md) §8); this is the version that costs
nothing.

### 8.4 Composition, and the trap

```glsl
vec3 foil = gratingReflectance(N, V, L, Tr, uSpacingNm, uOrders, uSheenSpread);
colour   += texture(uFoilMask, uv).r * uStrength * (foil + sparkle * uGlitterGain);
```

Additive, and **this is the trap** — §7.4 item three. Post #50: *"in Diffraction,
RGB are not separated. The light still gets reflected, in the same quantities,
but the wave is simply destroyed."* Diffraction **redistributes** energy; it does
not create it. Adding a saturated rainbow on top of a lit surface will blow out
the highlights, and the correct fix is to let the foil *replace* the specular
under the mask rather than stack on it — or, if a brighter-than-real look is
wanted, do what Secrop does in the same post and drive the colour above white
deliberately. Either is fine. Doing it by accident is not.

### 8.5 Authored values to start from

| Card finish | `spacingNm` | Orders | Tangent | Notes |
|---|---|---|---|---|
| Linear / "cracked ice" foil | ~700 | 1–2 | patterned | `[COMMUNITY]` #35's holographic figure |
| Rainbow rare, fine grating | 700–1000 | 2 | patterned + glitter | sparkle carries most of the read |
| Full-art mirror foil | ~1600 | 3–4 | uniform, from UV | this is the CD setting; add anisotropy |
| Pearlescent varnish | — | — | — | **not a grating.** Thin film, §6 |

The last row matters: a pearl or "galaxy" finish that shifts smoothly through two
or three hues with no fan of repeated rainbows is thin-film interference, and no
amount of tuning `spacingNm` will produce it. Check which one the reference image
actually shows before writing anything.

### 8.6 What it costs here

`[inferred]` One interpolated tangent (§7.1 — already in the vertex stream, just
not forwarded), two texture fetches (mask, pattern) plus one optional (glitter),
a loop that is 1–4 iterations in practice, and the six-parabola fit. Comfortably
under a hundred ALU. Against a full-screen RTS view this would be a question; on
a card filling a quarter of the screen at close range it is not measurable.

It does not earn a profiler zone — it is arithmetic inside `lit scene`, not a
system.

---

## 9 Verdict

`[inferred]`

**Buildable, small, and worth a prototype — but prototype the *motion* before
writing the shader.** The ordering I would use:

1. **Tier 0, sun only, tangent from the vertex stream, on one test surface.**
   Fifty lines of GLSL and one interpolator. This answers the only question that
   matters — does it read as a hologram in this camera, at this light angle, at
   this pixel density — for a day's work.
2. If it reads: add a **tangent-rotation map** so the fan varies across the
   surface. This is the difference between "a CD" and "holographic foil", and
   `[GEMS]` fig. 8-8, Secrop #35 and Toisoul's §6.1 mask all independently arrive
   at it.
3. If it aliases (it will): widen with distance before reaching for anything
   clever.
4. Only if tier 0's colours are unconvincing: bake tier 1's 1D table from
   Toisoul's **analytic multislit** function rather than from the parabola sum,
   because that is where correct relative order brightness comes from.
5. Tier 2's photographic capture is a real option and cheap — a DSLR, an LED
   flash and a gel filter — but it is a pipeline, and it only pays if several
   distinct diffractive materials are wanted.

**Do not port the Blender node graph.** Its structure exists to work around
Cycles' lack of a diffraction closure, and every one of those workarounds — the
fake normals, the per-wavelength BSDF instances, the brick texture standing in
for a random number generator — is a cost a rasteriser does not have to pay.

**Do not write thin film.** Belcour & Barla is published, closed-form, and
shipping in two commercial engines.

---

## 10 What I could not establish

- **No shipped game.** I found no GDC talk, postmortem or engine feature
  describing a diffraction-grating BRDF in a released title. Thin film ships
  (Unity HDRP, UE5 Substrate); grating appears only in papers, tutorials and
  offline renderers. Argument from absence is weak, but the asymmetry is
  suggestive: `[inferred]` grating needs a tangent field, a moving light and a
  close camera, and most games have at most two of the three.
- **No cost measurement of tier 0 on modern hardware.** The 65 FPS figure is
  Toisoul's tier-2 shader on a 2015 laptop GPU. Tier 0's ALU count above is my
  estimate from the listing, not a profile.
- **Zucconi's part 8 ("Iridescence on Mobile")** — the baked-LUT version, the
  most directly relevant tier-1 reference — **is behind a $5 Patreon paywall.**
  Its existence and approach are confirmed only from a comment on a sibling post.
- **No cost numbers for thin film either.** Belcour's page carries none, and §6
  establishes Unreal's *structure* — one per-pixel evaluation, nothing per light
  — from source, but no measurement. The `m <= 3` loop with two `cos`, two `exp`
  and a `sqrt` per iteration is not free, and where Epic decided that was
  affordable is not recorded anywhere I read.
- **Unity's HDRP implementation was not read**, only Unreal's. The two may
  differ in exactly the way §6.3 is about — whether the term is folded into F0 or
  evaluated per light — and that is the interesting question.
- **The thread's `.blend` files were not downloaded or opened.** Everything above
  is from the post text, the OSL listings quoted in full in posts #52 and #70,
  and the sources those posts name.
- **Stam's 1999 SIGGRAPH paper was not read** — only the *GPU Gems* chapter that
  simplifies it. The general model there covers surfaces more complex than a
  regular grating, and if a natural structure (feather, beetle) ever matters, it
  is the paper to get.
