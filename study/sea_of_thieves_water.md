# Sea of Thieves water — reference notes

How Rare renders the ocean in Sea of Thieves, and which parts are worth taking.
Kept as a counterpart to [`source2_rendering.md`](source2_rendering.md): CS2 and
SoT solve *different* water problems, and knowing which one our water resembles
decides which to copy.

**[SIGGRAPH]** marks statements from Rare's own talk, *The Technical Art of Sea
of Thieves* (Ang, Catling, Cifariello Ciardi & Kozin, SIGGRAPH '18 Talks) —
linked at the bottom. **[inferred]** is our reading.

> **Sourcing caveat, and it matters here.** The primary source is a **two-page
> Talk**, not a full paper. It is authoritative on *what* was done and largely
> silent on *how much* — there are no parameter values, no LOD scheme, no mesh
> tiling details and no performance figures anywhere in it. Where this document
> is specific, it is quoting; where the talk is silent, this document says so
> rather than filling the gap.

> **Correction worth recording up front.** A great deal of secondary coverage
> states that Sea of Thieves' ocean uses **Gerstner waves**. It does not. Rare's
> own talk says the ocean "is an implementation of the FFT technique described
> in [Tessendorf 2001]". **[SIGGRAPH]** Gerstner and FFT are different methods
> with different characteristics, and the popular claim is simply wrong. Do not
> take wave-method claims from gaming journalism.

---

## 1. It is a different problem from CS2's water

Worth stating before any technique, because it decides what transfers.

| | CS2 | Sea of Thieves |
|---|---|---|
| the water is | **contained** — pools, canals, puddles, a flooded room | an **open ocean**, horizon to horizon |
| the interesting axis | *depth* — how much water is between you and the floor | *surface* — the shape and motion of an unbounded field |
| surface geometry | a flat plane, normal-mapped | a genuinely **displaced** mesh |
| what sells it | absorption through the water column | wave shape, foam, and light through wave crests |
| shading | PBR | stylised; the talk's own CCS terms list *non-photorealistic rendering* **[SIGGRAPH]** |

Neither is better. They answer different questions, and §7 below sorts out which
parts of each are worth taking.

## 2. The ocean surface — FFT

"The underlying ocean water simulation is an implementation of the FFT technique
described in [Tessendorf 2001]." **[SIGGRAPH]**

That is the whole of what the talk says about the simulation itself, and
Tessendorf's *Simulating Ocean Water* is the actual reference to read. In
outline: build a wave-height field in frequency space from an oceanographic
spectrum, inverse-FFT it to a spatial displacement field each frame, and tile
that field over the world. It produces a statistically plausible sea rather than
a sum of hand-placed waves, and it gives **choppiness** — horizontal vertex
displacement that sharpens crests and flattens troughs — as a by-product.

That choppiness by-product turns out to matter for the shading, which is §3.

The talk does not describe the mesh, the LOD scheme, or how the tile is mapped
to the world. **[SIGGRAPH silent]**

### 2.1 Why most game water reads as a bumpy sheet

The common failure — water that undulates gently instead of having *waves*, with
no crisp crests and no sense of mass — is a **geometry** problem, not a shading
one. No amount of shader work fixes it, which is why it survives in games whose
water is otherwise well shaded. Four causes, roughly in order of damage
**[inferred]**:

**1. Normal maps standing in for geometry.** A flat plane with scrolling normal
maps has **no silhouette**: the horizon stays a straight line, no wave occludes
another, and there is no parallax between crest and trough. Whatever the shading
does, it reads as a textured sheet.

Note that **CS2's water is in this family** — multi-octave wave *normals*, three
iterations, no displacement — and that is the correct choice there, because CS2's
water is contained and essentially flat. A canal or a puddle has no wave shape to
lose. It is only the wrong choice for an ocean.

**2. Vertical displacement only.** A height field `y = f(x,z)` is single-valued:
one height per horizontal position. It cannot represent a crest that leans, let
alone one that curls over. Sinusoids summed vertically produce symmetric humps —
rounded crest, rounded trough — where real gravity waves have a **sharp crest and
a broad flat trough**. That asymmetry is most of what separates "wave" from
"bump" perceptually.

**3. No horizontal displacement.** The fix for (2), and what both Gerstner waves
and Tessendorf's **choppiness** provide: displace vertices sideways as well as
vertically, bunching them toward crests and stretching the troughs. That yields
the trochoidal profile, and pushed far enough the surface folds over itself — a
genuine overhang no height field can express. §3.1's Jacobian is how that fold is
detected. **Choppiness is not a polish parameter; it is the mechanism that makes
a wave a wave.**

The self-defeating mitigation to watch for: clamping steepness to avoid the
self-intersection artifacts *removes the sharp crest the steepness existed to
create*, and lands straight back at (2).

**4. Not enough vertices where it matters.** A crest is the highest-frequency
feature on the surface and precisely where a coarse mesh smooths the shape back
into a bump. A uniform grid spends vertices on flat distant water while starving
the crests near the camera. The usual answers are a **projected grid**
(tessellate in screen space, so density is roughly uniform per pixel),
**clipmaps / CDLOD** rings around the camera, or GPU tessellation keyed to
displacement magnitude. Which of these SoT uses is not stated.
**[SIGGRAPH silent]**

**A fifth, subtler one: the spectrum.** Summing a handful of arbitrary sinusoids
gives isotropic bumps travelling in unrelated directions. An oceanographic
spectrum (Phillips, JONSWAP) with a **directional spreading function** produces
long-crested waves that run with the wind and group into sets. That large-scale
structure is a large part of what reads as a sea rather than a disturbed surface,
and it is one of the things an FFT approach gives essentially for free while a
hand-summed wave stack does not.

### 2.2 What this means for us — [inferred]

Our water, if it exists, is contained and viewed from a high tactical camera.
Grazing views are rare, so silhouette and overhang almost never matter — which
makes **CS2's normal-mapped flat plane the right model, and this section a reason
not to build displacement** rather than a reason to.

The point of writing it down is to make that a decision rather than an accident.
If water ever needs to read as an ocean — a coastal map, a boat sequence, a
cutscene at eye level — then the whole of §2.1 applies at once, and it is a
different system, not a parameter change to the flat one.

## 3. Subsurface scattering — the part we came for

Rare's model, in full: **[SIGGRAPH]**

> The water colour is based on scattering approximations. We blend between a
> deep water colour and a sub-surface water colour based on a combination of
> view angle, sun direction and a wave peak mask. The wave peak mask is
> generated from the FFT choppiness vertex offsets. Where the choppiness offset
> is greater, this corresponds to wave peaks, which show more sub-surface due to
> shorter distance traveled by light through the water.

Unpacked, because the last sentence is the whole idea:

- **There is no scattering integral.** Two authored colours — a deep one and a
  subsurface one — and a blend factor. That is the entire model.
- The blend factor combines **view angle**, **sun direction** (so crests light up
  when backlit, which is the effect everyone remembers) and a **wave peak mask**.
- The wave peak mask is a **thickness proxy**, and it is free: the FFT already
  computes choppiness offsets, and a large choppiness offset *is* a wave peak.
  A peak is thin, so light traverses less water, so more subsurface colour shows.

**The transferable insight, and it generalises past this game [inferred]:**
neither Sea of Thieves nor CS2 does real subsurface scattering in water. Both
pick a cheap **geometric proxy for optical thickness** and blend authored colours
by it. They differ only in which proxy:

| | proxy for optical thickness |
|---|---|
| CS2 | **depth of the water column** — surface-to-floor delta from the depth buffer |
| SoT | **thinness of the wave crest** — choppiness offset from the FFT |

Those are complementary, not competing. CS2's answers "how deep is this water",
SoT's answers "how thin is this bit of wave". A surface that is both deep and
wavy wants both terms.

**Do not read "no scattering integral" as "no visible effect".** The translucent
glow through a wave crest in Sea of Thieves is real, prominent, and the whole
point of the term. What is absent is the *simulation* — no diffusion profile, no
transport through the medium. The phenomenon is genuine; only the method is a
proxy. Worth stating plainly because the distinction is easy to overshoot in
either direction.

**The sun-direction term is what sells it [inferred].** Water forward-scatters
strongly, so a crest with the sun *behind* it glows while the same crest lit from
the front does not. That is why the effect shows on wave faces angled away from
the viewer toward the sun, and it is the physically correct behaviour even though
the maths is a lerp between two authored colours.

A corollary that is easy to confirm from any storm screenshot: **in overcast or
night conditions the subsurface term largely disappears**, because the
sun-direction factor has nothing to drive it. The water falls back to the deep
colour, and foam takes over as the thing carrying the surface's shape — which is
exactly the sea-state behaviour §4 describes. The model degrades in the same
direction the real phenomenon does.

### 3.1 Wave creases and whitecaps are the same quantity

Tessendorf's choppiness applies *horizontal* displacement that sharpens crests
and broadens troughs. Push it far enough and the surface compresses until it
folds over itself. The standard detector is the **Jacobian of the displacement
field**: where its determinant goes negative, the surface has folded.

That fold is visible in-game as a sharp crease along a wave face — easily read as
a "seam" in the water — and it is **also Tessendorf's foam criterion**. So the
creases and the whitecaps are two views of one quantity, which is what Rare mean
by generating foam at wave peaks "using the method described in the reference
paper". **[SIGGRAPH]** + Tessendorf.

Worth keeping precise: the **SSS wave-peak mask** uses the choppiness *offset
magnitude*, while **foam** uses the *Jacobian*, which is its gradient. Same
displacement field, closely related, **not** the same quantity — reusing one for
the other will not give the right result.

*(A second possible reading of "seams": an FFT displacement field is a tiling
patch, and its repeat period shows as periodic structure at distance. The usual
mitigation is summing several FFT cascades at non-harmonic tile sizes so the
combined period is effectively invisible. The talk does not mention cascades
either way.* **[SIGGRAPH silent]**)

## 4. Foam — the most reusable part of the talk

Three sources, composited: **[SIGGRAPH]**

1. **At wave peaks**, by the method in Tessendorf.
2. **Around objects intersecting the surface**, via **depth buffer comparisons**,
   within a **camera-centred window** (so the cost is bounded by screen area, not
   by world size).
3. The foam buffer is **progressively blurred with feedback** — the previous
   frame's blurred result feeds the next — which "simulate[s] the foam dispersing
   and … give[s] us a softer mask".

The result is then **blended with artist-authored textures** for a stylised look,
and the whole behaviour is modulated by sea state: *stormy* water gets much more
foam to read as churn, *calm* water gets foam **only** around intersecting
objects.

Two things here are worth stealing regardless of art style. The **feedback blur**
gives persistence and dispersion for the cost of one blur — foam trails behind a
moving object without any simulation. And **depth-buffer intersection foam** is
the cheapest possible way to make water look like it is genuinely touching the
things standing in it, which is the same "contact" problem SSAO solves on land.

## 5. Specular, and Snell's window

**Area specular.** "We also apply an area specular highlight to allow for a large
low sun reflection using the closest point on sphere approximation[Karis and
Games 2013]." **[SIGGRAPH]** This is UE4's representative-point area light
approximation: treat the sun as a sphere of real angular size and shade against
the closest point on it, rather than as a punctual direction. It is what produces
the long stretched glare across the water under a low sun, which a punctual
specular simply cannot make.

**This is not a water technique.** It is a general improvement to any glossy
surface under the sun, and it is worth flagging as such — see §8.

**Two specular scales, and they are not the same thing [inferred].** A sunny
screenshot shows both at once and they are easy to conflate:

- **The broad stretched glare** under a low sun — that is the area specular
  above, and it comes from giving the sun a real angular radius.
- **The dense field of small sparkles** across the whole surface — that is
  *not* the area light. It is the FFT's high-frequency detail: many small facets
  each briefly catching the sun as their normals sweep past the mirror
  direction.

The second is most of what reads as "detail" in the water, and it has a known
failure mode: at distance, those facets fall below a pixel and the sparkle field
aliases into a crawling shimmer. The standard fix is **normal-variance
roughness** — convert the sub-pixel normal distribution into extra roughness as
the surface recedes (Toksvig / LEAN-style filtering) so a mip's worth of tiny
facets becomes one appropriately rough highlight. Anything with detailed normals
under a bright sun wants this, water included.

**Snell's window.** Looking up from underwater, Rare apply a Snell's Window
effect to show the scene above the surface. **[SIGGRAPH]** The real phenomenon:
the entire above-water hemisphere compresses into a cone of about 97° (for
n ≈ 1.33), and outside that cone the surface is a mirror by total internal
reflection. Only relevant with an underwater camera.

## 6. Shallow water and incidental water

A separate system from the ocean. **[SIGGRAPH]**

- Water on a ship's deck uses a **GPU water surface simulation** based on
  [Mei et al. 2007] — a height-field/pipe-model shallow water solver, originally
  a hydraulic erosion technique.
- For waterfalls and streams, they **project the depth buffer from the camera's
  perspective onto the mesh surface, into the texture space of that mesh's
  shallow water simulation**.

That projection trick is neat and general: it lets a character *occlude* a
waterfall, and lets someone standing in a stream generate foam around their feet
where they intersect the surface — all without the water system knowing anything
about characters.

## 7. Side by side with CS2

| | CS2 `csgo_water_fancy` | Sea of Thieves |
|---|---|---|
| surface motion | multi-octave wave normal texture, 3 iterations, no displacement | **FFT** (Tessendorf 2001), genuinely displaced mesh |
| "SSS" | Beer-Lambert absorption from surface→floor depth delta | two-colour blend by view angle × sun direction × wave-peak thickness |
| refraction | screen-space grab, clamped, with chromatic separation | not discussed **[SIGGRAPH silent]** |
| reflection | three-tier: sky → cubemap → SSR over cubemap | area specular via sphere approximation; reflection strategy not discussed |
| foam | foam and debris layers, scale/wobble/tint | Tessendorf peaks + depth-intersection + feedback-blurred buffer + artist textures |
| caustics | projected, triplanar, killed in shadow | not discussed **[SIGGRAPH silent]** |
| shading model | PBR | stylised / non-photorealistic |
| underwater | `g_flUnderwaterDarkening` | Snell's window |

The silences are as informative as the entries: the SoT talk simply does not
cover refraction, reflections or caustics, so **CS2 remains the reference for
those three** and nothing here supersedes
[`source2_rendering.md`](source2_rendering.md) §12.

## 8. What to take for this project

Our water, if it ever exists, is *contained* — a canal, a flooded basement,
puddles on a tactical board — seen from a fixed tactical camera. That is CS2's
problem shape, not Rare's. So: **[inferred]**

**Take from CS2** the overall structure — refraction, absorption, caustics,
the reflection ladder. An FFT ocean solves a problem we do not have.

**Take from Sea of Thieves** three specific things, all of which are better or
cheaper than what CS2's shader documents:

1. **The two-colour scattering model.** Deep colour and subsurface colour blended
   by view angle, sun direction and a thickness proxy. It is art-directable,
   costs almost nothing, and composes with CS2's depth-based absorption rather
   than competing with it — absorption for how deep the water is, this for
   backlit thinness.
2. **Foam from depth-buffer intersection, blurred with feedback.** We already
   have a depth prepass, so the intersection test is nearly free, and this is
   what makes units wading through water look like they are in it rather than
   clipped through it.
3. **Sea-state modulation.** One parameter driving foam quantity, dispersion and
   texture blend gives calm/normal/stormy for very little. Cheap variety.

**Take for the renderer generally, not for water** — the **area specular via
closest-point-on-sphere**. Our sun is a punctual directional light, so every
glossy surface in the game gets a specular highlight of zero angular size. Giving
the sun a real angular radius is a small change to `pbr.fs.glsl` that improves
wet ground, metal, glass and water alike, and it pairs naturally with the
area-light softness the shadow work in `source2_rendering.md` §9 is chasing.

**Do not take** the FFT ocean, Snell's window, or the shallow-water GPU solver
unless the game grows an ocean, an underwater camera, or dynamic flooding
respectively. The Mei et al. height-field solver would map neatly onto our tile
lattice if flooding ever becomes a mechanic — worth remembering, not worth
building.

---

## 9. A readable reference implementation

Rare's talk says the ocean "is an implementation of the FFT technique described
in [Tessendorf 2001]" and stops. This fills that gap: an open-source Unity
implementation of the same pipeline, complete and short enough to read in an
afternoon. **[repro]** — it is not Rare's code, but it implements the same
method, and it is *far* more informative than anything extraction would yield.

[`Biebras/Ocean-Simulation-Unity`](https://github.com/Biebras/Ocean-Simulation-Unity).
Core is eight files: four compute shaders, three C# scripts, one surface shader.
Check its LICENSE before reusing any of it; what follows documents *technique*.

### 9.1 It is more advanced than the 2001 paper

Worth knowing, because "implements Tessendorf" understates it:

| | Tessendorf 2001 | this implementation |
|---|---|---|
| spectrum | Phillips | **JONSWAP** with a **TMA** finite-depth correction |
| inputs | wind speed, direction | wind speed, wind angle, **fetch**, **depth**, **swell** |
| directionality | implicit in the spectrum | explicit **directional spreading**, energy-normalised |
| scales | one FFT | **three cascades** with wavenumber cutoffs |

**The spectrum choice is a large part of why it looks real.** JONSWAP is
fetch-limited — it models a sea built by wind blowing a finite distance, which is
what actual seas are — and *fetch* is a directly authorable parameter. Phillips,
the 2001 default, is the weaker model.

**Directional spreading is the other large part.** `BaseSpread` is a sech² lobe
whose width varies with frequency relative to the spectral peak, multiplied by a
swell term, then divided by a numerically integrated total so total energy is
conserved. This is what produces **long crests aligned with the wind** rather
than isotropic lumps — the distinction §2.1 identifies as separating a sea from a
disturbed surface.

### 9.2 Cascades — the fix for the tiling seam

`CascadeSettings` carries a `LengthScale` plus `LowCutoff` / `HighCutoff` in
**wavenumber**. Three cascades run as three independent FFTs, the cutoffs
partition the spectrum so no wavelength is counted twice, and the shader sums all
three — displacement in the vertex stage, normals and foam in the fragment stage,
each sampled at `worldUV / _LengthScaleN`.

This does two jobs at once: it removes the visible repeat (§3.1's second reading
of "seams"), because the combined period is the LCM of three unrelated tile
sizes; and it spans swell down to ripples, which a single FFT resolution cannot.

### 9.3 Choppiness, in code

The horizontal displacement term — §2.1's "the mechanism that makes a wave a
wave" — is three lines in `TimeDependantSpectrum.compute`:

```hlsl
float2 ih = float2(-h.y, h.x);                    // multiply by i
float2 displacementX = oneOverKLength * k.x * ih; // -i·(k/|k|)·ĥ
float2 displacementZ = oneOverKLength * k.y * ih;
```

and applied in the vertex shader with the asymmetry that matters:

```hlsl
v.vertex.x += displacement.x;   // horizontal offsets ACCUMULATE
v.vertex.y  = displacement.y;   // height is ABSOLUTE
v.vertex.z += displacement.z;
```

`DisplacementFactor` is the choppiness scale λ, and it is applied **both** to the
displacement and to the Jacobian below — correct, since how far the surface folds
depends on how hard it is displaced.

### 9.4 Jacobian foam, in code

Confirms §3.1 exactly. The partials are computed in frequency space alongside the
displacement:

```hlsl
j_xx = oneOverKLength * k.x * k.x * -h;
j_zz = oneOverKLength * k.y * k.y * -h;
j_xz = oneOverKLength * k.x * k.y * -h;
```

then combined into the determinant and turned into foam:

```hlsl
float jacobian = (1 + λ*j_xx) * (1 + λ*j_zz) - (λ*j_xz) * (λ*j_xz);
jacobian *= -1;
jacobian += FoamIntensity;

float accumulation = FoamMap[id].x - FoamDecay * DeltaTime / max(jacobian, 0.5);
float foam = max(accumulation, jacobian);
```

Foam appears where the determinant goes **negative** — where the surface has
folded over itself. The accumulation line gives **persistence with decay**, so
foam trails behind a crest and fades instead of flickering on and off. Same
intent as Rare's feedback-blurred foam buffer (§4), reached by a different
mechanism — a per-texel decay rather than a blur.

### 9.5 The scattering model — what §3's "fake SSS" looks like in code

This is the most directly useful thing in the repository:

```hlsl
part1 = _Tweak1 * max(0, posWorld.y)                          // 1. height above the waterline
      * pow(DotClamped(sunDirection, -viewDirection), 4.0)    // 2. looking toward the sun
      * pow(0.5 - 0.5 * dot(sunDirection, normal), 3.0);      // 3. face turned away from the sun
part2 = _Tweak2 * pow(DotClamped(viewDirection, normal), 2.0);

scatter = (1 - fresnel) * (part1 + part2) * _WaterScatterColor * _LightColor0;
```

Three conditions multiplied together, and all three must hold for the term to
fire: the wave is **tall**, the viewer is looking **toward the sun**, and the
wave face is **turned away** from it — i.e. backlit. That is exactly the physical
configuration for light transmitting through a thin crest.

Two details worth carrying over:

- **The thickness proxy is just world-space height** (`max(0, posWorld.y)`) —
  cruder even than Rare's choppiness mask, and it works. Crests glow, troughs do
  not. Reinforces §3's point that the *proxy* barely matters; having one does.
- **`(1 - fresnel)` gates the whole thing.** Only light that was not reflected
  can scatter. The same `R + T = 1` bookkeeping that drives glass opacity in
  `source2_rendering.md` §12.1 — one principle, two materials.

There is also a separate **air-bubbles** term in the ambient:

```hlsl
ambient = _Tweak3 * normal * _WaterScatterColor * _LightColor0
        + _DensityOfWaterBubbles * _AirBubblesColor * _LightColor0;
```

Entrained air from breaking waves scattering light — a real effect, and part of
why churn reads as white-green rather than grey.

**Not PBR**, exactly as with SoT: ambient + scatter + specular + cubemap
reflection, weighted by Fresnel. Two independent implementations of good-looking
water both declining a principled BRDF is itself a data point.

**Provenance note [inferred]:** this specific scatter formulation (the two-part
sum, the water-scatter and air-bubble colours) appears in several open ocean
implementations and is not this author's invention. It is most likely descended
from the Atlas water talk (*Wakes, Explosions and Lighting: Interactive Water
Simulation in Atlas*, SIGGRAPH 2019). Not verified — treat the attribution as a
lead, not a fact.

### 9.6 What to take from it

Beyond §8's list, which this does not change:

- **The scatter formulation itself** is directly portable and needs no FFT. It
  wants a height-above-waterline term, a view-sun term and a normal-sun term —
  all available for any water surface, including a flat normal-mapped plane.
  This is the cheapest route to the effect the screenshots show.
- **Foam accumulation with decay** is a two-line idea worth having wherever foam
  exists, with or without a Jacobian to drive it.
- **The `(1 - fresnel)` gate** should be applied to any transmission term we
  write, water or glass.

---

## 10. Big waves and storms — why FFT alone does not get there

A reasonable complaint about §9's reference implementation is that it does not
produce the *mass* that Sea of Thieves' storms have — no towering waves, no
churn. Three separate causes, and only the first is a knob.

### 10.1 Parameters — the height is available and not dialled in

JONSWAP amplitude is driven by two inputs together:

```
alpha  = 0.076 · (U² / (F · g))^0.22        // energy scale
omega_p = 22 · (g² / (U · F))^0.33          // peak frequency
```

Big waves need **both** high wind speed `U` **and** long **fetch** `F`. Fetch —
the distance the wind has blown over open water — is the one usually forgotten,
and it is what converts wind chop into long swell. High wind over a short fetch
produces a steep, nasty, *small* sea. Raising `WindSpeed` alone will not get
there.

### 10.2 The cascade band may be discarding the biggest waves

The longest wave a cascade can represent is its `LengthScale`, and `LowCutoff`
removes low-wavenumber components outright — precisely the long, tall ones. If
the largest cascade's `LengthScale` is too small, or its `LowCutoff` too high,
the storm swell is deleted before the IFFT runs. Check the low-k end of the
largest cascade before touching anything else.

### 10.3 The ceiling: height fields cannot break

This one is structural and no parameter reaches it. From the Arc Blanc real-time
ocean framework, on assuming `y = h(x,t)`:

> This hypothesis excludes a lot of physical phenomena like breaking waves, but
> it helps to reach large scale domain.

That is §2.1's argument confirmed from a peer-reviewed source. Choppiness — the
horizontal displacement of §9.3 — is a first-order correction: it leans crests
and can drive the surface into self-intersection (which is what §9.4's Jacobian
detects), but it **cannot produce a wave that curls over and throws water
forward**. A genuinely plunging breaker is outside the model.

### 10.4 What dramatic oceans actually do — hybrid Gerstner + FFT

The standard answer is to stop asking FFT to do it alone:

- **FFT** for the statistically correct sea — spectrum, spreading, detail.
- **A handful of large Gerstner waves** layered on top for **authored** swell,
  whose height, wavelength and direction are set directly rather than emerging
  from a spectrum. This is what gives a "wall of water" on cue.
- **One sea-state control** (Beaufort) driving both, so calm→storm is a single
  parameter.
- **A folding/steepness threshold** for whitecaps — the same Jacobian criterion
  as §9.4 under a different name.

**Attribution.** The hybrid description above comes from the *Oceanology*
plugin, which is an **Unreal Engine** plugin — not Unigine; search results
conflate the two. Unigine's own ocean has since been checked separately and is
**not** a hybrid at all: it is pure Gerstner. See §11.

Rare's talk does not say whether SoT layers anything on top of its FFT.
**[SIGGRAPH silent]**

### 10.5 Most of the "storm" is not the height field

Worth separating, because it is the cheapest part and the most overlooked. A
stormy sea reads as stormy because of:

- **Foam coverage**, raised dramatically — Rare state this directly: stormy water
  gets much more foam "to give the impression of the churn", calm water gets foam
  only around intersecting objects. **[SIGGRAPH]**
- **Spray particles**, which no height field produces. Drive emission from the
  Jacobian where it goes most negative — the surface is folding there, and §9.4
  already computes the buffer.
- **Lighting and sky** — the subsurface scatter term of §9.5 depends on sun
  direction, so under storm cloud it collapses on its own and the water falls
  back to its deep colour. The model degrades correctly for free.
- **Rain, and reduced visibility.**

§9's reference implementation has foam but no spray, no rain, no sea-state
presets and a fixed sky. At *identical wave heights* it still would not read as a
storm. If the goal is the storm look rather than the wave physics, this section
is worth more than §10.1–10.4 combined.

---

## 11. Unigine Global Water — the other end of the design space

Checked because Unigine's ocean visibly achieves the wave *mass* §10 is about.
The answer is not a better spectrum. **[UNIGINE]** marks Unigine's own docs;
**[UNIGINE-SRC]** marks their **shader source**, read directly — see §11.5.

> **Licence.** The source discussed from §11.5 on is part of the UNIGINE 2 SDK
> and is covered by the UNIGINE License Agreement. It is quoted here in short
> excerpts for study only. **Do not paste any of it into this project.** Read
> it, understand the technique, implement independently.

**Global Water is Gerstner, not FFT.** It is "an infinitely spread mesh with
auto-tessellation" running "real-time water simulation based on fast
implementation of **Gerstner waves** model". **[UNIGINE]** No Fourier transform
anywhere.

That is worth sitting with, because it means the two best-looking oceans in these
notes are built on opposite methods.

### 11.1 How it is controlled

- **Up to 256 simulated waves** (docs recommend ~100), organised into **wave
  layers / groups** ordered largest to smallest. **[UNIGINE]**
- **Per-wave-system control via API**: spectral parameters, direction, length,
  amplitude and a **shape factor**, all settable at runtime. **[UNIGINE]**
- **Beaufort mode**: presets reproducing sea states **0 (Calm) to 12
  (Hurricane)**, with smooth transitions between them. In this mode the main
  wave-geometry parameters are **locked** — a calibrated preset in exchange for
  direct control. **[UNIGINE]**
- **Steepness Scale**, which "affects the sharpness of the wave crests".
  **[UNIGINE]**
- **Auto-tessellation** on an infinite mesh.

### 11.2 Why this produces big waves where §9's FFT does not

Directly answers §10: **a Gerstner wave's amplitude is a number you type.**
There is no spectrum standing between the author and the wave height. Set it to
ten metres and it is ten metres.

| | FFT (Tessendorf, §9, and SoT) | Gerstner sum (Unigine) |
|---|---|---|
| where waves come from | a statistical spectrum | up to 256 individually parameterised waves |
| amplitude | **emergent** from wind speed and fetch | **explicit**, per wave |
| getting a big wave | tune spectrum inputs, hope | set an amplitude |
| irregularity | free, and natural | must be bought with wave count |
| art direction | indirect | direct |
| cost | O(N log N) once into textures, then cheap lookups | O(waves) per vertex — 256 is heavy |

All three of §2.1's causes are addressed here by **explicit controls** rather
than by getting a spectrum right: amplitude per wave, `Steepness Scale` for crest
sharpness, and auto-tessellation for enough vertices at the crest.

**Gerstner is not the lesser technique.** A Gerstner wave displaces horizontally
*and* vertically by construction — it is the original trochoidal formulation, and
Tessendorf's choppiness (§9.3) is essentially its FFT equivalent. So it has
§2.1's sharp-crest property natively. It can also over-steepen into
self-intersection exactly as choppiness can, which is what `Steepness Scale`
exists to bound.

**§10.3's ceiling still applies.** Gerstner is a parametric surface, not a
volume, so it cannot produce a genuinely plunging breaker either. It simply gets
much closer, because amplitude and steepness are directly dialable.

### 11.3 Its shading is the most complete of the three

Global Water ships parameters for **procedural foam** and **subsurface
scattering**, **planar reflections**, **underwater** rendering whose final colour
combines `FogColor`, `FogSunLighting` and `FogEnvLighting`, and **caustics
rendered as a post effect** alongside fog and an underwater mask. Field-height
objects locally adjust water height. Materials are `water_global_base` and
`water_mesh_base`. **[UNIGINE]**

Notably that is a *fuller* underwater treatment than either CS2 or Sea of Thieves
documents — CS2 has a single `g_flUnderwaterDarkening`, SoT has Snell's window,
Unigine has a lit fog model plus post-effect caustics.

### 11.5 Read from the SDK source

The Community SDK ships `data/core/` **unpacked** — no archive to open, no
capture needed. The entire water system is about **3,000 lines of readable
UUSL** under:

```
<sdk>/data/core/materials/base/objects/water/
  global/shaders/gerstner_waves.h      the wave model
  global/shaders/common.h              surface helpers
  global/shaders/parameters.h          every uniform
  global/shaders/deferred.{vert,frag}  the surface pass
  global/shaders/default.{cont,eval}   hardware tessellation
  global/shaders/raytrace.frag         a raytraced path
  render/overwater/composite_deferred_overwater.frag    the lighting
  render/underwater/composite_deferred_underwater.frag
  render/shaders/screen_space/common.h  scattering functions
```

(`.cont`/`.eval` are hull and domain shaders — the auto-tessellation of §11.1 is
genuine hardware tessellation.)

### 11.6 The Gerstner core, and three techniques worth stealing

Each wave is a struct in a structured buffer — `direction`, `amplitude`,
`length`, `magnitude` (wavenumber), `frequency`, `phase`, **`steepness`** — and
the sum is textbook trochoidal, Z-up: **[UNIGINE-SRC]**

```hlsl
float theta = data.magnitude * dot(data.direction, position)
            - data.frequency * s_water_animation_time + data.phase;
displacement += float3(-sin_theta * data.direction * data.steepness, cos_theta)
              * amplitude_faded;
```

**`steepness` scales the horizontal term only** — direct confirmation of §2.1:
horizontal displacement is the crest-sharpening knob, and here it is per-wave.

**(a) Anti-aliasing fade by polygon size — and an early break.** This is the
best idea in the file:

```hlsl
float fade = 1.0f - saturate(poly_size * magnitude - 1.0f);
...
if (aa_fade <= EPSILON) break;
```

A wave whose wavenumber is too high for the local polygon size is **faded out
of the geometry entirely**. Waves are ordered large→small, so the loop `break`s
at the first unresolvable one. That is simultaneously geometric anti-aliasing
*and* the answer to "how do you afford 256 waves": near the camera with fine
tessellation you evaluate many, far away with coarse polygons you exit after a
handful. §2.1's cause 4 is solved by *removing* waves the mesh cannot represent
rather than only by adding vertices.

Its complement is the `s_distant_waves_*` parameter block — a texture-based
normal detail blended in over distance, replacing the geometric waves the fade
just removed. Geometry near, texture far.

**(b) Analytic normals.** Because a Gerstner sum is differentiable in closed
form, tangent and binormal come from differentiating the same loop — no normal
map, no finite differences, no derivative textures. An FFT ocean needs extra
transformed buffers for this; Gerstner gets it free.

**(c) The foam mask is the Jacobian again.** After building the frame:

```hlsl
info.normal = cross(info.tangent, info.binormal);
info.foam_peak_mask = info.normal.z;      // taken BEFORE normalize
```

The un-normalised cross product's Z component is `t.x*b.y − t.y*b.x` — exactly
the **2D Jacobian determinant of the horizontal displacement**. So Unigine's
foam criterion and Tessendorf's (§9.4) are the *same quantity*, reached from two
completely different wave models. Strong signal that this is the right criterion
rather than an artefact of FFT.

**(d) The problem nobody mentions: querying height is not free.** Because
Gerstner displaces horizontally, "the wave height at world XY" is not the sum of
cosines — you must invert the displacement to find which parameter position
lands at that XY. `calcWavesHeightAtPosition` runs a fixed-point iteration
`WATERLINE_ACCURACY` times, commented "for high beauforts with steepness this
cycle try to guess error offset by xy coords". **[UNIGINE-SRC]** Anything doing
buoyancy or underwater detection against a horizontally-displaced surface hits
this, FFT included.

### 11.7 Deferred, with a composite pass

Unlike CS2 (forward) and §9's Unity shader (forward), Unigine's water is
**deferred**: the surface `deferred.frag` writes normals and foam into MRTs, and
the actual water lighting happens in `composite_deferred_overwater.frag`. That
pass, in order: **[UNIGINE-SRC]**

1. **Refraction** — `calcRefractedUV(uv, water_data.refraction_scale, data)`,
   with a depth compare against `prewater_depth` to reject samples in front of
   the surface (the trap flagged in §12.3 of the Source 2 notes).
2. **A reflection ladder**, layered by mask exactly as CS2's is:
   environment probe → **planar reflection** → **SSR**, each `lerp`ed over the
   last, then multiplied by `calcWaterFresnel(dotNV)`.
3. **Subsurface** — §11.8.
4. **Fog, bottom colouring and caustics**, with a `water_depth_lut` texture for
   depth-based colour.

Note there are **four** independent foam systems in `parameters.h` — peak,
whitecap, wind and contact — each with its own contrast and intensity, plus
separate shoreline foam. SoT documents two.

### 11.8 The subsurface function, in full

The whole thing: **[UNIGINE-SRC]**

```hlsl
float calcSubsurfaceScattering(float3 normal, float3 view_direction,
                               float3 light_vector, float subsurface_wave_intensity)
{
    float dotLV = dot(view_direction, light_vector);
    float visibility = saturate((dotLV + 1.0f) / 2.0f);
    float visibility_gradient = pow(saturate(1.0f - pow(1.0f - visibility,
                                    subsurface_wave_intensity)), 3.0f);
    float dotLW = dot(light_vector, -normal);
    float subsurface = pow((dotLW + 1.0f) / 2.0f, 3.0f);
    return (subsurface * visibility_gradient);
}
```

Two factors: **looking toward the sun** (`dot(view, light)` remapped and shaped),
and **the normal facing away from the light** (`dot(light, -normal)`, cubed) —
i.e. backlit. Structurally identical to §9.5's `part1` and to Rare's prose
description in §3.

At the call site it is composed from five additive contributions — ambient,
main-through-waves, foam-around (twice, environment and diffuse), and decals —
and then:

```hlsl
float diffuse_brightness = saturate(1.0f - fresnel);
sss *= diffuse_brightness;
```

**Three independent implementations, one principle.** Unigine, the Unity FFT
ocean (§9.5) and Source 2's glass (`source2_rendering.md` §12.1) all gate
transmission on `1 - fresnel`. Only light that was not reflected can enter the
medium. If we write one transmission term in this renderer, it gets that gate.

One difference worth noting: Unigine's function has **no thickness proxy** —
no wave height, no choppiness mask. It substitutes the **foam mask** as the
stand-in for thin, aerated water (`subsurface_around_foam` keys off the foam
channel). A third proxy for the same quantity, reinforcing §3's point that
having one matters more than which.

### 11.4 Read

Three oceans, three positions on one axis: **[inferred]**

- **CS2** — no displacement at all. Correct for contained water.
- **SoT / §9** — FFT. Statistically correct sea, indirect control, natural
  irregularity for free.
- **Unigine** — Gerstner sum. Direct authored control, drama on demand,
  irregularity bought with wave count and vertex cost.

For anything wanting *art-directed* water — a scripted storm, a hero wave, a
sea state that changes on cue — the Gerstner end is the better fit, and the
Beaufort presets show how to keep it authorable. For water that should simply
*be* a convincing sea with no one directing it, FFT gives more for less effort.

None of this changes §8 or §2.2 for our project: the water we would plausibly
need is contained and viewed from above, and stays flat.

---

## Sources

- [The Technical Art of Sea of Thieves](https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf)
  — Ang, Catling, Cifariello Ciardi & Kozin, Rare Ltd., SIGGRAPH '18 Talks. The
  primary source for everything marked **[SIGGRAPH]**.
- Jerry Tessendorf. 2001. *Simulating Ocean Water* — the actual ocean simulation
  method, cited by the talk.
- Brian Karis / Epic Games. 2013. *Real Shading in Unreal Engine 4* — the
  closest-point-on-sphere area specular approximation.
- Xing Mei, Philippe Decaudin, Bao-Gang Hu. 2007. *Fast Hydraulic Erosion
  Simulation and Visualization on GPU* — the shallow-water solver.
- [Tessendorf's own course notes](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)
  — the full derivation, freely available from his Clemson page. The primary
  source for the dispersion relation, choppy waves and the Jacobian.
- [`Biebras/Ocean-Simulation-Unity`](https://github.com/Biebras/Ocean-Simulation-Unity)
  — the reference implementation read in §9. JONSWAP + TMA, directional
  spreading, three cascades, Jacobian foam, and the scattering model.
- [Arc Blanc: a real time ocean simulation framework](https://arxiv.org/pdf/2503.03326)
  — the source for §10.3's statement that the height-field assumption excludes
  breaking waves. Also uses a Donelan-Banner directional spectrum under Beaufort
  sea-state control, and Tessendorf's choppy-wave displacement.
- [Oceanology (Unreal plugin) — Beaufort Scale](https://galidar.github.io/oceanology-docs/nextgen/getting-started/features/Beaufort-Scale.html)
  — hybrid Gerstner + FFT under one sea-state control (§10.4). **Unreal, not
  Unigine.**

For §11, marked **[UNIGINE]** — Unigine's own documentation:

- [Global Water](https://developer.unigine.com/en/docs/latest/objects/objects/water/water_object)
  — the Gerstner wave model, wave layers, Beaufort presets, steepness, foam,
  subsurface scattering, underwater and caustics parameters.
- [`water_global_base` material](https://developer.unigine.com/en/docs/2.14.1/content/materials/library/water_global_base/)
  and [`water_mesh_base`](https://developer.unigine.com/en/docs/latest/content/materials/library/water_mesh_base/).
- [Water Optimization](https://developer.unigine.com/en/docs/future/content/optimization/water/).

*(The docs site is JavaScript-rendered and does not scrape; §11.1–11.4 were read
via search-result summaries. §11.5 onward is read from the SDK source itself and
is reliable.)*

For §11.5–11.8, marked **[UNIGINE-SRC]** — the UNIGINE 2 Community SDK 2.17.0.1,
installed locally. `data/core/` ships **unpacked**; the water system is under
`data/core/materials/base/objects/water/`. Covered by the UNIGINE License
Agreement — study only, do not copy into this project.
