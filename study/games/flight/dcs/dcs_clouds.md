# DCS World's clouds — the third data point, and the only one that shows the bill

This directory already has two cloud notes, and both are read from something
better than this one has. [`unigine_clouds.md`](../../rendering/unigine_clouds.md) is read from
**UNIGINE's own shader source** — the density function, the step schedule, the
phase lobes, the reprojection, all of it, line by line.
[`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md) is read from **Rockstar's own
SIGGRAPH talk**, with formats, resolutions and PS4 millisecond costs.

**DCS can match neither.** Eagle Dynamics ship native C++ and compiled shaders,
have given no talk and published no paper. What they *have* published is an
eleven-page white paper, *Volumetric Weather: DCS History Excursion and
Techniques*, and it is a strange and valuable document: almost nothing about the
algorithm, and an unusual amount about **what went wrong, what they had to give
up, and what they still cannot do.**

So this note is deliberately not a third attempt at "how do you raymarch a
cloud". §3 says what the density function must contain and then points at
UNIGINE for the actual code. **What DCS uniquely provides is the four things a
shader listing cannot tell you**, and they are the reason to write it:

1. **A published post-mortem of a shipped-then-withdrawn cloud system** (§1).
   Nobody publishes these.
2. **A spherical earth, sixteen layers and a 400 km radius** (§4) — RDR2's and
   UNIGINE's systems are both effectively local and flat by comparison.
3. **The fog unification and the voxel-resolution conflict** (§6), stated by ED
   with a candour that ends *"today, we do not know of a single successful
   implementation."*
4. **The bill.** DCS's volumetric clouds are better than the particle clouds they
   replaced in every visual respect and **lost a gameplay capability the particle
   system had** (§11), and the content model had to collapse from parameters to
   thirty presets (§8). Neither of the other notes has to talk about this,
   because an engine feature and a single-player renderer do not have to answer
   to a simulation.

Point 4 is the one that matters to this project.

## 0. Sourcing

| Tag | Meaning |
|---|---|
| **[ED]** | Eagle Dynamics' *Volumetric Weather* white paper and newsletters. Quoted directly where it matters. |
| **[MIZ]** | DCS's own mission-file weather schema and the complete cloud preset table, read from `pydcs`, a library that reads and writes real `.miz` files. This is the shipped data, mirrored. |
| **[COMMUNITY]** | Forum, mod and reporting sources. |
| **[reconstructed]** | **My reconstruction of the algorithm from ED's constraints plus the published literature the technique family comes from.** §3 is entirely this. It is not evidence about DCS's code, and it is marked at every step. |

The distinction between [reconstructed] and the rest is the whole integrity of
this note, and §15 says plainly what could not be established — which is most of
the implementation.

---

## 1. Three generations, and the one that was withdrawn

ED's history section is the most useful page in the paper because it is the only
published account of a **shipped cloud system being pulled**.

### 1.1 Particles (1999 → 2021)

> The first iterations were implemented based on a particle system where each
> cloud was represented as a separate object. **This made it quite easy to
> scatter them around the maps and use this data to calculate line of sight
> blocking.** However, this was a rather inefficient rendering technique with
> many issues and shortcomings. These included sorting problems and
> rotating/intersecting particles when moving the point of view. Additionally,
> with particles, it is almost impossible to effectively describe and render huge
> multi-layered cloud formations that cast shadows on the terrain, objects, and
> themselves, while adequately taking into account the scattering and attenuation
> of light. **[ED]**

Read that first sentence again, because §11 is entirely about it. **The particle
representation's *first* stated advantage is that the simulation could query
it.** A cloud was an object with a position and a radius; "is this line of sight
blocked" was a test against a list of spheres. That is a cheap, exact,
CPU-side query, and it was free because the rendering representation happened to
be made of objects.

The listed failures are all rendering failures: sorting, view-dependent
rotation, no multi-layer, no self-shadowing, no proper scattering. Every one is
fixed by going volumetric. The one thing that was working is the one thing that
broke.

### 1.2 The 2014 raymarch, which shipped and was withdrawn

> In 2014, the first version of raymarch clouds was created. These were
> **single-layer clouds designed for flat earth** which suffered from noticeable
> limitations in quality and rendering distance as well as self-shadowing issues.
> A global coverage map was supported, and weather fronts could be created.
> Taking into account the limited power of video cards at that time, it was not
> possible to fit them into the rendering frame budget in a way that would satisfy
> the majority of our customers. **Those of you who have been with us for a while
> may remember how an earlier update included clouds being rendered at the edge of
> the render field and how they would slide by, simulating movement.** From your
> feedback, it became clear that this new cloud technology was not ready based on
> available hardware. **[ED]**

Two things worth extracting.

**The failure mode is specific and recognisable.** Clouds rendered only at the
far edge of the view and slid past — i.e. the march was affordable only at long
range and low angular density, so they shipped it as a distant band and it read
as a scrolling backdrop. That is what "we could not afford the near field"
looks like from the outside, and it is exactly the failure
[`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md) §5.2 describes Rockstar solving —
*"ray placement: three attempts, and why the first two failed"*.

**Seven years elapsed between the withdrawn version and the shipped one** (2014 →
2021), and ED name hardware as the blocker. A volumetric cloud system is not a
feature you can decide to have; it is a feature you can decide to have *when the
median GPU can afford it*, and the gap between "we can implement this" and "our
customers can run this" was most of a console generation.

### 1.3 The 2021 rewrite

> In 2021, a new volumetric cloud system was released, **written almost from
> scratch**. This system:
> - Was designed for a **spherical earth**;
> - Allowed **up to 16 independent layers**;
> - Had a drawing radius of **400 km**; the radius can be increased even more
>   without much loss of quality;
> - Had extremely advanced and efficient mechanisms for integrating lighting and
>   cloud composition;
> - **Self-shadowing was no longer limited to a small radius; clouds on the horizon
>   could now cast a shadow across the entire map**;
> - All transparent objects and effects blended with the clouds;
> - There was no longer the problem of sorting effects and glass with clouds based
>   on the particle system;
> - **Rain and snow were now part of the system**;
> - Most of the optical effects that appear on clouds had been implemented like
>   rainbows, moonbow, halo, glory, fogbow, and more. **[ED]**

And the governing idea:

> Clouds no longer exist as separate objects, but are rather **represented as
> continuous volumes in which the cloud density is known at each point in
> space.** To draw such a volume, you must calculate each pixel along the line of
> sight and calculate the fair integral of the illumination and opacity of the
> clouds. **[ED]**

"Clouds no longer exist as separate objects" is the sentence that costs them the
LOS query. It is stated as a benefit, and for rendering it entirely is.

---

## 2. What is actually known about the technique

Stripping ED's paper to load-bearing technical statements, this is the complete
list of what is *established* about how DCS draws a cloud:

1. It is **raymarching** through a continuous density field. **[ED]**
2. The field is defined over a **sphere**, not a plane. **[ED]**
3. Up to **16 independent layers**. **[ED]**
4. **400 km** draw radius. **[ED]**
5. **The frame is not marched completely in one pass**; **temporal reprojection**
   reuses cloud samples drawn in previous frames. **[ED]**
6. Self-shadowing is unbounded in radius — horizon clouds shadow the whole map. **[ED]**
7. Fog is stored **in the same volume as the cloud data** and drawn in the same
   pass. **[ED]**
8. Clouds and fog **receive and scatter light from discrete light sources and from
   the Earth** — city lights illuminate cloud bases. **[ED]**
9. A **physical model of light scattering**, "similar to the one used in our sky
   model". **[ED]**
10. Precipitation (rain, snow) is part of the same system, and there is a **rain
    volume** that fog blends with. **[ED]**

That is it. There is no published statement of the noise basis, the sampling
schedule, the phase function, the transmittance model, the light-march strategy,
the texture formats, the resolutions, or the cost. §15.

---

## 3. The density function — what must be there [reconstructed]

**This section is reconstruction and contains no evidence about DCS's code.** It
is here because "clouds are a continuous density field" is not actionable, and
because the reconstruction is tightly constrained: ED's ten facts above plus the
visual behaviour rule out most of the design space.

Everyone in this technique family — Schneider's *Nubis* work for Horizon Zero
Dawn, Bauer's RDR2 system, UNIGINE, and by every appearance DCS — builds
`density(p)` from the same four ingredients, and
[`unigine_clouds.md`](../../rendering/unigine_clouds.md) §2 has the real version read from real
source. The skeleton:

```
density(p):
    coverage   = sample2D(coverageMap, p.xz)         # where clouds are, and what type
    heightFrac = (altitude(p) - layerBase) / layerThickness
    gradient   = heightProfile(cloudType, heightFrac) # the vertical silhouette
    base       = coverage * gradient
    base       = erode(base, noise3D_low(p))          # SUBTRACT low-frequency noise
    detail     = erode(base, noise3D_high(p + curl))  # SUBTRACT high-frequency, at the edges
    return detail
```

Four things about this shape are worth stating because they are the parts people
get wrong, and all four are confirmed in the UNIGINE note from actual source:

- **Noise erodes; it does not add.** You start with a filled shape defined by
  coverage × height gradient and *subtract* noise to carve it. Adding noise to
  zero gives you fog everywhere; subtracting from a filled shape gives you
  cauliflower.
- **The height gradient is per cloud type**, and the type comes out of the
  coverage map. That is how one field produces stratus, cumulus and cumulonimbus
  in the same volume — the thing ED explicitly say the single-layer 2014 version
  could not do.
- **Detail noise is applied at the edges only**, weighted by how close the base
  density is to its boundary. Applying it everywhere is both a waste of samples
  and visually wrong.
- **Curl noise offsets the detail sample** to make wispy, advected edges rather
  than isotropic fizz.

**What DCS must have that this skeleton does not show**, from ED's own facts:

- `layerBase` / `layerThickness` are **per-layer, up to 16**, and the march must
  traverse layers in order and accumulate through gaps between them.
- All of `p.xz` is **spherical**, not planar (§4).
- The same volume also carries a **fog density** at a different spatial frequency
  (§6), which is the conflict that made this hard.

Where the reconstruction stops: the noise basis (Perlin-Worley? Worley octaves?),
the texture resolutions and formats, whether the coverage map is a texture or
procedurally generated from the preset, and how the 16 layers are packed. None of
that is published. **For working code, read the UNIGINE note; it is the same
family and it is real.**

---

## 4. Spherical earth and 400 km — the genuinely different requirement

This is where DCS's problem stops resembling the other two notes'.

RDR2's volumetrics live in a **frustum-aligned froxel volume** near the camera
plus a raymarch beyond, over a map you can cross on horseback. UNIGINE's clouds
sit in one of three world shapes with a bounded march. Neither has to be correct
at 400 km, and neither has to be correct for a viewer at 15,000 m looking down
at the curve of the Earth.

DCS does, and the consequences chain:

**The layers are spherical shells, not slabs.** A "cloud layer at 3,000 m" is a
shell at a constant radius from the planet centre. At 400 km, a flat slab would
be wrong by roughly `d²/2R` = `400000²/(2 × 6371000)` ≈ **12.5 km of altitude
error** at the far edge — the layer would be at the wrong altitude by more than
the layer's own height range, several times over. Flat-earth clouds are not an
approximation at this distance; they are a different sky.

**This is why the fog rewrite was forced.** ED say it directly: the old fog was
computed for a flat Earth and the new clouds for a spherical one, so *"they cannot
blend with each other"* **[ED]**. Two systems that disagree about the shape of the
world cannot be composited, at any quality level. **A geometry assumption is not a
local detail; it is a contract between every system that touches the same space.**

**Self-shadowing across the whole map** (ED's own bullet) is only meaningful
because of the radius. A cloud on the horizon casting a shadow on the terrain
under you is a 400 km-baseline light transport problem, and it is not solvable by
the small-radius shadow-cone trick that UNIGINE uses for local self-shadowing
([`unigine_clouds.md`](../../rendering/unigine_clouds.md) §6.1 — *"occlusion: a fixed cone, not a
march"*). Something map-scale must exist — most plausibly a low-resolution
cloud-shadow map rendered from the sun's direction over the whole coverage field,
sampled by the terrain and object shaders. **[reconstructed]**

---

## 5. Temporal reprojection, and the sentence that admits the budget

> Even today, the technique of raymarching clouds is quite computation-heavy, and
> **the entire frame of clouds is not drawn completely in one pass.** Instead, the
> technique of **temporal reprojection** is used that allows cloud samples drawn in
> previous frames to be reused. **[ED]**

This is the same affordability trick both other notes describe, and the numbers
there are the useful part:

- **RDR2** marches **one ray in four, reconstructed over four frames**
  ([`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md) §5.1).
- **UNIGINE** does **interleaved rendering, one pixel in N×N per frame**, plus
  reprojection and an upsample ([`unigine_clouds.md`](../../rendering/unigine_clouds.md) §8).

DCS does not publish its ratio. What it does publish is the **cost of the trick**,
and this is the part the other two notes underweight:

> This technique isn't perfect either as it **introduces a whole class of problems
> and artifacts that only get worse when trying to paint thin, dense fog against
> the ground.** **[ED]**

Temporal reuse is valid exactly to the extent that the thing being reused is
stable between frames. A cloud 20 km away, subtending a few pixels and moving
slowly, reprojects almost perfectly. **Fog thirty metres in front of a moving
aircraft does not** — the sample you are reusing was taken at a materially
different position in a field with far higher spatial frequency, and the error is
a smear that lands in the most visually sensitive part of the screen.

**So the affordability mechanism and the hardest content are in direct
opposition, and it is not a tuning problem.** That is the honest general lesson
about temporal techniques: they buy performance by assuming coherence, and they
fail precisely where coherence fails, which is usually where the action is.

Community reporting is consistent with clouds being the single largest GPU cost
in DCS, and the settings tiers (Low / Standard / High / Ultra) are widely
described as *"Standard and High nearly imperceptible; Low clearly lower; Ultra a
different and finer appearance"* **[COMMUNITY]** — which reads like the tiers
change sample counts and the reconstruction ratio rather than the model. No
measured figures exist publicly. §15.

---

## 6. Fog in the cloud volume — the best engineering content in the paper

The old fog was flat-earth, uniform, unshadowed, non-self-shadowing, and could not
blend with rain or with the new clouds **[ED]**. So it had to move inside the
cloud system. ED's account of why that was hard is the most technically candid
paragraph they have published:

> A seemingly simple task, it was in fact an extremely non-trivial one requiring
> many man years of dedicated work. Drawing the fog in one pass along with the
> clouds while **storing information about the density of the fog in the same
> volume as the cloud data** was highly complex and challenging. This is due to the
> **high data-density required to describe clouds and fog that are not comparable.
> To describe low and dense fog, a very small voxel size is required, but a larger
> voxel is sufficient to describe a small cloud.** …
>
> **Today, we do not know of a single successful implementation of a single
> volumetric cloud system that can draw fog in one pass without a significant
> performance hit.** **[ED]**

Unpacked, this is a clean statement of a resolution conflict inside a shared data
structure:

| | Clouds | Ground fog |
|---|---|---|
| Vertical extent | kilometres | tens of metres |
| Spatial frequency | low | **high** |
| Distance from camera | far | **near** |
| Reprojection validity | good | **poor** (§5) |
| Voxel size wanted | large | **small** |

**Every row disagrees, and the last two disagree in the same direction**, which is
what makes it a genuine conflict rather than a sizing problem: the representation
that makes clouds affordable is the one that makes fog wrong, and the fog is in
the near field where errors are most visible.

**This is the direct counter-argument to RDR2's design, and the two notes should
be read together.** Rockstar solved the same conflict by **not unifying**:
[`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md) is built on **froxel volume near,
raymarch far** — two representations at two frequencies, with a blend between
them. ED chose one volume and spent "many man years" on it.

Both shipped. The trade:

- **Unify (DCS):** one pass, guaranteed-consistent lighting and shading between
  fog and cloud, correct compositing by construction, no blend seam. Cost: an
  extremely hard reconstruction problem, and years of it.
- **Split (RDR2):** each representation sized for its own frequency, each cheap,
  each independently tunable. Cost: two systems, and a blend region that must be
  made invisible.

**The general rule the pair suggests: unify when the two phenomena must shade
identically and composite exactly; split when they differ in spatial frequency by
more than about an order of magnitude.** Fog and cloud fail the second test
badly, which is why ED's version was "many man years" and why they say nobody
else has done it.

What they got, in their own list **[ED]**: fog that is fully integrated with cloud
shading, spherical-Earth-correct, self-shadowing, receiving cloud *and* ground
shadows, blending with the rain volume, any colour, patchy rather than uniform,
animatable, "extremely dense and down to ground level", and **up to 5 km thick
"that can be used to simulate suspended matter in the air"**. Dust is now the same
system.

---

## 7. Volumetric lights and earthshine

> Light scattering is one of the key features of clouds and fog as it creates a
> glowing effect around light sources. This is in tandem with addition of
> volumetric light sources. Fog and clouds are now illuminated by different light
> sources **depending on their density**. Moreover, clouds and fog now **receive
> and scatter light from the Earth**. …the clouds and fog are highlighted by city
> lights. **[ED]**

Two notes:

- **"Depending on their density"** is the correct behaviour and the easy thing to
  get wrong — a light's contribution must be attenuated by the medium it is
  travelling through, not just added at the sample.
- **Light *from the Earth*** is the expensive one. Cloud bases lit by city lights
  requires the ground's emitted/reflected radiance to reach the volume, which is
  a second lighting direction over the whole field. RDR2 does the reciprocal
  thing — [`rdr2_atmospherics.md`](../../rendering/rdr2_atmospherics.md) §6.4, **the volumetrics
  feed the GI via sky irradiance probes** — so between them the coupling runs both
  ways, and neither is cheap.

---

## 8. The content model: sixteen layers of capability, thirty presets of content

This is the part with real shipped data, and it is the most surprising finding in
the note.

**[MIZ]** DCS's static weather exposes the volumetric cloud system through
**exactly 30 named presets and one slider.** The full table, with each preset's
authored layer stack written as a METAR-style string and the legal range of the
one adjustable parameter:

| Preset | UI name | `base` range (m) | Authored layers |
|---|---|---|---|
| Preset1 | Light Scattered 1 | 840 – 4200 | `FEW/SCT 7/8` |
| Preset2 | Light Scattered 2 | 1260 – 2520 | `FEW/SCT 8/10 SCT 23/24` |
| Preset3 | High Scattered 1 | 840 – 2520 | `SCT 8/9 FEW 21` |
| Preset4 | High Scattered 2 | 1260 – 2520 | `SCT 8/10 FEW/SCT 24/26` |
| Preset5 | Scattered 1 | 1260 – 4620 | `SCT 14/17 FEW 27/29 BKN 40` |
| Preset6 | Scattered 2 | 1260 – 4200 | `SCT/BKN 8/10 FEW 40` |
| Preset7 | Scattered 3 | 1680 – 5040 | `BKN 7.5/12 SCT/BKN 21/23 SCT 40` |
| Preset8 | High Scattered 3 | 3780 – 5460 | `SCT/BKN 18/20 FEW 36/38 FEW 40` |
| Preset9 | Scattered 4 | 1680 – 3780 | `BKN 7.5/10 SCT 20/22 FEW 41` |
| Preset10 | Scattered 5 | 1260 – 4200 | `SCT/BKN 18/20 FEW 36/38 FEW 40` |
| Preset11 | Scattered 6 | 2520 – 5460 | `BKN 18/20 BKN 32/33 FEW 41` |
| Preset12 | Scattered 7 | 1680 – 3360 | `BKN 12/14 SCT 22/23 FEW 41` |
| Preset13 | Broken 1 | 1680 – 3360 | `BKN 12/14 BKN 26/28 FEW 41` |
| Preset14 | Broken 2 | 1680 – 3360 | `BKN LYR 7/16 FEW 41` |
| Preset15 | Broken 3 | 840 – 5040 | `SCT/BKN 14/18 BKN 24/27 FEW 40` |
| Preset16 | Broken 4 | 1260 – 4200 | `BKN 14/18 BKN 28/30 FEW 40` |
| Preset17 | Broken 5 | 0 – 2520 | `BKN/OVC LYR 7/13 20/22 32/34` |
| Preset18 | Broken 6 | 0 – 3780 | `BKN/OVC LYR 13/15 25/29 38/41` |
| Preset19 | Broken 7 | 0 – 2940 | `OVC 9/16 BKN/OVC LYR 23/24 31/33` |
| Preset20 | Broken 8 | 0 – 3780 | `BKN/OVC 13/18 BKN 28/30 SCT FEW 38` |
| Preset21 | Overcast 1 | 1260 – 4200 | `BKN/OVC LYR 7/8 17/19` |
| Preset22 | Overcast 2 | 420 – 4200 | `BKN LYR 7/10 17/20` |
| Preset23 | Overcast 3 | 840 – 3360 | `BKN LYR 11/14 18/25 SCT 32/35` |
| Preset24 | Overcast 4 | 420 – 2520 | `BKN/OVC 3/7 17/22 BKN 34` |
| Preset25 | Overcast 5 | 420 – 3360 | `OVC LYR 12/14 22/25 40/42` |
| Preset26 | Overcast 6 | 420 – 2940 | `OVC 9/15 BKN 23/25 SCT 32` |
| Preset27 | Overcast 7 | 420 – 2520 | `OVC 8/15 SCT/BKN 25/26 34/36` |
| RainyPreset1 | Overcast And Rain 1 | 420 – 2940 | `VIS 3-5KM RA OVC 3/15 28/30 FEW 40` |
| RainyPreset2 | Overcast And Rain 2 | 840 – 2520 | `VIS 1-5KM RA BKN/OVC 3/11 SCT 18/29 FEW 40` |
| RainyPreset3 | Overcast And Rain 3 | 840 – 2520 | `VIS 3-5KM RA OVC LYR 6/18 19/21 SCT 34` |

Four readings.

**The numbers read as thousands of feet and the coverage classes are real METAR
octa classes** — FEW, SCT, BKN, OVC. `BKN 12/14` is a broken layer occupying
12,000–14,000 ft. The recurring `FEW 40` / `FEW 41` on most presets is a cirrus
deck at 40–41,000 ft, which is where cirrus actually lives. **The presets were
authored by someone reading weather reports, not by someone moving sliders**, and
that is a defensible content pipeline: the design space is "skies that occur",
not "parameter combinations that are legal".

**Presets use two to four layers. The engine supports sixteen.** Not one shipped
preset comes close to the architectural limit. That gap is worth naming, because
it is a common and expensive shape: the capability was built to a round number,
and the content never needed it. Whether the headroom is for future dynamic
generation or was simply over-built is not established.

**The user gets one continuous knob.** `[MIZ]` The mission file's cloud block,
when a preset is used, is:

```lua
clouds = { base = <the only real variable>, preset = "Preset14",
           density = 0, thickness = 200, iprecptns = 0 }
```

and `pydcs` carries the comment: *"The hard coded values here seem to be the case
for all presets. They are not configurable in the ME and not defined by
clouds.lua."* **`density`, `thickness` and `iprecptns` are vestigial in the preset
path** — they are the *old* parametric cloud model's parameters, still present in
the file format, now inert. The legacy path still exists as the other branch of
the same block, where those three fields are live and there is no preset.

**So the mission format carries both cloud models simultaneously**, discriminated
by whether `preset` is present. That is the same shape as the two weapon APIs and
the two damage models in [`dcs_world.md`](dcs_world.md): **DCS's characteristic
migration strategy is to add the new system beside the old one and leave the old
one loaded.**

**Parameters collapsed into presets, and that is the real cost of going
volumetric.** The old particle system had three orthogonal, physically meaningful
knobs — base, thickness, density — and any combination produced *something*. A
16-layer volumetric field has no such user-facing parameterisation: the honest
parameter set is a coverage map, per-layer type weights, erosion scales and
gradients, and none of those mean anything to a mission designer. **The
representation became more expressive and the authoring interface became less
expressive**, and ED resolved it by hand-authoring thirty good skies. The
white paper's own remaining-work list confirms this is understood as unfinished:
*"development of a dynamic cloud generator"*, *"ability to describe weather fronts
and local cloud formations"*, *"provide a cloud editor interface"* **[ED]**.

---

## 9. Two weather systems that do not meet

**[MIZ]** The mission weather block is:

```lua
atmosphere_type = 0|1,                       -- static | dynamic
wind = { atGround = {...}, at2000 = {...}, at8000 = {...} },
cyclones = { [1] = { centerX, centerZ, pressure_excess, pressure_spread,
                     ellipticity, rotation }, ... },
clouds = { ... },                            -- §8
fog = { thickness, visibility }, enable_fog = bool,
visibility = { distance = 80000 },
enable_dust = bool, dust_density = n,
halo = { preset, crystalsPreset },
qnh = 760, groundTurbulence = n, season = { temperature }
```

There are **two mutually exclusive weather models** behind `atmosphere_type`:

- **Static** — wind specified at three fixed altitudes (ground, 2,000, 8,000),
  and clouds from the §8 preset list.
- **Dynamic** — a **pressure field** built from elliptical cyclones and
  anticyclones (`pressure_excess` in Pascals against 101,325, `pressure_spread` as
  a radius in metres, `ellipticity` a width-to-height ratio, `rotation` in
  radians), from which wind is *derived* rather than specified. **[MIZ]**,
  **[COMMUNITY]**

The dynamic model is the more interesting simulation and the worse renderer
client. Community reference material reports **[COMMUNITY]**:

- dynamic-weather clouds *"will develop at roughly 8–10,000 ft MSL no matter
  what"*, with no known way to move them;
- it works for localised effects (~100 nm) rather than map-wide;
- separate surface / low / high wind velocities cannot be set;
- the editor's preview runs at roughly **125 km resolution** and may disagree with
  what the mission actually produces.

**So the physically-simulated weather system does not drive the 30 hand-authored
volumetric presets.** The pressure field produces its own clouds in a fixed
altitude band. The good cloud rendering and the good weather simulation are in
different modes, and you choose one.

That is the same class of finding as §11 and it has the same cause: **the
volumetric field is not addressable by anything except its own authoring path.**
A pressure solver can say "there should be stratus here, thickening"; there is no
interface that turns that into a coverage field, because the coverage field is
inside a preset.

---

## 10. Network synchronisation — the constraint the other two notes never face

DCS's clouds are **fully network synchronised**: what you see is where the other
player sees it **[ED]**, **[COMMUNITY]**. Neither RDR2 (single-player) nor UNIGINE
(an engine feature) has to solve this, and it is a hard constraint on the design.

It means the density field must be **a pure function of (preset, base, wind,
mission time)** and nothing else — no per-client random seeding, no accumulated
simulation state that could diverge, no dependence on frame rate or on how long a
client has been connected. A client joining a four-hour server must reconstruct
the same sky as everyone else from the mission's weather block and the clock.

The consequences are visible in the design and explain things that otherwise look
like limitations:

- **Clouds advect but do not evolve.** ED's 2.8-era change is that *"volumetric
  clouds now move according to wind direction and speed, with each cloud layer
  respecting wind settings at the set altitude band"* **[COMMUNITY]** — a rigid
  translation of the field per layer, which is trivially a pure function of time.
  Genuine formation and dissipation would be integrated state, and integrated
  state is what you cannot synchronise cheaply.
- **The "dynamic cloud generator" on ED's future-work list is a networking
  problem as much as a rendering one** — anything that grows a cumulus over
  twenty minutes must either be deterministic from the clock or be replicated.

**This is a genuinely useful design rule and it generalises well beyond weather:
if an environmental effect must be identical on every client, express it as a
closed-form function of time rather than as an integrated simulation.** Advection
by a wind field is closed-form. Convection is not. That is why DCS's clouds slide
and do not bloom.

---

## 11. The line-of-sight regression — why this note is in a gameplay directory

This is the most important section, and it is one sentence in ED's paper plus one
line in their to-do list.

The particle system's *first* listed advantage: *"quite easy to scatter them
around the maps and **use this data to calculate line of sight blocking**"*
**[ED]**.

The 2021 system's remaining-work list, in full **[ED]**:

> - Development of a dynamic cloud generator;
> - Support new cloud types;
> - Support additional lightning effects;
> - Improve rendering and lighting quality and optimization;
> - Add the ability to describe weather fronts and local cloud formations;
> - Dynamic cloud simulation;
> - Ability to import real weather data;
> - Provide a cloud editor interface;
> - **AI and visual and sensor line of sight blocking;**

**A capability the 1999 particle system had is on the 2021 system's to-do list.**
For years, DCS's clouds have been things you can hide *behind* visually and not
things the AI or the sensors know about. ED do note that the *new fog and dust*
"can affect the AI and sensor visibility, which was not the case in previous
versions" — so the fog half has been reconnected, and the cloud half has not.

**The cause is structural, not a matter of priorities.** Consider what the two
representations offer a query:

| | Particle clouds | Volumetric field |
|---|---|---|
| What a cloud *is* | an object with a centre and a radius | a density function |
| "Is this segment occluded?" | intersect a segment against N spheres, CPU, microseconds | **integrate the density along the segment** |
| Where the data lives | a CPU list | a GPU-side field, sampled during the march |
| Cost per query | trivial | a march, or a readback |
| Queries needed | one per (unit, target) pair per evaluation | same |
| Rate | simulation rate, thousands per second, on the CPU | — |

The renderer marches the field once per pixel per frame **on the GPU, for one
viewpoint**. The simulation needs to ask *from every unit to every other unit,
on the CPU, at a completely different rate*. There is no cheap way to serve the
second from the first. You cannot read the GPU field back per query (§ the
readback rule in CLAUDE.md's GPU section), and you cannot march a 400 km field
thousands of times a frame on the CPU.

**The general lesson, and the one to carry into cromwell:**

> **Render geometry and query geometry are different assets with different
> budgets, different owners and different rates. A representation change that
> improves the image can silently delete a query the simulation depended on, and
> the deletion is not discovered until someone asks why the AI shoots through a
> thunderstorm.**

This directory has said the same thing three times now from three directions, and
that is enough to treat it as a rule rather than an observation:

- [`ruse.md`](../../strategy/ruse.md) §5 — **three kd-trees per map, split by *purpose*, not by
  contents**: terrain-only, objects-only, camera-only. The objects tree is 556
  triangles, because "forest" is a stat and not geometry. The occlusion triangle
  count is a **budget, not a resolution**.
- [`rainbow_six_siege.md`](../../shooters/rainbow_six_siege.md) §4.9 — *"poking holes degrades
  occlusion efficiency"*: changing the render geometry at runtime damages the
  structure the visibility system relies on.
- **DCS, here** — the render representation got better and the query
  representation ceased to exist.

**If cromwell ever grows volumetric weather that gameplay must respect, the query
structure gets designed first and separately.** The shape is not hard: a coarse
CPU-side occupancy volume — a few hundred metres per cell over the play area,
one byte of opacity — that the simulation owns and queries with a cheap DDA walk,
and which the *renderer's* field is authored or generated to match. It is the same
"summary of authoritative data" pattern CLAUDE.md already describes for
`OcclusionGrid` over `Tile`, with the same escape-hatch discipline. **The mistake
is to build the pretty field first and then go looking for a way to ask it
questions.**

---

## 12. What is *not* in the volumetric system

Two things, and both are informative about where the boundary of a volumetric
system sensibly falls.

**Cirrus is still textured.** Community reporting describes **four cirrus cloud
textures blended together**, which in 2.9 gained motion and stopped fading in and
out **[COMMUNITY]**. Every one of the §8 presets carries a `FEW 40` / `FEW 41`
line, so nearly every DCS sky has a cirrus deck — and it is a textured layer, not
part of the marched volume.

That is the right call and worth stating as a rule. Cirrus is optically thin, at
40,000 ft, effectively at infinity for parallax, and has no interior anyone will
fly through meaningfully. **Marching a volume buys you interior, parallax and
self-shadowing; a phenomenon with none of those should stay a texture.** The
volumetric budget goes where the aircraft actually flies.

**Halo optics are their own preset system.** **[MIZ]**

```lua
halo = { preset = "off" | "auto" | "AtmoHighClouds" | "VolumetricOnly"
                | "HighClouds" | "CirrusOnly",
         crystalsPreset = "AllKinds" | "BasicHaloCircle" | "BasicHaloWithSundogs"
                | "BasicSundogsTangents" | "SundogsArcs" | "Tangents" }
```

Six *media* selectors crossed with six **ice-crystal habit** selectors. Sundogs,
tangent arcs and the 22° halo are different crystal geometries producing different
optical figures, and DCS lets the mission author pick which. That is an
unusually deep piece of atmospheric optics to expose as mission data — and note
that it is exposed **as presets**, exactly like the clouds (§8), for the same
reason: the underlying parameters are not things a user can reason about.

---

## 13. The three-way comparison

| | **DCS World** | **UNIGINE** ([note](../../rendering/unigine_clouds.md)) | **RDR2** ([note](../../rendering/rdr2_atmospherics.md)) |
|---|---|---|---|
| **Source quality for this repo** | white paper + shipped data; **no code** | **real shader source** | **SIGGRAPH talk with formats and ms** |
| Technique | raymarch a continuous field | raymarch a continuous field | **froxel near + raymarch far** |
| World shape | **spherical earth** | three bounded world shapes | local/flat |
| Draw radius | **400 km** | bounded | scene-scale |
| Layers | **up to 16** | single volume, layered shadows | cloud map + height LUT |
| Fog | **inside the cloud volume, one pass** | separate haze term | **separate froxel volume** |
| Affordability | temporal reprojection, ratio unpublished | **interleaved 1-in-N×N + reproject + upsample** | **1 ray in 4 over 4 frames** |
| Self-shadowing | **whole map, horizon to horizon** | fixed cone, local | shadow volume `R16F` |
| Phase function | "physical model", unspecified | **two HG lobes** | **HG stack + floor** |
| Feeds GI? | earthshine *into* clouds | no | **yes — sky irradiance probes** |
| Network-synchronised | **yes** | n/a | n/a |
| Blocks AI/sensor LOS | **no — open work** | n/a | n/a |
| Content model | **30 authored presets + 1 slider** | parametric | artist-authored maps |
| Published cost | **none** | quality presets in numbers | **PS4 ms per pass** |

Three readings.

**One: the technique converged; the constraints did not.** All three march a
density field built from coverage × height gradient eroded by noise, and all
three reconstruct temporally. The differences are all *requirements* — spherical
earth, 400 km, 16 layers, network sync — not *ideas*. **When you find three
independent teams with the same algorithm and different architectures, the
algorithm is settled and the architecture is where your problem actually is.**

**Two: DCS is the only one that had to answer to a simulation, and it is the only
one that lost something.** RDR2's clouds serve a camera. UNIGINE's serve a
licensee. DCS's serve a camera *and* an AI *and* a radar, and the last two got
dropped. That is not a criticism of ED's engineering — it is what happens when
one representation has three consumers with incompatible access patterns, and it
was predictable from the moment "clouds no longer exist as separate objects".

**Three: unify versus split is the live decision, and DCS is the case study for
"unify".** §6 is ED spending many man-years and still saying nobody has solved
it; RDR2 split and shipped. **If this project ever builds participating media,
split by spatial frequency and blend, unless the two phenomena must shade
identically.** The existing plan in
[`source2_rendering.md`](../../valve/source2_rendering.md) §13 already points at the froxel
approach, and this note is a second, independent reason to keep it there.

---

## 14. What transfers, ranked

1. **Design the query structure before the render structure** (§11). The single
   most expensive lesson in this note, confirmed three ways across this
   directory. A coarse CPU occupancy volume the simulation owns, which the render
   field is made to match — not the reverse.
2. **Split participating media by spatial frequency; unify only when they must
   shade identically** (§6). ED's own "we know of no successful implementation"
   is the strongest available evidence for the split.
3. **Anything that must be identical on every client should be a closed-form
   function of time, not an integrated simulation** (§10). Advection yes,
   convection no. Generalises to any replicated environmental effect.
4. **A geometry assumption is a contract between systems** (§4). Flat-earth fog
   and spherical-earth clouds could not be composited at any quality level, and
   the fix was a rewrite. Decide the world's shape once, globally, early.
5. **Erode, don't add** (§3). Start from a filled shape and subtract noise. This
   is the one algorithmic point worth carrying, and
   [`unigine_clouds.md`](../../rendering/unigine_clouds.md) has the real code.
6. **Temporal reuse fails exactly where coherence fails** (§5), which is the near
   field, which is where the player is looking. Budget for that rather than
   discovering it.
7. **Match representation to what the phenomenon actually offers** (§12). Cirrus
   has no interior, no parallax and no meaningful self-shadowing, so it stays a
   texture. Spend the marched volume where the camera goes.
8. **When a system's honest parameters are unusable, ship curated presets** (§8).
   Thirty authored skies described in METAR beat a parameter space nobody can
   navigate — and the tell that this is happening is *vestigial fields left live
   in the file format* (`density`, `thickness` inert in the preset path).
9. **Publish, or at least write down, the post-mortem of the thing you withdrew**
   (§1.2). ED's account of the 2014 system is the most useful page in their paper,
   and it exists because somebody wrote down a failure.

### One anti-pattern

**Building capability to a round number ahead of content** (§8): sixteen layers,
and no shipped preset uses more than four.

---

## 15. What this note does not establish

Substantially more than the two sibling cloud notes, and the gap is the point of
§0.

**The entire implementation.** No noise basis, no texture formats or resolutions,
no sampling schedule or step-size heuristic, no phase function, no transmittance
or multiscatter model, no light-march strategy, no reprojection ratio, no
rejection/validation scheme, no upsampling filter, no data layout for the 16
layers. §3 is reconstruction from the technique family and ED's constraints, and
it is evidence about the *family*, not about DCS.

**Every performance number.** There is no published cost for DCS's clouds — not a
millisecond figure, not a resolution, not a sample count, not what the
Low/Standard/High/Ultra tiers actually change. Every statement here about cost is
either ED's own qualitative wording or community impression, and the comparison
table's DCS column is empty where the other two have numbers.

**How the 400 km self-shadowing is actually done.** §4's cloud-shadow-map
suggestion is reconstruction.

**How network synchronisation is implemented.** §10 derives what the design
*must* satisfy from the fact of synchronisation; ED have not described the
mechanism, and "pure function of mission time" is inference, not a quote.

**The exact `base`-to-layer relationship in the presets** (§8). The METAR strings
read unambiguously as coverage classes and thousands of feet, and `min_base` /
`max_base` are the legal range of the one adjustable parameter — but whether
`base` offsets the whole stack, positions only the lowest layer, or scales the
arrangement is not established.

**Whether DCS's shaders remain readable.** Community shader mods edit plain-text
`.fx` and `.hlsl` under `Bazar/shaders/` (e.g. `enlight/`, and the old
`clouds/cldshad.FX`) **[COMMUNITY]**, so at least parts of the shader tree ship as
source. **I could not establish whether the 2021 volumetric cloud shaders are
among them** — no mirror exists on GitHub and DCS is not installed on this machine
(see [`dcs_world.md`](dcs_world.md) §0). **If the install ever exists locally,
`Bazar/shaders/` is the first place to look, and it would upgrade §3 from
reconstruction to source in an afternoon.**

**The cirrus system** (§12) is entirely community-sourced: four blended textures
is reporting, not documentation.

**Version drift.** The white paper is undated in its text and describes the state
around the 2021 release plus the later fog work. The preset table is current
`pydcs`. Cirrus and wind-driven layer motion arrived in 2.8/2.9. Anything on ED's
remaining-work list may have shipped since — **including, possibly, the LOS
blocking in §11, which is the one item worth re-checking before relying on this
note.**
