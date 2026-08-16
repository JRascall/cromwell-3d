# Portal 2 — the gels

How Valve painted a level. The gels are the best-documented part of Portal 2
because one of their authors gave a GDC talk about the textures, another
recorded a commentary node about the renderer, and the tuning constants are all
still exposed as console variables in the shipping game.

The one-line thesis: **a gel is two completely separate systems that share a
colour.** In the air it is an *isosurface* — a marching-cubes mesh regenerated
every frame from a few hundred moving spheres. On a surface it is a *paint map* —
a per-texel channel painted into the level's lightmap parameterisation, read by
every world shader, and by the movement code. Neither one is a particle system,
and neither one is a decal, and §3 is the argument for why not.

Rendering companions: [`portal2_portal_rendering.md`](portal2_portal_rendering.md),
[`../source2_particles.md`](../source2_particles.md).

---

## 0. Provenance

| Tag | What it is |
|---|---|
| **[GRIMES]** | Bronwen Grimes, *"Making and Using Non-Standard Textures: Manipulating UVs through Color Data in Portal 2"*, GDC 2011 — Valve's published slides **with speaker notes**, which is where all the detail lives. |
| **[VALVE-COMMENTARY]** | Portal 2 in-game developer commentary, transcribed. |
| **[VDC]** | Valve Developer Community — Portal 2 entity keyvalues and console variables, names and defaults from the shipping binaries. |
| **[VLACHOS]** | Alex Vlachos, *"Water Flow in Portal 2"*, SIGGRAPH 2010. |
| **[P2-RETAIL]** | The shipping game at `E:\SteamLibrary\steamapps\common\Portal 2` — materials from `pak01_dir.vpk`, and RTTI class names, cvar names and shader parameters mined from `bin\stdshader_dx9.dll` and `portal2\bin\{client,server}.dll`. |
| **[inferred]** | Our reading. |

No Portal 2 source code exists publicly, and unlike the portal renderer there is
no Portal 1 ancestor to read — the gels are new in 2011. **Every claim in this
note is therefore Portal 2's own**: two published Valve talks, the developers'
recorded commentary, the shipped `bin\portal2.fgd`, the entity setups inside
Valve's own shipped `.bsp` maps, ~140 cvars, the materials, and the C++ type
names still present in the retail binaries. §10 is the raw evidence list; §4.0
and §6 carry the parts that changed the picture.

---

## 1. Where it came from

**[VALVE-COMMENTARY]**, Brett English:

> Similar to the way the student game Narbacular Drop became the original Portal,
> the paint mechanics in Portal 2 come from a student game, **Tag: The Power of
> Paint**.

Valve hired the DigiPen team. **[GRIMES]** puts it plainly: *"We enjoyed the game
so much that we hired the creators to implement the mechanic in Portal 2. Though
the mechanic was a great fit for the Portal universe, the fiction and the visuals
still needed integration."*

What changed on the way in is a masterclass in porting a mechanic rather than
copying it. From the commentary:

- **Repulsion gel** (blue). Tag's bounce paint *"always bounced the player at a
  fixed height and activated whenever the player touched the paint"*. Portal 2's
  **reflects the player's velocity** — you bounce back to the height you fell
  from — *"because players have to think about gaining and preserving height"*,
  which is the same currency portals already trade in. And it only triggers if
  you **jump off or fall onto** it, because *"players would often accidentally
  walk onto repulsion gel they didn't see and then trigger an unwanted bounce.
  This was a particularly nasty problem in Portal 2 because the player couldn't
  get rid of paint once it was applied; whereas in Tag the player can erase paint
  at any time."*
- **Propulsion gel** (orange) *"carried over almost exactly the same as the speed
  paint in the student game, except that it is slightly more than twice as fast"*,
  plus *"a slight funneling effect to help guide the player when they are speeding
  into a portal"*.
- **Conversion gel** (white/grey) was last, and the finding was about *delivery*:
  *"it was most effective when it was deployed in large quantities"* and *"dropping
  the paint at an angle allowed the player to play more freely"*.
- **The paint gun was cut.** Ted Rivera: freely painting an indoor puzzle space
  *"changed the game's pacing significantly"*, and *"there's a certain elegance in
  the simplicity of manipulating all the game elements using only the portal
  gun"*. Instead: droppers and tubes, and **you aim the paint by aiming a portal**.

That last decision is the one to steal. **They removed the player's direct verb
and re-expressed the mechanic through the verb the game already had.** A gel
stream is a thing you redirect with portals, so gel puzzles are automatically
portal puzzles, and the tutorial cost of the whole feature is zero.

## 2. The types, and what survived

The enum is in Valve's shipped `bin\portal2.fgd`, and it is worth quoting exactly
because the community documentation renames one of them: **`0 : Bounce`,
`1 : Stick`, `2 : Speed`, `3 : Portal`, `4 : Erase`**. **[P2-RETAIL]** Slot 1 is
*Stick* — adhesion — in the shipped tool file; the reflection gel that later
occupied that slot never got its name back. `prop_paint_bomb` defaults to
`PaintType 4` (erase) and `info_paint_sprayer` to `0` (bounce).

| Gel | Colour cvar (default) | Effect | Status |
|---|---|---|---|
| Repulsion | `bounce_paint_color 0 165 255 255` | bounce | shipped |
| Propulsion | `speed_paint_color 255 165 0 255` | low friction, high speed | shipped |
| Conversion | `portal_paint_color 128 128 128 255` | makes any surface portalable | shipped |
| Cleansing | — (`erase_visual_color`) | erases other gels | shipped |
| Reflection | — | reflect lasers | **unfinished**; missing `blob_surface_stick` texture and particle systems |
| Adhesion | — | stick to surfaces | **cut**; removed entirely by DLC1 |

Conversion gel shipped grey rather than white, and the VDC notes the community
argument about whether that was an accident — *"others argue it may have been
deliberately done so that players could more easily distinguish it from regular
portal surfaces"* — which, whatever the history, is exactly the constraint §3
turns out to be built around.

## 3. The constraint that shaped everything

**[GRIMES]**, on requirements:

> Performance-wise, we can't procedurally generate meshes; we have decals in the
> Source engine, **but they are short-lived to avoid growing memory use beyond
> hardware limits; gameplay can't depend on them**. We wanted to create something
> as a material effect only, using tiling textures whenever possible, so that the
> memory footprint would be small.

Read that as a rule: **an effect that gameplay depends on may not live in a system
that is allowed to forget.** Source's decals are a fixed-size recycling pool.
Paint is a gameplay surface property that must persist for the whole level. So
paint could not be decals, and that single sentence is why the rest of this note
exists.

Tag could paint directly into diffuse textures because it used a **unique
parameterisation** for its whole world. Portal 2 could not: *"Portal 2 is a more
realistic environment, with several large levels. We could not afford to give up
tiling textures."*

## 4. The paintmap

### 4.0 What it actually is, from the engine

Before the talk's account, the mechanism, read out of `bin\engine.dll` and out of
Valve's own shipped maps. **[P2-RETAIL]**

**It is not in the BSP.** `sp_a3_bomb_flings.bsp` is `VBSP` version 21 and has no
paint lump — the lump table stops at 61 and every used lump is a standard one.
What the map carries is a single worldspawn key:

```
"paintinmap" "1"
```

on `mp_coop_paint_bridge` and friends. **A shipped Portal 2 level contains no
paint at all; it contains a flag saying "allocate paintmaps for this level".**
Everything else is runtime state.

**The engine owns it.** `engine.dll` has `CPaintmapDataManager` behind an
`IPaintmapDataManager` interface, `R_CheckForPaintmapChanges` (a per-frame dirty
check), `r_redownloadallpaintmaps` (re-upload every paintmap to the GPU), and
`mat_paint_enabled`. The alpha semantics are exposed too —
`paint_min_valid_alpha_value`, `paint_max_surface_border_alpha`,
`paint_alpha_offset_enabled`, `debug_paint_alpha` — so a paintmap texel is
**a paint type plus an alpha**, the alpha being coverage, and the border alpha
being how the splat's edge is feathered. The failure message names the invariant:

> `MEMORY LEAK: adding surface paint powers in a level with no paintmaps.`

**Paint powers hang off surfaces via the paintmap**, and a level without
paintmaps has nowhere to put them. That is §3's "gameplay can't depend on
decals" enforced by an assert.

**And it is networked as its own message.** `engine.dll` carries `SVC_PaintmapData`
/ `svc_PaintmapData` — a dedicated wire message type alongside the usual Source
SVC messages. That is how a co-op partner sees your paint, and how a client
joining late gets a room that is already painted. Compare the paint *splats*,
which are a temp entity (`DT_TEWallPaintedEvent` in `client.dll`): **the durable
state goes over a bulk message, the momentary effect goes over a TE.** Two
mechanisms, one for what must be true and one for what must be seen.

So the full chain is: map flag → engine allocates paintmaps sized from the
lightmap parameterisation → blobs land and write texels → dirty check → re-upload
to the GPU → the world shader samples it (§5) and the movement code samples it in
a sphere (below) → and the whole thing replicates over `svc_PaintmapData` and
saves to disk via `CPaintSaveRestoreBlockHandler`.

### 4.1 Why the lightmap parameterisation

The answer is to borrow a unique parameterisation the engine already has.
**[GRIMES]**:

> We use **the same coordinates as our lightmaps**, which allows us to use our own
> unique parameterization. It's much lower-resolution than Tag's implementation,
> however. This solution was preferred because **it was easy to understand its
> impact; we knew already how much lightmaps cost, so were able to predict the
> impact of doubling the required memory.** We were able to adjust our texture
> budgets accordingly. It was also existing tech, which meant much of the work was
> already done.

Three separate arguments in one paragraph, and the *budget* one is the
interesting one: they chose the representation whose cost they could already
price. Not the best-looking option, the *predictable* one.

The consequences leak all the way out to level design **[VDC]**:

- **A map must have real lighting compiled, not just a VRAD run, or gel splats do
  not render at all.** The paint map is generated from the lightmap.
- **Lightmap scale sets gel resolution.** *"Gel seems to work best with the scale
  set to 16. Lower numbers may cause lag spikes, and higher can result in their
  textures and areas of effect looking odd."*
- **Displacements cannot be painted.** `%nopaint` marks a material unpaintable.
- Related cvars: `mat_paint_enabled` (gel on brush geometry),
  `r_hidepaintedsurfaces` (*"the effects of gel will still apply"* — proof the
  paint map and the physical effect are the same data read by two consumers),
  `paintsplat_noise_enabled` / `paintsplat_max_alpha_noise 0.1` (random alpha per
  splat, *"very subtle and usually not noticed"*), and `removeallpaint`.

And on the gameplay side, a sphere radius around the player within which painted
texels count — VDC documents it as `sv_paint_detection_sphere_radius 16`, and the
name in the retail binary is **`sv_paint_surface_sphere_radius`** **[P2-RETAIL]**;
setting it to 8 or below *"disables conversion gel"*, 0 disables all gel. **The
movement code samples the paint map in a sphere**, it does not ask "what am I
standing on". It also samples *ahead*: `paint_power_look_ahead_sample_density` is
in the same binary, so the paint power the player is about to be standing on is
resolved before they get there.

### Iterations, and the one that was rejected for a gameplay reason

**[GRIMES]** shows three:

1. **Raw paintmap** — the low-res unique-parameterised application, visibly
   blocky. *"Obviously, the visuals are not ideal."*
2. **Tiling normal map with an alpha channel**, alpha combined with the
   paintmap's and thresholded, giving splatter edges at texture resolution rather
   than lightmap resolution. Better — and rejected:

> What's more, it obscured the surfaces beneath, which caused gameplay problems.
> In Portal 2, surfaces either accept portals, or they don't. You can distinguish
> them visually. The gels do not interfere with your ability to place portals…
> **It doesn't matter if the gel does not physically block portal placement if you
> can no longer determine whether the underlying surface accepts them; you'll be
> stuck just the same.** Consequently our next implementation needed to be more
> translucent.

This is the most transferable sentence in the whole talk and it is not about
rendering. **An overlay that hides an affordance is as bad as one that removes
it.** Our equivalents are obvious the moment you look for them: fog of war over
cover indicators, damage decals over a destructible's material, a selection glow
over a hazard tile.

3. **Translucent elastomer with embedded bubbles** — the shipped look. The
   reasoning is fiction-driven and worth preserving because it is how an art
   requirement gets turned into a shader requirement: *"what's more science-y than
   plastics?"* → translucent thermoplastic elastomers and silicone rubber → and
   for elasticity, super-bouncy balls → *"how about embedding some in the paint,
   creating a bunch of reverse balls: air in an elastomer? Bubbles would be great
   for communicating thickness, too, helping sell the idea that the gel is applied
   thickly enough to change the surface properties significantly."*

## 5. Surface-embedded sprites

The bubbles have to look round from every angle — i.e. they must behave like
sprites — but sprite cards were out (thousands of camera-facing quads) and decals
were out (§3). So: **sprite behaviour with no sprite geometry, entirely in the
pixel shader.** **[GRIMES]**

The mechanism is a *UV layout map*: a texture that does not perturb UVs but
**replaces** them.

> The previous technique (flow maps) used vectors read from a texture to displace
> existing UVs. In this technique, we're going to flat-out replace them. This
> allows us to control, through a texture, the UV layout on our surface.

Each "island" in the layout map is a porthole onto the bubble texture. Then the
contents of every island are rotated to face the camera, **in tangent space**:

- `v_s` = camera side vector; `v_f` = **the vector from the view position to
  *this pixel***, not the camera forward — *"so that it varies over the surface"*.
- `T` = world-to-tangent. Build a matrix from those, fill the missing entry with a
  cross product, orthonormalise (*"incidentally also forcing the view side vector
  to vary over the surface"*).
- **Truncate it.** *"M'' is M' truncated: we don't need any z-entries or anything
  that would affect the z-entries: we're working with two-component texture
  coordinates. It's important to keep M'' small by removing any entries we don't
  need: we're passing it from the vertex shader to the pixel shader, and
  interpolators aren't free!"*
- Pre-transform the UVs so the island centre is `[0,0]`, apply by dot product,
  post-transform back to `[0.5,0.5]` — *"so the sprites will appear to be pinned at
  their centers instead of their top left corners"*.

Failure mode and mitigation, stated honestly: the "sprite" clips against the edge
of its island *"as though you are looking through a porthole"*, so the texture is
padded, faded out at grazing angles, and masked with soft edges.

The layout maps themselves are cheap to author — *"unlike the flow maps, UV layout
textures are not difficult to hand author"* — and Valve's were **generated in
Processing using randomised placement plus the Apollonian gasket fractal** to get
tiling layouts of mixed bubble sizes.

In the final shader the layout tiles over the surface, looks up the bubble
texture, and drives *normals, reflections and opacity*, with the lookup further
offset by refraction *"so that the bubbles sit better inside the surface"*.

**All of it shipped, and it is visible in the install.** `pak01_dir.vpk` contains
`materials/paint/bubble.vtf` and — the one that proves the technique —
**`materials/paint/bubblelayout.vtf`**, the UV layout map itself, beside
`splatnormal_default.vtf` and `paint_envmap_hdr.vtf`. The world shader in
`stdshader_dx9.dll` carries the matching parameters
**`$PAINTSPLATBUBBLE`, `$PAINTSPLATBUBBLELAYOUT`, `$PAINTSPLATNORMALMAP`** —
*"the paint splat normal map to use when paint is enabled on the surface"* — and
the combos `PAINT`, `PAINTREFRACT` and `THICKPAINT`. **[P2-RETAIL]** So gel on a
wall is not a separate material at all: **it is three extra texture parameters and
a static combo on the ordinary world shader**, which is why §4's "one more
channel on the lightmap" framing is the right one and why the shader has to be
enabled per level.

**Cost**, from the talk's own summary:

> The transform cost is very low, especially since we're using this technique on
> Portal chambers: very blocky geometry with few verts. On the other hand, **the
> fill cost is very high.** … the cost for the sprites is doubled due to the two
> layers of bubbles, which was an artistic choice. We're doing a lookup on the
> normal map, single step parallaxing on the normal map, refraction of the
> underlying surface, cubemap reflections off the bubbles and off the surface of
> the gel…

Mitigations: **clip early in the pixel shader when alpha is below a threshold**;
enable the shader on world geometry **only in levels that use the mechanic**; and
a reduced shader LOD for low-end PC and Mac. Vertex-cheap, fill-expensive, and
every mitigation is about fill — the same conclusion
[`../source2_particles.md`](../source2_particles.md) §13 reaches from the other
direction.

## 6. The blobs: Blobulator

In the air, gel is a **metaball isosurface**, and Valve's name for the system is
in the shipping FGD: `info_paint_sprayer` has a **`RenderMode` keyvalue with
choices `0 : Blobulator`, `1 : Fast Sphere`**. **[VDC]**

The history, from the commentary — this is the whole node, because there is no
other public account of it: **[VALVE-COMMENTARY]**

> **The first implementation of the blob was integrated in the Source engine back
> in 2007.** Over the years, the code has been significantly optimized, but was
> still way too slow to run on game consoles. The blob was a key feature of Portal
> 2, even though we did not know if we could make it work for consoles. In summer
> 2010, we were still considering using a completely different tech for
> consoles — one that would certainly not look as nice. On the 360, even with a
> very low quality blob, we were barely within our performance budget. But we
> really wanted to have the same high quality blob among all platforms. Meanwhile,
> the code was poorly suited for PS3 SPU. **We ended up re-writing all the blob
> code so it would take better advantage of multiple cores and SPUs**, giving us
> quality blobs on all platforms while staying within performance and memory
> constraints.

Four years from prototype to shippable, a genuine possibility of cutting it a
year out, and the fix was **parallelism, not a cheaper algorithm**. VDC lists it
as a branch feature: *"a particle renderer that renders blobs of liquid… A similar
blob particles system was originally present in Source 2007 or Source 2009, but
was disabled in code."* **[VDC]**

**Valve's own type names, from the retail `client.dll`** — this is no longer
inference **[P2-RETAIL]**:

```
Blobulator::IBlobRenderer
Blobulator::LightBucketImplementation::LightweightRenderer
Blobulator::LightBucketImplementation::CSectionBlobRenderer
Blobulator::LightBucketImplementation::CBlobJob
Blobulator::BlobRenderInfo_t          Blobulator::CDrawInfo
NPaintRenderer::C_Blobulator_AutoGameSystemPerFrame
CBasePaintBlob / CPaintBlob / C_PaintBlob / CPaintStream / CPaintStreamManager
BlobTraceEnum        C_OP_RenderBlobs (a particle operator)
```

with cvars `r_threaded_blobulator` (*"If enabled, blobulator will use material
thread"*), `Paintblob_DrawIsoSurface` (*"Draws the surface as an isosurface"*) and
**`cl_blobulator_freezing_max_metaball_radius`** — Valve's word, *metaball*.

Its error strings are the best documentation of the fragment pool the cvars only
hint at:

> `[Blobulator] Preventing deadlock in SwapFragments(). Expect visual corruptions.`
> `[Blobulator] Recycling fragments to draw to prevent deadlock. Expect visual corruptions.`
> `[Blobulator] Switching to one pass as fragment vertex buffer is already half full…`
> `[Blobulator] Too many particles are added to a tile. Some particles will be discarded.`
> `[Blobulator] Can't get fragment, canceling job. Report this issue.`

Read those as a design summary: **jobs produce mesh fragments into a fixed pool;
the pool can starve; when it does the renderer degrades — one pass instead of
two, recycled fragments, discarded particles — rather than stalling or
crashing.** `r_paintblob_force_single_pass` is the manual version of the third
message. That is a real-time system with a hard budget and four documented
graceful-degradation paths, which is exactly what shipping a 2007 research
renderer on a 512 MB console requires.

The shipped cvars describe the rest of the architecture with unusual precision
**[VDC]**, and every name below is present in the retail binaries
**[P2-RETAIL]**:

| cvar (default) | What it says about the implementation |
|---|---|
| `draw_paint_isosurface 1`, `r_paintblob_draw_isosurface 1` | it is an **isosurface**; disabling the second one *"renders individual droplets"* instead of clumping |
| `r_paintblob_highres_cube 0.8` | **cube size** for the polygonisation — *"how round gel should look. The closer to 0, the closer to a perfect sphere. 0 disables rendering, because it would cause the renderer to attempt to make infinite vertices"* → marching cubes over a grid |
| `paintblob_isosurface_box_width 8` | the sampling box per blob |
| `r_paintblob_blr_cutoff_radius 5.5`, `r_paintblob_blr_render_radius 1.3` | the **field function**: an influence cutoff radius (expensive to grow — *"can be much more expensive"*) and a display radius that *"will not scale the effect of blobs morphing together"*. Classic metaball: kernel support vs isolevel |
| `r_paintblob_max_number_of_threads 4` | the multicore rewrite; **setting it to 0 crashes the game** |
| `r_paintblob_debug_spu` | the SPU path, still referenced on PC |
| `r_paintblob_tile_index_to_draw`, `..._debug_draw_tile_boundaries`, `..._max_number_of_{vertices,indices}_displayed 1000000` (*"per tile"*) | the volume is **tiled**, each tile meshed independently with its own vertex/index budget — which is what makes the threading possible |
| `r_paintblob_timeout_for_recycling_fragments 100` (ms), `r_paintblob_use_optimized_fragment_copy 1` | a pool of mesh fragments, recycled on a timeout |
| `r_paintblob_calc_uv_and_tan 1`, `..._calc_tan_only`, `..._calc_color`, `..._calc_hifreq_color` | per-vertex attributes are computed during polygonisation and are individually switchable |
| `r_paintblob_mainview_highres 1` / `r_paintblob_otherviews_highres 0` | **quality LOD keyed on which view is being rendered** — full resolution in the main view, reduced inside portal views, independently switchable |
| `r_paintblob_only_mainview_displayed 0` | and the option to skip blobs in portal views entirely |
| `r_paintblob_force_single_pass 0` | the surface is drawn in **two passes** by default |
| `paintblob_draw_distance_from_eye 18` | blobs closer than this to the camera are not drawn, *"prevents them from blocking the player's view"* |

The last two rows in the LOD block are worth pausing on: **the blob renderer knows
it is inside a portal view and drops quality accordingly.** That is
[`portal2_portal_rendering.md`](portal2_portal_rendering.md) §12's cost problem
showing up in a completely different subsystem, solved the same way — by making
the *representation* cheaper for less important views.

### The simulation side

Blobs are simulated as particles with a small, very legible set of rules
**[VDC]**:

`paintblob_gravity_scale 1`, `paintblob_air_drag 0.1` (*"if the air drag is too
much, gel will behave like a thick jelly"*), `paintblob_update_per_second 60`,
`paintblob_lifetime 1.5` with `paintblob_limited_range 0`, plus a **streaking**
model for shallow impacts — `paintblob_streak_angle_threshold 45`,
`paintblob_radius_while_streaking 0.3`, `paintblob_streak_trace_range 20` — so gel
hitting a wall at a glancing angle runs down it instead of splatting.

**A stream is networked as parameters and a seed, not as positions.** `DT_PaintStream`
carries `m_nBlobRandomSeed`, `m_nMaxBlobCount`, `m_flBlobsPerSecond`,
`m_flBlobSpreadRadius`, `m_flBlobSpreadAngle`, `m_flNoisyBlobPercentage` and
`m_flPercentageSinceLastNoisyBlob` — and nothing resembling an array of blob
positions. **[P2-RETAIL]** So the client runs the same blob simulation from the
same seed and gets the same stream; `draw_paint_server_blobs` and
`draw_paint_client_blobs` exist to look at the two independently, and
`paintblob_interpolation_time_offset` / `paintblob_old_data_time_offset` tune how
the client's copy is aligned in time. A few hundred blobs at 60 Hz would be
unaffordable to replicate and are instead **regenerated**; what *does* replicate is
the durable consequence — the paintmap (§4.0). Same split as everywhere else in
this note: simulate the picture, network the state.

**And Valve's own maps use far fewer blobs than the cap.** From the entity lumps
of the shipped co-op paint maps **[P2-RETAIL]**:

| key | `mp_coop_paint_bridge` | `mp_coop_paint_come_along` | FGD default / max |
|---|---|---|---|
| `maxblobcount` | **40** | **50** | 1 (default), 250 (max) |
| `blobs_per_second` | 40 | 30 | 1 |
| `min_speed` / `max_speed` | 10 / 250 | 0 / 200 | 100 / 100 |
| `blob_spread_radius` | 12 | 12 | 0 |
| `blob_spread_angle` | 12° | 8° | 0 |
| `blob_streak_percentage` | 100 | 100 | 0 |
| `start_radius_min/max` | 0.5 / 0.7 | 0.5 / 0.7 | — |
| `end_radius_min/max` | 0.5 / 0.7 | 0.5 / 0.7 | — |
| `radius_grow_time_min/max` | 0.5 / 1.0 | 0.5 / 1.0 | — |
| `light_position_name` | `light_paint1` | `light_paint2` | — |

**Forty blobs, not two hundred and fifty.** The headline gel waterfall in a Valve
co-op chamber is forty spheres. That is worth holding on to before budgeting any
reimplementation — and it means the Blobulator's tile budgets and fragment pool
exist to survive the *worst* case (several streams, a bomb, blobs through a
portal), not the normal one.

Three of those keys are not in the VDC documentation and matter:
**`start_radius_min/max`, `end_radius_min/max` and `radius_grow_time_min/max`** —
a blob is authored to grow (or shrink) from one radius to another over a randomised
time. The gel *inflating* as it falls is a keyframed property of the emitter, not
an emergent one. `light_position_name` is the other: **the whole stream is lit from
one named entity's position**, not from the world's lighting — one light for
several hundred metaballs.

Emitters are `info_paint_sprayer`, and its keyvalues are the authoring surface —
this is `bin\portal2.fgd`, Valve's own file, shipped with the game:
`maxblobcount` (1–250), `blobs_per_second`, `min_speed`/`max_speed`,
`blob_spread_radius`, `blob_spread_angle`, `blob_spread_percentage` (percentage
that streak), streak time and dampening ranges, an `AmbientSound` choice of
`None / Drip / Medium Flow / Heavy Flow`, and two flags that separate the two
systems cleanly: **`Silent?` = *"blobs will only paint (no render, effect, or
sound)"* and `DrawOnly?` = *"blobs will only render (no paint)"***. The paint and
the picture are independently switchable, which is the clearest possible evidence
that they are separate systems.

A `prop_paint_bomb` is not a special effect: it is **20 blobs**
(`paintbomb_draw_num_paint_blobs 20`) spread over a sphere of radius 25 moving at
50–80 u/s inside it, painting a radius of `paintbomb_explosion_radius 100` on
impact, using the futbol model for collision. **[VDC]**

## 7. Gel × portals, and the tuning table

Gel streams go through portals, and the interaction has its own constants
**[VDC]**:

- `paintblob_minimum_portal_exit_velocity 225` — the same idea as the player's
  floor-exit floor in [`portal2_portal_gameplay.md`](portal2_portal_gameplay.md)
  §4, applied to blobs, so a stream through a floor portal keeps flowing.
- A **vortex** model for blobs inside excursion funnels near portals:
  `paintblob_tbeam_vortex_circulation 30000`,
  `paintblob_tbeam_portal_vortex_circulation 60000` (double, near a portal),
  `paintblob_tbeam_vortex_accel 300`, `paintblob_tbeam_vortex_distance 50`,
  `paintblob_tbeam_accel 200`.
- `r_paintblob_display_clip_box` — blobs are clipped per-portal, i.e. the blob
  renderer participates in the portal view system rather than being drawn once.

The player-facing tuning, all of it exposed **[VDC]**:

| cvar | default | meaning |
|---|---|---|
| `sv_speed_normal` | 175 | walk speed off gel |
| `sv_speed_paint_max` | 800 | max speed on propulsion gel (**4.6×**) |
| `sv_speed_paint_acceleration` | 500 | ramp on and off |
| `sv_speed_paint_ramp_acceleration` | 1000 | faster ramp on ramps |
| `speed_funneling_enabled` | 1 | the "help me hit the portal" assist |
| `bounce_paint_min_speed` | 500 | minimum bounce-off speed, applied if your own would be lower |
| `bounce_paint_wall_jump_upward_speed` | 275 | upward kick added to a sideways wall bounce (*"about 384 units"* of height) |
| `sv_wall_bounce_trade` | 0.73 | fraction of outward velocity traded for upward on a wall bounce; **0 disables wall bouncing** |
| `sv_press_jump_to_bounce` | 3 | 0 = bounce on contact, 1 = only on jump press, 2 = only while held, **3 = jump press *or* landing** |
| `sv_paint_surface_sphere_radius` | 16 | sample radius against the paint map (VDC lists this as `sv_paint_detection_sphere_radius`; the retail binary's name is the one given here) |
| `sv_wall_jump_help` | 1 | assist for repeated wall jumps |

`sv_press_jump_to_bounce` is the commentary's "don't bounce people who walked
onto it by accident" fix, shipped as a four-way enum with the permissive original
still selectable. **The whole feel of repulsion gel is one integer.**

## 8. Bonus: the emancipation grid, or a shader that reads game state

Same talk, same technology, and the best small example in it. The Portal 1 fizzler
was particle-based and *"playtests revealed that players often walked through them
without registering their presence, and were confused by the consequences"* —
they could not report its three rules (closes your portals, blocks portal
placement, destroys carried objects). **[GRIMES]**

The rebuild used the water flow-map technique on an additive surface, with three
stated requirements: non-threatening (you must walk through it), non-solid
looking, and **communicative at the moment of interaction**. So:

- shooting it lights the whole surface — *"communicating its blocking ability"*;
- bringing a cube near it lights it locally **and changes the flow**, *"sucking
  the energy of the field towards the offending object"*.

The implementation is the part to steal: *"We had enough free interpolators to
pass the positions of **two** level objects at a time; this is enough for most
maps."* Those positions are projected into tangent space and used to generate
deltas across the surface — the same construction as the "upwellings" the water
flow maps were built from in Houdini, but evaluated live.

Cost and consequence, stated: 56 arithmetic instructions and 5 texture fetches
including **two dependent** fetches; *"in a couple of cases, level design needed
to change so that you couldn't look through several emancipation grids at once"*;
and a shader LOD dropping the flow for low-end PC and Mac.

A design system, a shader budget and a level-design constraint, all resolved
against each other in one slide. Also the honest forward-looking note: *"the
procedural modification of the flow was very effective, and is promising for
future applications. One can imagine a stream dynamically flowing around player
characters."*

For the flow-map machinery underneath — two phases of a warped color lookup
blended by a cycling noise mask, to hide the reset "pop" — see **[VLACHOS]**,
[Water Flow in Portal 2](https://cdn.akamai.steamstatic.com/apps/valve/2010/siggraph2010_vlachos_waterflow.pdf),
and Grimes' §1 for how the flow fields were *generated*: level geometry exported
to Houdini, a tessellated plane standing in for the water, metaballs scattered as
point attractors, vertex deltas becoming vectors, the cross product with the
surface normal turning attraction into vortices, artist-painted turbulence
masking — templatised until *"we could complete customized flow maps for 30
levels in just a day and a half."*

## 9. What transfers here

**1. The paintmap generalises to any per-surface gameplay state.** A second
channel in the lightmap parameterisation, painted at runtime, read by the shader
*and* by the movement code, is the right shape for scorch marks, blood, mud,
water, ice, frost, and irradiated tiles — anything that must persist, must be
visible, and must change behaviour. Our world is a grid, so we have an even
cheaper unique parameterisation available than a lightmap: **the cell index**. A
byte per cell per effect, indexed by arithmetic, is `OcclusionGrid`'s pattern
applied to surface state. The Valve lesson is not the lightmap — it is *"pick the
representation whose memory cost you can already predict"*.

**2. Gameplay data may not live in a forgetful system.** §3. Decals recycle;
paint cannot. Before putting anything into a pooled/LRU system, ask whether the
simulation reads it.

**3. Never let the overlay hide the affordance.** §4's rejected iteration. This is
a rule for our fog of war, cover indicators, damage states and selection
highlights, and it is worth applying as an explicit review question.

**4. Quality LOD keyed on *which view*, not on distance.** §6 —
`r_paintblob_otherviews_highres 0`. We already have more than one view (shadow
map, and eventually reflections); "is this the view the player is looking at" is a
better LOD input than distance for anything expensive.

**5. Surface-embedded sprites are a real technique with a narrow, useful home.**
§5. Round-looking detail on flat geometry with no geometry and no decal budget —
bubbles, pebbles in mud, rivets, rain droplets on glass. The cost profile is
vertex-cheap and fill-expensive, and the mitigations are known (early alpha clip,
enable per-level, LOD).

**6. Expose the feel constants and ship them.** §7 is ninety cvars, and the
important ones are single integers with obvious meanings
(`sv_press_jump_to_bounce`). The gel mechanic went through a redesign that is
*visible in the shipped default of one variable*. Our equivalents should be
tunable at runtime for the same reason: the tuning is the design.

---

## 10. Verified against the retail install

Read 2026-08-15 from `E:\SteamLibrary\steamapps\common\Portal 2`. **[P2-RETAIL]**

### The blob surface is its own shader, and it is a *thick translucent solid*

`materials/paintblobs/blob_surface_{bounce,speed,portal,erase}.vmt` all use a
shader literally called **`paintblob`** (`CShader@paintblob_dx9` in
`stdshader_dx9.dll`), and the parameters are the "translucent elastomer" brief
from §4 turned into numbers:

```
paintblob {
    $baseTexture   "paintblobs\blob_surface_bounce"      $uvscale "0.006"
    $normalMap     "paintblobs\blob_surface_normal"      $bumpstrength "0.4"
    $phongexponent  40   $phongboost 2                   // broad lobe
    $phongexponent2 400  $phongboost2 7                  // tight highlight
    $envMap        "paint\paint_envmap_hdr"
    $interior 0   $interiorFogStrength "0.00325"  $interiorFogLimit "0.56"
    $interiorFogNormalBoost "8.5"  $interiorBackgroundBoost 2
    $interiorAmbientScale 1  $interiorBacklightScale 1
    $interiorColor "[0.1 0.6 0.9]"  $interiorRefractStrength "0.06"   $nocull 1 }
```

Three things worth naming. **Two specular lobes** (40/2 and 400/7) — a soft sheen
plus a tight wet highlight, which is what reads as "gel" rather than "plastic".
**An entire `$interior*` model**: fog strength and limit, a normal boost on that
fog, a backlight scale, an ambient scale and a refraction strength. That is a
cheap participating-medium approximation *inside* the blob — you are meant to see
depth into it, and the cleanser variant turns `$interior 1` on with ten times the
fog strength and adds `$rimlightboost 300` and a `$fresnelWarpTexture`. **`$nocull
1`**, because an isosurface's back faces are the far wall of the same blob. The
shader DLL's own parameters confirm the machinery: `$INTERIORREFRACTSTRENGTH`,
`$INTERIORREFRACTBLUR`, `$LOCALREFRACT`, `$LOCALREFRACTDEPTH`, `$BLOBBYSHADOWS`.

There is also a separate `Blob` / `Blob_dx9` shader in the same DLL — the 2007
lineage the commentary mentions, still compiled in.

### Paint capability is a template mixin

The retail RTTI names are a design document:

```
IPaintPowerUser
CPaintableEntity<C_BasePlayer>          CPaintableEntity<CBaseMultiplayerPlayer>
CPaintableEntity<CBaseProjectedEntity>  CPaintableEntity<CNPC_FloorTurret>
CPaintableEntity<CPhysicsProp>
PaintPowerUser< CPaintableEntity<...> >
PropPaintPowerUser<CPhysicsProp>        PropPaintPowerUser<CNPC_FloorTurret>
PlayerPickupPaintPowerUser<CPhysicsProp>  PlayerPickupPaintPowerUser<CNPC_FloorTurret>
```

**"Can be painted" and "is affected by paint" are two separate policies, composed
onto entity classes by template rather than by inheritance from a common
gameplay base** — and there is a third specialisation, `PlayerPickupPaintPowerUser`,
for *an object being carried by the player*, because a bouncy cube in your hands
must not bounce. `CPaintableEntity<CBaseProjectedEntity>` is the light bridge, which
is why `cl_draw_projected_wall_with_paint` exists. The commentary's *"moving paint
around became a puzzle in and of itself, so we began to create puzzles that
depended on painting objects"* is this table.

`CPaintStream` holds `SharedVar_m_sharedBlobData` (a `CUtlVector<BlobInterpolationData_t>`)
guarded by a `SharedVar_m_sharedBlobDataMutex` (`CThreadFastMutex`) — the blob
array is genuinely shared across threads and **interpolated on the client**, with
`paintblob_interpolation_time_offset` and `paintblob_old_data_time_offset` to tune
it. And `CPaintSaveRestoreBlockHandler` means **the paint map is written into save
games**, which closes §3's argument: gameplay depends on it, so it persists.

### Cvars: VDC's list checks out, with additions

All ~90 `paint*` names from VDC are present. Ones VDC does not list, worth having:
`paint_compute_contacts_simd`, `paintblob_applies_impulse` (blobs push things),
`paintblob_collision_box_size`, `paintblob_streak_particles_enabled`,
`sv_paint_alpha_coat`, `sv_bounce_paint_forward_velocity_bonus`,
`sv_speed_paint_straf_accel_scale`, `draw_paint_server_blobs` /
`draw_paint_client_blobs` (the simulation runs on both sides and each can be drawn),
and `paint_power_look_ahead_sample_density`.

Leftovers of the cut gels are still in the shipped binary — `paint_bomb_stick`,
`paint_splat_stick_01` — matching §2's "adhesion, removed by DLC1".

### Where the rest of the evidence came from

- **`bin\portal2.fgd`** — Valve's own entity definitions, shipped with the game:
  every keyvalue in §6 with its real default, the `Blobulator` / `Fast Sphere`
  render-mode enum, and the `Stick` naming of §2.
- **Shipped `.bsp` entity lumps** — `mp_coop_paint_bridge`,
  `mp_coop_paint_come_along`, `sp_a3_bomb_flings`: the `paintinmap` worldspawn
  key, the real sprayer tuning in §6, and a real `prop_paint_bomb`
  (`PaintType 0`, `BombType 1` wet, `allowfunnel 0`, wired
  `OnExploded → trigger`). BSP `VBSP` v21, **no paint lump**.
- **`bin\engine.dll`** — `CPaintmapDataManager`, `IPaintmapDataManager`,
  `SVC_PaintmapData`, `R_CheckForPaintmapChanges`, `r_redownloadallpaintmaps`,
  the alpha cvars, and the memory-leak assert quoted in §4.0.
- **`portal2\bin\client.dll`** — `DT_PaintStream`, `DT_TEWallPaintedEvent`,
  `DT_PropPaintBomb`, `DT_PaintSprayer`, `m_nPaintPowerType`,
  `m_PaintedPowerTimer`, `m_iPaintPower`, `CPaintDatabase::PreClientUpdate`,
  `CPaintStreamManager::PaintStreamUpdate`, `PaintAllSurfaces`.

### Still not verified

Shipped cvar *values* (names and help strings mine reliably, defaults do not),
the paintmap's on-GPU texture format and per-texel layout, and the real per-frame
cost of the blob renderer. A RenderDoc capture on a gel chamber answers the last
two directly; `r_paintblob_wireframe 1` is Valve's own tool for the second.

---

## Sources

- **[GRIMES]** Bronwen Grimes, *Making and Using Non-Standard Textures:
  Manipulating UVs through Color Data in Portal 2*, GDC 2011 —
  [PDF](https://cdn.akamai.steamstatic.com/apps/valve/2011/gdc_2011_grimes_nonstandard_textures.pdf)
  (89 slides with speaker notes; gels from slide 65, emancipation grid from 58).
- **[VLACHOS]** Alex Vlachos, *Water Flow in Portal 2*, SIGGRAPH 2010 Advances in
  Real-Time Rendering —
  [PDF](https://cdn.akamai.steamstatic.com/apps/valve/2010/siggraph2010_vlachos_waterflow.pdf).
- **[VALVE-COMMENTARY]** Portal 2 developer commentary, transcript at
  [theportalwiki.com](https://theportalwiki.com/wiki/Portal_2_developer_commentary)
  — nodes *Tag: The Power of Paint*, *Porting Paint*, *Speed Paint*, *Portal
  Paint*, *Blobulator*, *Gel Sounds*.
- **[VDC]** [Gel (Portal 2)](https://developer.valvesoftware.com/wiki/Gel_(Portal_2)),
  [Gel-related console commands](https://developer.valvesoftware.com/wiki/Gel-related_console_commands),
  [info_paint_sprayer](https://developer.valvesoftware.com/wiki/Info_paint_sprayer),
  [prop_paint_bomb](https://developer.valvesoftware.com/wiki/Prop_paint_bomb),
  [paint_sphere](https://developer.valvesoftware.com/wiki/Paint_sphere),
  [Portal 2 engine branch](https://developer.valvesoftware.com/wiki/Portal_2_engine_branch).
- **[P2-RETAIL]** The retail install, `E:\SteamLibrary\steamapps\common\Portal 2` —
  `portal2\pak01_dir.vpk` (`materials/paintblobs/*.vmt`, `materials/paint/*`),
  `bin\stdshader_dx9.dll`, `portal2\bin\{client,server}.dll`. See §10.
