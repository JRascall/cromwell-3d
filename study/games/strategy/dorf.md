# D.O.R.F. — pre-rendered sprites as a *rendering* strategy, not a nostalgia one

**Read as of 2026-08-14. The game is unreleased** (developer estimate: late
2027–2028), so a large part of what follows is stated intent rather than shipped
behaviour, and it is tagged as such. What makes it worth a note anyway is that
the developers publish *why* they chose each technique, and the engine they
forked — **OpenRA** — is open source, so the "before" picture can be read
directly instead of guessed at.

The one-sentence version: **D.O.R.F. renders 2D isometric sprites in order to
get cheap dynamic lighting, not in spite of it.** Every unit is a 3D model in
Maya that is baked to four synchronised sprite sheets — diffuse, normal, depth,
and a separate low-resolution depth set used to rebuild a shadow-casting mesh —
so the sprite carries enough surface information to be re-lit per pixel at
runtime. The nostalgia is real and stated, but the *engineering* argument the
lead developer makes is that this buys "dozens (possibly hundreds)" of dynamic
lights at a cost a comparable 3D game would not pay.

## Sourcing

| Tag | Meaning |
|---|---|
| **[DEV]** | A named D.O.R.F. developer's own words — dev blog, Steam developer posts, or the 2025 recorded interview. Dated at every use, because the design churns |
| **[OPENRA]** | Read from OpenRA `bleed` on GitHub, fetched 2026-08-14. This is the **upstream they forked**, so it is the "before" state, not D.O.R.F.'s code |
| **[SHOWN]** | Visible in publicly released footage |
| **[PLANNED]** | Stated intent with no public demonstration yet |
| **[PRESS]** | Third-party coverage |
| **[inferred]** | My reading, flagged so it is not mistaken for a source |

Named people, because attribution matters when the claims conflict across
dates: **John Williams** — lead developer, sole full-time artist, writes the dev
blog and posts on Steam as `DORFdev`. **Thomas van Leth** ("Tommy") and
**Gustas Kažukauskas** — the two engineers. Kažukauskas is `PunkPun` on GitHub,
an OpenRA contributor; Williams says in the 2025 interview that he "took on some
of the OpenRA developers as programmers", which is the unusual fact underneath
everything here — **the fork is being maintained by people who maintain the
upstream**.

Provenance notes for two sources. The 2025 developer interview is a
third-party video (perafilozof, Aug 2025) whose auto-generated captions render
"D.O.R.F." as "Dwarf" throughout; quotes below are de-garbled but otherwise as
transcribed. The monthly Patreon devlogs are the fullest record of the work and
**most of them are patron-only** — the public subset is mirrored to the website
dev blog and to Steam announcements, and that is what is cited here. Several
months of 2024–2025 rendering work (the ones titled *Shadows and Lights*, *3D
Sprites*, *Tilting sprites*, *Interpolation Progress*) are behind the paywall;
their titles are public and are used only as a timeline, never as evidence of
what they contain.

Related: **[`openra.md`](openra.md) is the companion note and should be read with
this one** — the engine D.O.R.F. forked, torn down from source: game loop,
deterministic lockstep, rendering, input and asset loading. §3 below is a short
version of it; that note is the long one, and its §8 lists what reading the
engine changes about the claims here.
[`surface_depth.md`](../../topics/surfaces/surface_depth.md) — §5's depth-sorting trick
is pixel depth offset by another name, and that note explains what writing
`gl_FragDepth` costs. [`lod_systems.md`](../../topics/world/lod_systems.md) — the impostor
section is the same "capture once, sample many times" idea used as an LOD rather
than as the whole art style. [`ruse.md`](ruse.md) and
[`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md) are the contrasting
budgets: a publisher-funded engine and a middleware assembly, against three
people forking an open-source one.

---

## 1. The facts, dated

**[DEV]** [PRESS] D.O.R.F. *Real-Time Strategic Conflict* — DORFteam, Costa
Mesa CA. Post-apocalyptic isometric RTS, three factions (Empire, Warbands,
Collective/N.W.O.), land/air/sea, campaign per faction plus 8-player
multiplayer. Steam app 2388620, announced 2023. Kickstarter ran 17 Mar –
16 Apr 2026 and closed at **$302,806 from 5,370 backers against an $85,000
goal**; one stretch goal paid for an additional engineer. Windows, Linux and
macOS at launch, Steam and GOG.

The team went from three part-timers to two full-time engineers plus hires
during 2026. In the 2025 interview Williams names this as the primary
bottleneck: **[DEV]** *"there were long periods of time where the game was
seemingly in development hell … when the dynamic lighting system was still a
work in progress, I made very few new art assets because I knew that any that I
created would have to be immediately thrown out as soon as the dynamic lighting
system was actually implemented"* (2025-08). That is the real cost of the
technique this note is about, and §4 puts a number on it.

### 1.1 The design churns — read every claim with its date

Two examples, because they set the reliability bar for everything else:

- **Power as a resource.** 2025-08 interview: power *"basically just works like
  it does in C&C"*, one of five resources. 2026-05 blog: **[DEV]** *"Power is no
  longer a resource for any faction but the Collective. Power simply added
  needless complexity … Unfortunately this means those cool Powerplant and wind
  turbine sprites no serve no purpose."* The power plant is the exact asset used
  to illustrate the 2023 lighting post.
- **Logistics.** The forklift/worker/pipeline scheme described in detail in 2025
  was substantially reworked in 2026-05 after it *"resulted in some strange
  gameplay that felt way too busy"*.

**[inferred]** For a rendering study this matters less than it looks — the
render architecture has moved in one direction the whole time (more per-pixel
information, more lights, newer API) while the gameplay systems have thrashed.
But it means the 2025 interview is not a description of the current game.

---

## 2. The engine lineage, and why it is the interesting part

**[DEV]** *"D.O.R.F. is built on a heavily-modified version of the OpenRA
engine, although so many new features have been added, and so many existing
functionalities (such as basic unit attack and pathfinding logic) either have
been or will become so heavily modified that the game engine will bear little
resemblance to its OpenRA origins upon release."* (Steam FAQ, last edited
2026-03-24.)

OpenRA is a C# / .NET reimplementation of the Westwood RTS engines — Red Alert,
Tiberian Dawn, Dune 2000 — with a data-driven trait system and its own OpenGL
renderer. It is **GPLv3**, and the consequence is stated plainly: **[DEV]**
*"the source code itself uses GL[P]-3 licensing"*, and *"we will likely release
the source code in a read-only repo when the game launches"* (2024-01). A
commercial RTS whose engine is legally obliged to be published is rare enough to
be worth watching on its own.

Williams' account of how the project started is the clearest statement of the
build-vs-fork trade in the whole record: **[DEV]** *"I actually have some
concept sketches that I made going as far back as 2010. Although, while I am a
trained 3D artist, I lack the technical knowledge for programming … I remember
actually trying to fuss with the Unity engine, but without even rudimentary
programming knowledge, that turned out to be too much for me. What finally got
me into game development proper was the realization that not only could I take
OpenRA as an engine and make my own original RTS with it, but also realizing
there were software engineers already working on OpenRA who were actually
interested in helping develop for D.O.R.F."* (2025-08.)

**[inferred]** The fork was chosen for the *people*, not the technology. That is
the transferable observation: an open-source engine's contributor list is part
of what you are acquiring, and it is the part that does not show up in a feature
comparison.

The July 2026 blog is the other side of that bargain: **[DEV]** *"fixing a lot
of broken features like netcode, which was previously largely neglected ever
since D.O.R.F. was forked from OpenRA years and years ago"*, and unit deploy
logic *"was actually a weirdly complicated thing to update from how it worked in
OpenRA"*. **A fork ages away from upstream and every unmodified subsystem
becomes a liability on a schedule you did not choose.**

---

## 3. What OpenRA's renderer actually does — the "before" picture

This section is **[OPENRA]** throughout: read from `bleed` on 2026-08-14. It
matters because three of D.O.R.F.'s headline features are *modifications of
mechanisms that already existed upstream*, and the press coverage does not
distinguish those from the ones built new.

### 3.1 One shader, palette lookup in the fragment stage

`glsl/combined.frag` + `combined.vert` do essentially everything. Per-vertex
attributes are packed into a single `uint` bitfield: bits 0–2 select whether the
primary texture is unused / RGBA / a *paletted* sample from one of R,G,B,A; bits
3–5 do the same for a secondary channel; bits 6–8 and 9–11 pick which of **eight
bound samplers** each uses; bits 16–31 carry the palette row.

Paletted sprites are still the default path — a sprite pixel is an index, and
the fragment shader does `texture(Palette, vec2(dot(x, vChannelMask),
vTexPalette))`. There is a hand-written bilinear-in-palette-space filter
(`SamplePalettedBilinear`) and a pixel-art scaling mode that reconstructs
bilinear interpolation in *window* coordinates so magnified sprites stay crisp
without going nearest-neighbour blocky. Team colour is a **palette row swap**,
which is why D.O.R.F.'s magenta accents (§4) are notable — they are a *mask in
an RGBA sprite*, not a palette index.

There is **no lighting in this shader at all**. The only per-fragment colour
manipulation is an HSV `ColorShift` and a vertex tint multiply.

### 3.2 Upstream "lighting" is a CPU-side tint on terrain corners

`TerrainLighting.cs` keeps light sources in a `SpatiallyPartitioned<LightSource>`
with `BinSize = 10` cells, each source a position, range, intensity and RGB
tint. `TerrainSpriteLayer.UpdateTint` samples `TintAt` at the **four corners of
each terrain cell** and writes the result into vertex colours — the comment says
this is *"to smooth out the staircase effect"*. Global tint has `Intensity`,
`HeightStep` and per-channel R/G/B multipliers.

**That is the whole of it.** No normals, no per-pixel evaluation, no shadows,
terrain-first. **[inferred]** So when D.O.R.F. says it built a lighting engine,
it genuinely did — there was nothing to extend.

### 3.3 Depth sprites and `gl_FragDepth` were already there

This is the non-obvious one. `DefaultSpriteSequence` already exposes
`DepthSprite`, `DepthSpriteFrame` and `DepthSpriteOffset` fields, and there is a
`ZRamp` field described as *"Additional sprite depth Z offset to apply as a
function of sprite Y (0: vertical, 1: flat on terrain)"*. The fragment shader
consumes it:

```glsl
float depth = gl_FragCoord.z;
if (length(vDepthMask) > 0.0)
{
    vec4 y = Sample(vDepthSampler, vTexCoord.pq);
    depth = depth + DepthTextureScale * dot(y, vDepthMask);
}
gl_FragDepth = depth;
```

So **per-pixel depth from a paired grayscale sprite is stock OpenRA**, and
D.O.R.F.'s depth-map channel plugs into an existing socket. (`EnableDepthPreview`
even ships a debug visualisation of it.)

But the *practice* is new, and the shipped mod data says how new: `DepthSprite`
appears **88 times across all of OpenRA's mods and every one names the same
file, `isodepth.shp`** — a single generic isometric depth ramp, used only by the
Tiberian Sun mod, which is also the only mod setting `EnableDepthBuffer: True`.
Upstream uses depth to keep units sorting correctly against buildings. D.O.R.F.
authors a real depth map per asset and lets it carry general sorting. See
[`openra.md`](openra.md) §4.3.

**[inferred]** Worth internalising as a technique in its own right: this is
[pixel depth offset](../../topics/surfaces/surface_depth.md) used for *sorting* rather than for
apparent relief, and it converts an ordering problem (which sprite draws on top)
into a per-fragment comparison the hardware already does.

### 3.4 The batcher, and exactly why the fire effects are slow

`SpriteRenderer.DrawSprite` accumulates into one temp vertex buffer and holds up
to eight sheets bound at once. It flushes when it runs out of room, when it
needs a ninth sheet — and here:

```csharp
if (s.BlendMode != currentBlend || vertexCount + 4 > renderer.TempVertexBufferSize)
    Flush();
```

**A blend-mode change ends the batch.** In an isometric game, sprites are
submitted in depth order. Additive fire and screen-blend explosions are
interleaved with alpha-blended units and terrain by position, so a busy fight
alternates blend modes *per sprite* and the batch size collapses towards one
quad per draw call.

That is precisely the symptom Williams reports, without naming the mechanism:
**[DEV]** *"there are some performance issues … especially when lots of
explosions or fire occur onscreen at once. This is largely due to the
limitations of the current system for rendering entities with alternative
blendmodes"* (2026-03), and *"OpenRA's rendering processing system and general
graphics rendering pipeline … are currently not suited to D.O.R.F's larger and
more numerous sprites and thus are huge performance hogs"*.

**[inferred]** The sprite-size half of that compounds the same limit from the
other side: bigger sprites mean fewer per sheet, more sheets in flight, and the
eight-sampler ceiling is hit sooner. Both failure modes are *batching* failures,
which is why a Vulkan port (§7) is the answer being reached for — the fix is
sorting by state and issuing fewer, larger draws, and that is easier to express
in an API with explicit pipeline objects.

### 3.5 The GL floor

`OpenGL.cs`: *"Core features are defined as the shared feature set of GL 3.2 and
(GLES 3 + derivatives, BGRA extensions)"*, with `Modern` and `Embedded`
profiles. Kažukauskas' 2026-04 note says he was *"learning about the extent of
OpenGL 3.3 spec"* before concluding a port was the better use of the time.

---

## 4. The asset pipeline: one model, four synchronised sheets

**[DEV]** *"Most graphics are created by creating 3D models and then rendering
them from an isometric perspective in Maya Autodesk."* (Steam, 2023-06.) Every
unit and building is authored as a 3D model and never shipped as one. Per
asset, per animation frame, per facing, the bake produces:

| Sheet | Content | Used for |
|---|---|---|
| **Diffuse** | Flatly lit albedo. **Magenta accents are a team-colour mask** | The visible image |
| **Normal** | Tangent/world-space normals as RGB — *"cyan = up, magenta = southeast, blue = southwest"* | Per-pixel lighting response |
| **Depth (Z-depth)** | Grayscale distance from camera, lighter = closer | **Depth sorting** primarily; some lighting/shadow use |
| **Micro Z-depth / "shadow sprite"** | A *smaller* set of Z-depth frames from 8 angles | Rebuilding an invisible mesh that casts shadows (§6) |

**[DEV]** *"This is called the Diffuse sprite. This is the most important
sprite, since while it possesses no lighting information, and itself is flatly
lit, it's the most obvious piece of visual information."* (2023-08.)

### 4.1 The facing/frame arithmetic, which is the whole reason it is affordable

Williams works the combinatorics out explicitly for the shadow set, and it is
the most useful paragraph in the public record:

> **[DEV]** *"a mech with 32 facings, with 16 frames per each walk cycle
> animation, would only require 128 Micro Z-depth frames, rather than 4096, as
> it only needs to capture enough data to create the mesh for each frame of the
> animation cycle, but since it is a 3D mesh, it can simply be rotated to
> account for each of the 32 facings. It's for this reason that a tank would
> only need 8 Micro Z-depth frames to render a shadow mesh from; even if the
> tank has 32 or 64 frames of rotation, the shadow mesh can simply be rotated to
> fit all these angles. Of course, if a tank has a turret, that will also need
> its own series of Micro Z-depth frames."* (2023-08.)

**8 angles × 16 frames = 128, versus 32 × 128 = 4096.** The saving comes from
the shadow proxy being *reconstructed geometry*, which can be rotated, while the
diffuse/normal/depth sheets cannot and pay the full facing count. **[inferred]**
The asymmetry is the design: the channels that must be pixel-exact stay 2D and
eat the combinatorial cost; the channel that only needs to be approximately the
right silhouette goes back to 3D and escapes it.

On why sprites at all, Williams gives both reasons and does not pretend the
first is technical: **[DEV]** *"we could have implemented support for 3D models
with textures, but we instead opted for this unique hybrid system of using 3D
sprites since not only is it a lot easier to make lots of assets for very
quickly, but also there's just something kind of magical about that late '90s,
early 2000s pre-rendered isometric look"* (2025-08); and *"one really nice thing
about the 2D nature of the game's artwork is units can be created pretty quickly
and efficiently. So, a lot of units isn't too much of a problem"* — with a
target of ~15–20 base units per faction, roughly doubled by capture/hybrid
units.

He is also correct about the historical lineage, which is worth recording
because it is usually got wrong: **[DEV]** *"Even the original C&C and Red
Alert, which did use some hand-drawn art for things like infantry and map
decorations, still used CG models baked into 2D sprites for their vehicles and
buildings, which is just a more efficient way of creating 32 different
directional facings for each vehicle."*

### 4.2 The re-render, which is what this technique actually costs

**[DEV]** *"it will be an enormous amount of rework to re-render every single
asset in the game so far to account for this new system"* (2023-08), and a year
later, on shipping it: *"It's been quite a while, mostly due to having to throw
out basically the entirety of the game's graphics and replace them with new
sprite sheets that are compatible with the new lighting and shadowing system"*
(YouTube, 2024-08).

**Roughly a year of elapsed time, and the entire art library re-baked.** The
2023 post to the 2024-08 "New Lighting Engine" video brackets it.

**[inferred]** This is the lesson to carry, and it generalises past sprites:
**a baked representation freezes your lighting model into your assets.** Any
change to how surfaces respond to light is a full re-bake, and the pipeline must
be automated enough to survive several of them. The mitigation is to keep the
authored source (here: the Maya scenes) as the truth and the sheets as pure
build output — which is what makes the year survivable rather than fatal.

### 4.3 A pipeline detail worth stealing

**[DEV]** The 2026-04 post lists what "finished" means for a civilian prop:
*"A lot of these models were made ages ago, but were never textured, given
damaged variants, shadow meshes and were never actually implemented in game."*
Four separate completion states per asset — textured, damage variant, shadow
mesh, integrated — is a better asset-tracking schema than "done / not done", and
it is the schema the bake pipeline imposes.

---

## 5. Depth sorting: the problem the whole thing was built to fix

Order the artefacts by how much they bothered the developer, and lighting is
*second*. **[DEV]** *"shadows themselves are just a flat image with no actual
depth data … it can result in artifacting, such as shadows being cast flatly
onto objects in a nonsensical way, or shadows not casting onto objects at all …
There are also all sorts of depth-sorting issues, such as sprites rendering
above or below objects in a nonsensical way (for example a unit entering a
building but then still appearing overlaid on the building rather than
disappearing beneath its roof)."* (2023-08.)

And in 2024-06, on Steam, the ordering is explicit: the new lighting system
*"will fix some of the issues we've been having with the game's depth sorting,
but more interestingly will of course make the game more visually spectacular"*.

The fix: **[DEV]** the depth map *"uses grayscale values to determine distance
from the camera (with lighter = closer and darker = farther), and while it also
has some application with lighting and shadowing, it will primarily be used in
depth-sorting, so that the game engine knows which sprites to render over or
under which other sprites."*

**[inferred]** Read against §3.3, this is: replace the painter's algorithm with a
real Z-buffer whose depths come from the sprite's own bake. A classic isometric
engine sorts *whole sprites* by a scalar (usually the actor's Y or a
hand-tuned Z offset), which cannot express "this unit is behind the building's
front wall but in front of its back wall" — one sprite gets one answer.
Per-pixel depth makes the question local, and the interpenetration cases stop
being special cases. The cost is `gl_FragDepth`: writing it in the fragment
shader disables early-Z for that draw, which on a heavily overdrawn 2D scene is
a real bill (see [`surface_depth.md`](../../topics/surfaces/surface_depth.md) §on pixel depth
offset). For a game whose scenes are mostly quads it is clearly the right trade;
it would not automatically be for a 3D one.

---

## 6. Lighting and shadows

### 6.1 The stated goal is Brigador

**[DEV]** *"It is also partly inspired by the game Brigador, which has an even
more unconventional angle to its visuals in that it is fully dynamically lit
while also being fully 2D. We've gone without publicly announced updates for
almost a year largely because we've been working on trying to get a lighting
system similar to Brigador in place"* (Steam, 2024-06). The connection is
literal as well as aesthetic: **[DEV]** *"the 3D models for the character
portraits were made by J.D. Peters, who is part of the team for Brigador"*
(2025-08).

### 6.2 What the normal map buys

**[DEV]** *"Essentially this uses color information to determine facings … This
is what will be used to give the 2D sprite 3-dimensionality, so that light
hitting the surface will be applied to each surface correctly, rather than
affecting the surface as though it were a flat texture."* (2023-08.)

The economic argument is made outright, and it is the thesis of the whole
project: **[DEV]** *"Another major advantage of this system above just having a
fully 3D game is that this method of lighting/shading is extremely efficient for
creating lots of lights at once. While most 3D games struggle with even a few
light sources in an instance, this method of lighting/shading means maps could
potentially have dozens (possibly hundreds) of light sources."*

**[inferred]** The claim is directionally right for a specific reason worth
stating precisely: a sprite scene has **no geometry cost per light** — no shadow
map render, no extra vertex work, no draw-call multiplication. Cost is
per-covered-pixel only, at a fixed and modest orthographic resolution with no
LOD or occlusion to manage. It is essentially a G-buffer with the expensive half
(rasterising geometry) precomputed at bake time. The "most 3D games struggle
with even a few light sources" framing is a decade out of date for
clustered/tiled renderers, but the underlying asymmetry — **lights are cheap
when geometry is free** — holds.

### 6.3 What shipped in the trailer, 2026-03

**[SHOWN]** [DEV] Landed just before the Kickstarter trailer, credited to van
Leth:

- **Spotlights** — *"distinct from the lights we've shown before (point lights),
  since they focus light in an aperture rather than casting it radially"*, with
  controllable aperture and falloff radius. Vehicle headlights, searchlights.
- **Volumetric lighting** — visible shafts, *"intensity of volumetrics can also
  be determined on a per-light basis, so in some cases, volumetrics are reduced
  or turned off entirely in instances where it doesn't make sense … such as
  fires, where the light source is actually far above the fire itself"*. Also
  applied subtly to explosions and tracers as a bloom-like effect.
- **Haze and fog, per map, with a height value** — *"Fog can also have a height
  value applied, and so can be made to be thicker at lower or higher
  elevations"*; more fog directly means starker volumetrics.

**[inferred]** The per-light volumetric intensity knob is the detail that says
this was built by people shipping a game rather than a demo — it exists because
the physically-consistent answer looked wrong for fire, and an artist needed an
override.

### 6.4 The two admitted gaps

**[DEV]** (2026-03, verbatim): *"You may have noticed that objects do not cast
shadows from smaller light sources, and that shadows are only cast from the sun
position. This will be altered in a future update. You may have also noticed in
the trailer that many vehicles only have a single headlight. This is currently
an optimization issue, as there is a limitation of **100 light sources per map**
right now. This will be changed in a future update to allow for theoretically
infinite light sources, through the implementation of **Forward+ rendering** …
This of course will be a large project in of itself, and will require a lot of
man-hours to replace the existing rendering architecture."*

**[inferred]** A hard cap of 100 with no spatial culling is the signature of a
fixed-size uniform array iterated per fragment — every light evaluated against
every lit pixel. That the fix named is Forward+ (cluster the lights, each
fragment reads only its cluster's list) confirms it. It is the same conclusion
this project reached from the 3D side — see
[`re_engine_rendering.md`](../rendering/re_engine_rendering.md) §on clustered light
culling — and it is mildly reassuring that a 2D renderer and a 3D one converge
on the same answer, because the technique is about *light–pixel assignment*,
which does not care whether the pixels came from geometry or from a quad.

**Two headlights per vehicle instead of one is the kind of budget symptom worth
remembering:** the cap did not show up as a frame-rate problem, it showed up as
an *art* compromise that shipped in a marketing trailer.

### 6.5 The shadow-mesh scheme, and its honest doubts

The most speculative part of the design, and the developer says so:

> **[DEV]** *"Using a series of Z-depth frames rendered from specific angles, the
> game will take this Z-depth information and use it to create a 3D mesh on the
> fly. This mesh will be invisible in game, but will function as a means from
> which 2D sprites will cast functional, dynamic shadows onto objects from light
> sources. … this system is largely experimental, and it's not totally clear yet
> how this will actually function in game; there may be cases where the shadow
> mesh generated does not quite match the intended shape of the object, and
> there may be cases where the shadow mesh incorrectly envelopes the object it is
> supposed to be casting from."* (2023-08.)

Status as of 2026-08: shadows exist and are **sun-only** (§6.4). Whether the
reconstruction is the shipping mechanism is not publicly established — assets
are described as being given "shadow meshes" as an authoring step in 2026-04,
which reads like a per-asset pipeline product rather than a runtime
reconstruction. **[inferred]** The obvious question the public record does not
answer is why 8 depth captures are reconstructed into a mesh at runtime instead
of a proxy mesh being decimated from the source Maya model at bake time, which
would be exact, cheaper, and already available. Possibly because the shadow
proxy must match the *sprite* — including whatever the bake camera's projection
did to it — rather than the model. That is a genuinely good reason if so, and it
is the sort of thing only a re-implementation would settle.

---

## 7. The performance arc, 2026

A clean sequence of dated decisions, useful as a case study in what an indie
team actually does when a renderer stops keeping up:

| When | Move |
|---|---|
| 2026-03 | Diagnosis: blend-mode-heavy effects (fire, explosions) are the cost; OpenRA's sprite pipeline is unsuited to larger/more numerous sprites |
| 2026-04 | Tried **instanced rendering** within GL 3.3; then *"investigating how easily we could transition to Vulkan. Then getting it running in a week"* |
| 2026-04 | Settled on **Vulkan 1.2** — *"it has basically everything we need … Blender seems to be going with 1.2 … For final stretches we'll probably need to add support for older API's. It seems standard for engines to use like 4 graphics API's"* |
| 2026-05 | *"replacing the game's graphical API with Vulkan, which, while it would not provide immediate performance improvements, will allow us to implement optimizations once the entire framework is in place"* |
| 2026-07 | *"continuing to update the game's rendering engine and optimization work"*; a video caveat notes *"the glitchy effects on the barrel are a consequence of the ongoing refactoring of the render engine"* |
| **[PLANNED]** | Forward+ to remove the 100-light cap |

All quotes **[DEV]**; the Vulkan ones are Kažukauskas via the 2026-04 blog.

Two things to take from it. **[inferred]** First, the honesty in the 2026-05
line is unusual and correct: an API port is not an optimisation, it is
*permission* to optimise, and teams that expect frames back from the port itself
are usually disappointed. Second, "a week to get Vulkan running" against
"several months and still refactoring" is the normal ratio — the port is not the
work, re-expressing the renderer's state management is.

---

## 8. "3D sprites" — the hybrid, and where the illusion is spent

Sprite-based games usually spend their remaining budget on *more frames*.
D.O.R.F. spends it on making the sprite behave like the model it came from. The
public evidence:

- **[SHOWN]** *"D.O.R.F. — 3D Sprites"* (2025-06): *"Ever wished you could have
  sprites that could move like 3D models, without actually needing to be boring
  old 3D models?"* The demonstrated behaviour is sprites **tilting to terrain
  slope**. Patreon devlog titles corroborate the sequence: *Interpolation
  Progress* (2024-12), *Tilting sprites, light cast from gunfire* (2025-05).
- **[DEV]** In the comments of that video, on the visible clipping: *"The
  clipping is more a result of the way the game handles pathing; all vehicles
  only count as occupying a single terrain tile, so for larger units, this makes
  them appear to tilt really fast and unnaturally when on slopes. We'll change
  this to allow for multi-tile unit pathing."*
- **[SHOWN]** *"Physics-Based Movement"* (2026-01): *"We've completely
  overhauled unit movement, using a physics-based system."*
- **[DEV]** (2025-12) *"We've even updated the game to include acceleration and
  deceleration for units, as well as movement that's unbound by the map grid"* —
  both absent from stock OpenRA, which the same post notes has *"no acceleration
  or deceleration logic for ground units (units will move to their top speed
  instantaneously, and stop instantaneously)"*.

**[inferred]** The interesting failure in that comment is a **coupling bug, not
a rendering bug**: the sprite's tilt is driven by the terrain under its *single
occupied cell*, so a long vehicle reads one sample and snaps. The fix is in the
pathfinding representation, not the shader. This is the recurring shape of the
whole project — the visual upgrade keeps exposing that the simulation underneath
is still Westwood-era one-cell-per-unit, and each fix is a simulation fix.

**[inferred]** The economics of the hybrid are worth stating plainly, because
they are the reason to care. A pre-rendered sprite normally loses on: arbitrary
camera angles, arbitrary rotation, per-instance deformation, and lighting.
D.O.R.F. buys back *rotation smoothness* (interpolation), *orientation to
terrain* (tilt), and *lighting* (normals + depth) — and simply does not need the
first, because the camera is fixed isometric. **The style survives because the
camera constraint is real.** Loosen it — the requested zoom-out is already on the
list — and the bake's fixed resolution becomes the limit. Williams says as much:
**[DEV]** *"That's mostly a flaw of the current camera not really being designed
around these giant sprites. We still need to change the camera parameters to be
able to zoom out twice as far."*

---

## 9. What the fork had to change in the simulation

Not rendering, but the cost of the fork, and the part with the most direct
lessons for an RTS:

- **Multiple weapons and turrets with limited traverse.** **[DEV]** *"Thomas
  completely reworked OpenRA's simple vehicle attack code a while back, so we can
  now have vehicles that have multiple guns and turrets with limited traverse
  that can independently target separate units."* (2025-08.) The 2025-12 post
  gives the payoff — a Destroyer with *"three double-barreled 5cm autocannon
  turrets, two side-mounted 5cm autocannons, a torpedo launcher, an anti-missile
  turret"*.
- **Large collision footprints.** Stock C&C-lineage engines make every unit one
  cell regardless of sprite size; §8's tilting bug and the naval design both
  trace to it.
- **Blocked line of fire.** **[DEV]** (2026-04) *"unit gunnery can be blocked by
  friendly units in the way, and blocked units will simply not open fire"*,
  following Beyond All Reason, deliberately as an anti-deathball measure, paired
  with a Total War-style formation tool so it is not micromanagement hell.
- **Indirect fire** — barrels elevate for parabolic arcs.
- **Terrain deformation** — cratering warheads restricted to heavy artillery and
  superweapons *"as that would be rather annoying if terrain was constantly
  being warped by any little explosion"*; **[PLANNED]** worker levelling,
  trenches.
- **Water as a simulation, not tiles.** **[PLANNED]** **[DEV]** *"water levels
  are dynamic, and water can be drained from a reservoir if its surrounding
  ground barrier is destroyed, and will dynamically flow from terrain tile to
  terrain tile … a lot of this work has already been done for us. We can
  essentially modify the flow simulation logic we already have in place for the
  game's pipeline structures."* (2025-12.)

**[inferred]** That last one is the best systems-design move in the record:
**the resource-pipeline flow solver and the water solver are the same solver.**
The feature was affordable because an unrelated gameplay system had already paid
for it. Worth remembering as a prompt — when a new system looks too expensive,
check whether something already in the build is the same maths.

The 2025-12 post is also a well-argued piece of engine criticism in its own
right: it dissects why naval combat is bad in two C&C-lineage mods (Dawn of the
Tiberium Age, Combined Arms) and attributes every failure to a specific engine
limitation — one-cell collision forcing car-sized warships, no per-turret weapon
binding, no traverse limits, instant acceleration plus fire-on-the-move making
fast boats dominant. **[inferred]** It is the clearest example I have read of
*content design being dictated by engine constraints nobody chose*, which is
exactly the argument for owning the engine.

---

## 10. What transfers, and what does not

**[inferred]** throughout. Judged against cromwell's three target genres.

**Take:**

1. **Per-pixel depth from a paired depth texture, as a sorting mechanism.**
   Directly applicable to decals, billboards, particle-card impostors and
   anything else flat that must interpenetrate real geometry. The mechanism is
   already understood here as pixel depth offset; what D.O.R.F. adds is the idea
   of applying it *universally* and letting the Z-buffer replace a sort. Note the
   early-Z cost and keep it off the main opaque pass.
2. **Bake the expensive half of the G-buffer.** Impostors that carry normal +
   depth alongside albedo can be lit by the *live* light rig instead of being
   frozen with baked lighting — the standard failure of impostor LODs. This is
   the same idea as [`lod_systems.md`](../../topics/world/lod_systems.md)'s impostor section, with
   the lighting fix that section's subjects mostly don't apply.
3. **The blend-mode batching lesson, which is engine-agnostic.** Sorting by
   depth and sorting by state are in direct conflict, and additive effects
   interleaved by depth destroy batching. Applies unchanged to a 3D forward
   renderer's transparent pass. Worth a profiler zone that counts *state
   changes*, not just time.
4. **The completion schema for baked assets** (§4.3) — textured / damage variant
   / proxy mesh / integrated. Any pipeline that derives several artefacts from
   one source needs per-artefact status, not per-asset.
5. **Forward+ as the answer to a hard light cap**, arrived at independently from
   the 2D side. Reinforces the existing plan rather than changing it.
6. **The "same solver twice" habit** (§9, water = pipeline flow).

**Do not take:**

- **The pre-render itself.** It is a one-way door for a free-camera engine, and
  §4.2's cost — a year, and every asset re-baked — is what a lighting-model
  change costs once assets are frozen. Its virtues (fast asset creation, cheap
  lights) are bought with the camera constraint, which cromwell's genres do not
  accept.
- **The shadow-mesh-from-depth-frames reconstruction** (§6.5), which is
  unresolved by its own author's account and has an obvious cheaper alternative
  when the source model is in hand.
- **The palette/team-colour machinery**, which is a Westwood-compatibility
  artefact. D.O.R.F. itself sidesteps it with a magenta mask channel; a mask is
  the right answer.

---

## 11. What is not public

Honest gaps, so this note is not read as more complete than it is:

- **No code.** GPLv3 obliges publication, but the stated plan is a read-only
  repo **at launch**. Everything about D.O.R.F.'s own renderer is developer
  description; only the OpenRA baseline in §3 was read.
- **No build.** No demo or playtest has been released; a playtest is promised
  before launch and a demo was hoped for "end of the year" as a Kickstarter
  reward (2026-05).
- **No numbers.** No frame times, sprite counts, sheet sizes, resolutions,
  memory figures or unit counts have been published. The only quantities in the
  whole record are the 100-light cap and the 8/32/128/4096 facing arithmetic.
- **The 2024–2025 rendering devlogs are patron-only** — the months that would
  say how the lighting system was actually built.
- **Unanswered:** whether lighting is deferred, forward-with-array or something
  else today (§6.4 infers the second from the cap); what the depth encoding's
  precision and world-space range are; whether normals are baked in world or
  view space; how animation frames are packed; whether Vulkan brought a
  different sprite-batching architecture or a port of the old one.

The thing to watch: **the source drop at launch.** It will be the rare case
where a shipped commercial renderer built on these techniques can be read
line-by-line, and this note should be rewritten from it rather than extended.

---

## 12. Sources

**Primary — developer:**

- Dev blog, dorf-rts.com: *Creating Lighting in a 2D Game* (2023-08-12);
  *End of March Update* (2026-03-31); *End of April Update — Optimizations*
  (2026-04-30); *End of May Update* (2026-05-31); *End of July Update — New
  Faces* (2026-07-31).
- Steam announcements for app 2388620, retrieved via the ISteamNews API —
  notably *Fun On The Water* (2025-12-23), which is not on the website.
- Steam Community, developer account `DORFdev`: the pinned *D.O.R.F. FAQ*
  (posted 2023-08-06, last edited 2026-03-24); *Game engine?* thread
  (2023-06-13); *The genius of this game* thread (2024-06-08).
- YouTube `@dorf-rts` video descriptions and developer comment replies —
  *New Lighting Engine* (2024-08-25), *3D Sprites* (2025-06-28), *Naval Warfare*
  (2025-12-01), *Physics-Based Movement* (2026-01-31).
- Recorded developer interview, perafilozof, *"Developer Q&A 2025"*
  (2025-08); auto-generated captions.
- Presskit and Kickstarter campaign page (funded 2026-04-16).

**Primary — upstream code:** OpenRA `bleed`, github.com/OpenRA/OpenRA, fetched
2026-08-14 — `glsl/combined.{vert,frag}`, `OpenRA.Game/Graphics/SpriteRenderer.cs`,
`OpenRA.Game/Renderer.cs`, `OpenRA.Game/Graphics/TerrainSpriteLayer.cs`,
`OpenRA.Mods.Common/Graphics/DefaultSpriteSequence.cs`,
`OpenRA.Mods.Common/Traits/World/TerrainLighting.cs`,
`OpenRA.Platforms.Default/OpenGL.cs`.

**Secondary:** GamingOnLinux (2026-03-20); Rock Paper Shotgun (2023-06-14);
github.com/PunkPun (identifying Kažukauskas as an OpenRA contributor).
