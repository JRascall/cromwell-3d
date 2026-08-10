# Elite Dangerous — reference notes

How Frontier render a **1:1 Milky Way** — 400 billion star systems, every one
visitable, every landable surface walkable down to millimetre precision — and
store essentially none of it.

The planet renderer is the headline and §3 is the longest section, but it is not
separable from the rest: the terrain works the way it does *because* the galaxy
is a pure function, and the galaxy can be a pure function *because* nothing
downstream needs to store its output. That through-line is §1 and it is the
reason this game is worth reading rather than just admiring.

> **Read alongside:** [`space_engineers.md`](space_engineers.md) — the other
> "editable volume at solar-system scale" game in this folder, and the
> instructive contrast. SE stores a diff against a generator; ED stores nothing
> at all, and pays for that in a different currency. §4.3 and §9 read them
> against each other directly.
> [`map_scale.md`](map_scale.md) is the note about *extent* rather than count,
> which is the family this belongs to.
> [`rdr2_atmospherics.md`](rdr2_atmospherics.md) for participating media, which
> §5 is the thin-atmosphere cousin of.

---

## Sourcing, and the caveat

**Frontier have published almost nothing formal about COBRA or the planet
renderer.** No GDC talk, no SIGGRAPH paper, no slide deck with costs. This is a
weaker evidence base than [`re_engine_rendering.md`](re_engine_rendering.md)
(ten years of Capcom decks) or [`space_engineers.md`](space_engineers.md) (a
released codebase), and the note is structured to make that visible rather than
paper over it.

| tag | source | strength |
|---|---|---|
| **[ROSS]** | **Doc Ross / Dr Kay Ross**, Elite's lead render programmer, in a long technical interview and a community Q&A. **The single richest source on this game** and the origin of most of §3. | Strong. Direct quotes, but an interview — no numbers, no profiles. |
| **[FD]** | Other named Frontier staff — David Braben, Jonny Watts, Jonathan Bottone — in press interviews | Moderate. Executive-level; accurate in direction, vague in mechanism. |
| **[COMMUNITY]** | Player reverse-engineering of the galaxy's addressing, graphics-setting analysis, forum observation | **Weak but unusually good here.** The exploration community has mapped the sector/boxel scheme empirically over a decade. |
| **[inferred]** | Our reading. Not anybody's word. | — |

**The one thing to hold on to:** where this note describes ED's terrain
pipeline, that is Ross's own description and it is unusually specific for an
interview. Where it describes ED's *renderer* — depth precision, the lighting
pipeline, the frame structure — **there is no published source at all**, and
those parts say so instead of guessing.

---

## 1. The one thing that matters most

**Elite Dangerous stores no world. Every object in it is a pure function of a
64-bit integer, and the entire architecture — rendering, streaming, networking,
persistence — is downstream of that one decision.**

**[ROSS]** The structure is explicitly hierarchical and top-down:

> "Hierarchical data is useful here, where the **smallest details on planets are
> informed by the results of planet-scale information generation**, which are
> informed by star-system scale information, which are informed by galactic
> information."

Read that as a dependency chain with a very specific property: **each level's
output is the next level's seed**, so a mountain's shape is a deterministic
function of the planet's composition, which is a function of the system's
formation, which is a function of the galaxy's mass distribution at that point.
Nothing is looked up. Everything is recomputed.

**[inferred]** Four consequences, and they are the whole game:

1. **Storage is O(1) in world size.** 400 billion systems cost the same on disk
   as one. What ships is the generator plus a small catalogue of real stars and
   hand-authored content.
2. **Streaming is generation.** There is no asset to fetch for a mountain — you
   run the noise graph at the LOD you need. So "load distance" and "detail
   level" are the same dial, and the budget is *compute*, not bandwidth.
3. **Networking barely has a world to replicate.** Two clients 30,000 ly apart
   agree on what they will see without exchanging anything, because they compute
   the same function. §7 is the payoff.
4. **And it is a one-way door.** Change the generator and every player's
   memory of every world is wrong. §2.5 and §3.6 are both the bill for this.

**[inferred]** Compare [`space_engineers.md`](space_engineers.md) §4.2, which is
the same idea stopped one step short: SE's voxel storage is *provider plus a
stored diff*, so the player can dig a hole. ED has no diff, so ED's terrain is
not editable — and the two designs are exactly as different as those games are.
**The presence or absence of a diff layer is the whole architectural choice**,
and everything else follows from it.

---

## 2. Stellar Forge — the galaxy

### 2.1 It is a simulation, run once, not a noise field

**[FD/COMMUNITY]** Stellar Forge is not "noise that looks like stars". Frontier's
description is of an astrophysical simulation:

> Galactic mass distribution is taken as input, time is rolled **backwards
> toward the Big Bang**, and formation is simulated forward from there — gas
> collapsing under gravity, protoplanetary discs, accretion, "tidal forces,
> orbital resonances and gradual accretion of mass gradually chang[ing] their
> orbits, causing collisions, collapse and close encounters."

**[ROSS]** Each resulting body carries simulated properties, not authored ones —
"rough classification based on how much mass it has, the types of materials it
is made from, its volcanic parameters, a temperature differential" — with later
history applied: "solar winds blowing away materials, catastrophic events, tidal
locking, and gravitational heating."

**[COMMUNITY]** For Horizons' terrain specifically, "the movement of tectonic
plates [is] simulated to inform landmass creation, and the planet's chemical
makeup control[s] its color and geologic layers."

**[inferred] The reason this matters is not realism, it is *correlation*.** A
planet generated from independent noise fields has a colour that has nothing to
do with its density, which has nothing to do with its crater count. A planet
generated from a formation simulation has all three tied to the same causes, so
it reads as a *place* rather than as a parameter sample. That is the actual
product of the simulation and it is why Stellar Forge earns its cost — which is
paid **once, offline**, not per frame.

### 2.2 The addressing scheme, and why it is one integer

**[ROSS]** The identifier is the architecture:

> "A **64-bit integer** number can store the x, y, z coordinate of a sector of
> space, the sector layer (sectors come as part of an **eight-layer octree**),
> the ID of the star-system within the sector and the ID of the body within the
> star-system."

and the reason:

> "Due to the procedural nature of the game, **every object needs a unique
> identifier** so that each client and the server knows you're talking about the
> exact same object."

**[COMMUNITY]** The exploration community has mapped the layout empirically. A
**sector** is a 1,280 ly cube. Inside it, **boxels** (Frontier's term is
"subsectors") come in eight sizes by **mass code a–h**: mass code `a` is 10 ly
on a side, doubling up to `h` at 1,280 ly, each nested inside the next. Heavier
bodies live in the coarser layers. System names encode the boxel and an index
within it — `Vegnue WK-E d12-329` is boxel `WK-E d12`, mass code `d`, system
329.

**[inferred] Three things worth taking from that, none of them about space
games:**

1. **The octree layer is part of the key, not a lookup path.** You do not
   descend a tree to find a system; you *unpack an integer* and it tells you
   which layer, which cell, which index. The tree is conceptual — the addressing
   is arithmetic. That is the same move as a Morton code and the same move as
   this project's flat-array cell indexing.
2. **Mass code as the layer selector is a good idea in general.** Rare, heavy
   things live in coarse cells; common, light things live in fine cells. One
   structure, density-adaptive, with the adaptation driven by the *content*
   rather than by a rebalancing pass.
3. **The ID has to be assigned before the object exists.** In a generated world
   the identifier cannot be "the index where I stored it", because nothing is
   stored. It must be derivable from position and kind alone — which forces the
   packed-coordinate design, and is exactly why it is one integer rather than a
   handle.

### 2.3 Real stars, seeded in

**[ROSS]**

> "The exceptions come at a star-system level, where information from the
> **Hipparcos and Gliese stellar catalogs** are used to seed our generated Milky
> Way with real stars."

**[inferred]** The pattern is worth naming because it recurs everywhere:
**authoritative data overrides the generator at specific cells, and the
generator fills everything else.** It is the same shape as SE's edit diff
(§1) and the same shape as any "hand-place the important things, generate the
rest" pipeline. The generator's job is not to produce everything; it is to
produce *the background against which the authored things are placed*.

### 2.4 Determinism is a networking strategy

**[inferred]** This is the under-appreciated half of Stellar Forge and §7 is
where it pays out. Because every client can compute the galaxy, **the server
never has to send it.** What must be replicated is only what cannot be derived:
players, their ships, mission state, market prices, the political layer. That is
a tiny fraction of what an authored 1:1 Milky Way would have to stream, and it
is why a game of this extent runs on a peer-to-peer instancing model at all.

### 2.5 What it costs — the one-way door

**[COMMUNITY]** The simulation ran once and its output is now effectively
canonical: a decade of players have named, catalogued and built fiction on top
of specific systems. **Re-running Stellar Forge with a changed model would
invalidate all of it.** So the generator is frozen in a way ordinary content is
not — bugs in it become permanent features of the world.

**[inferred] Generalise this before adopting the technique:** a deterministic
generator is not "content you get for free". It is **content you can never
edit**, in exchange for content you never have to store. If any part of the
world needs to be fixable after ship, that part needs a diff layer, and the
diff layer needs designing up front (§9).

---

## 3. Planet rendering — the main event

### 3.1 The geometry: cube → quadtree → sphere → noise, on the GPU

**[ROSS]** The clearest description Frontier have given of anything:

> "The 'landable' surfaces start as a **cube with square sub-dividing faces
> which behave as quadtrees**. These faces are uniform tri-meshes which, as you
> get closer to one of them, further sub-divided into four sub-patches with
> closer points."

> "The patches continue to subdivide **depending on your distance from the
> surface**, ensuring that the vertex density is higher where most useful, down
> to a target final resolution."

> "Using **compute shaders on your GPU**, the patches undergo **spherification
> via a mathematical function** and they are subject to **noise graphs**" —
> which are "collections of noise equations which take the **point's position
> and unique planet ID** as input information".

**[inferred]** Unpacking the choices, because each one is load-bearing:

- **A cube, not a sphere.** Six quadtrees over square faces, projected outward.
  This is the standard quadrilateralised-sphere approach and it exists because a
  quadtree over a square is trivial and a quadtree over a sphere is not. The
  distortion at cube corners is the known cost and is why "spherification via a
  mathematical function" is called out separately — a naive normalise gives
  badly uneven vertex density, so there is a corrective mapping.
- **Uniform tri-meshes per patch.** Every patch is the *same mesh* — the same
  vertex buffer, the same index buffer — displaced differently. So the geometry
  is one instanced asset and what varies is the per-patch transform and the
  displacement the compute shader writes. This is what makes thousands of live
  patches affordable.
- **Position + planet ID as the noise input.** Not a seed threaded through a
  generator: the noise is a **pure function of where you are and which planet
  you are on**. That is what makes patch generation embarrassingly parallel, and
  what makes two clients agree without communicating.
- **Compute, not vertex shader.** Because the output is *reused* — the same
  displaced patch feeds rendering, collision, and (§3.4) the lighting bake. A
  vertex shader would recompute it per draw and throw it away.

**[inferred]** Note the ordering against `CLAUDE.md`'s rules: the quadtree
subdivides toward the camera, so **vertex density tracks screen-space need**,
which is "do less work" applied to geometry. The compute-shader displacement is
"do it closer" — layout and parallelism — and it comes second. The published
description gets these in the right order, which is mildly reassuring about the
rest of it.

### 3.2 The precision problem, and the two maths libraries

**[ROSS]** The clearest statement of the actual engineering difficulty:

> "To ensure consistent visuals and gameplay on the screen and between users we
> need **millimetre precision**, but the input values for the noise functions
> depend on the point on the surface of the world relative to the planet's
> centre, **which can be of the scale of tens of billions of millimetres**."

That is roughly 10¹⁰ dynamic range. A 32-bit float has ~7 decimal digits; this
needs ~13. So:

> "We have written **alternate libraries** to create the functions in 64 bit —
> i.e. **double precision** and **dual-float precision**. The former is native
> 64-bit handling floating point numbers and the latter is **emulated 64-bit
> functionality using two 32-bit floats**. Some GPUs handle one better than the
> other, or not at all, and they need good graphics card coverage."

**[inferred]** Three observations, and the third is the one to remember:

1. **Dual-float (float-float / "double-single") is the standard trick** for GPUs
   with crippled FP64. A value is stored as a high float plus a low residual and
   arithmetic is done with error-compensated sequences. It costs perhaps 5–10×
   the operations of native float but works everywhere, whereas consumer GPU FP64
   rates are commonly 1/32 or 1/64 of FP32.
2. **They shipped both, chosen per GPU.** That is a real maintenance burden — two
   implementations of the entire noise library, which must produce *bit-identical
   enough* results that two players see the same mountain. **[inferred]** That
   constraint is brutal and is not mentioned in the interview, but it follows
   directly from §2.4: if the two paths diverge, determinism across clients
   breaks.
3. **The precision requirement comes from the noise input, not from the camera.**
   This is the counter-intuitive part. Floating origin fixes *rendering* at
   distance; it does not help here at all, because the noise function is
   evaluated in **planet-centred coordinates by definition** — that is what makes
   it stable and view-independent. You cannot rebase the input to a procedural
   function without changing its output. **The generator's coordinate frame is
   fixed by determinism, and precision must be bought rather than dodged.**

**[inferred]** That is a genuinely transferable warning for any procedural
system: *the usual large-world tricks do not apply to the generator's own input
space.*

### 3.3 The LOD switch: past a threshold, terrain becomes a texture

**[ROSS]**

> "**Above a certain LOD level of the planet surface patches, flat geometry is
> used with textures generated for the look, normals, and height of the
> terrain**, which provides additional surface detail from orbit."

**[inferred] This is the single most transferable technique in the note.** Past
a distance, the quadtree stops subdividing geometry and instead **bakes the
would-be terrain into per-patch textures** — albedo, normal, and height — applied
to a flat (well, spherically-curved but undisplaced) patch. The silhouette is
wrong at that distance and nobody can tell; the shading is right, which is what
the eye actually reads.

The economics: a patch that would need thousands of vertices to hold its detail
becomes one low-poly patch plus three texture samples. And crucially the bake is
**the same compute shader**, run once into a render target instead of into a
vertex buffer, so there is no second implementation to keep in step — which is
`CLAUDE.md`'s derived-cache rule 2 satisfied for free.

**[inferred]** The parallel to this project is direct. Beyond the storey the
player is inspecting, geometry detail is wasted and shading detail is not; the
same "bake the emitter's output to a texture past a threshold" move applies to
`StoreyGeometryEmitter` output, and the fact that the *generator is shared*
between the geometry path and the texture path is what makes it safe.

### 3.4 Per-patch lighting, baked and shared with neighbours

**[ROSS]**

> "Each patch also **generates lighting information used by it and its
> neighbours**" — including "the shapes of shadows that would be cast across the
> surface and regions that would appear brighter due to **sub-surface
> scattering**."

**[inferred]** So alongside the height bake, each patch produces a lighting
product: **terrain self-shadowing** (long shadows cast by distant ridges, which
no shadow map at planetary scale could resolve) and a **cheap SSS term** for
scattering in dust and ice.

Two details worth extracting:

- **"used by it and its neighbours"** is a locality contract. A patch's shadow
  bake is valid for a bounded radius, so a neighbour can sample it instead of
  computing its own. That halves the work and, more importantly, makes the
  result *continuous across the patch seam* — which is where this class of
  technique normally shows its joins.
- **The bake is view-independent but sun-dependent.** ED's sun moves slowly
  (planetary rotation), so the bake has a long lifetime. **[inferred]** This is
  the same economics as [`re_engine_rendering.md`](re_engine_rendering.md) §1's
  cached shadow maps: cache what changes slowly, repair what changes fast, and
  the split is chosen by *rate of change*, not by what is expensive.

### 3.5 Texturing — Wang tiling and triplanar

**[ROSS]**

> "When Horizons originally launched we used entirely computer simulated
> planets... In an update, **artist-generated textures were introduced for rock,
> sand, and scree per material** on a planet, **blending depending on the
> gradient of the terrain**. **Wang-tiling** was used to break up the pattern of
> the texture. **Tri-planar blending** is also utilised to make sure no
> stretching happened to the textures across the curved 3-D surface."

**[inferred]** All three are standard and all three are the right answer:

- **Slope-based blending** — rock on steep faces, scree on moderate, dust in
  flats — is free (you already have the normal) and does most of the work of
  making terrain read geologically.
- **Wang tiling** breaks the repeat without a bigger texture, by tiling from a
  small set of edge-compatible variants chosen by a hash of the cell. Cheaper
  than the usual alternative (stochastic/histogram-preserving tiling) and it
  cannot produce the blend artefacts that one does.
- **Triplanar** is unavoidable on a sphere: there is no UV parameterisation of a
  planet that does not stretch somewhere. Cost is 3× the samples, mitigated by
  weighting toward the dominant axis and skipping the others where the weight is
  negligible.

### 3.6 Horizons → Odyssey: the trade Frontier actually made

**This is the most valuable design lesson in the note and it runs opposite to
the intuition.**

**Horizons (2015)** generated terrain from noise, in Ross's phrase "entirely
computer simulated planets", with separate generators for rocky and icy worlds.
**[COMMUNITY]** Its known failure was **craters in a visibly regular grid** —
the crater field was enumerated over the patch lattice and inherited the
lattice's structure.

**[inferred] That is exactly the failure `CLAUDE.md` names and
[`spatial_queries.md`](spatial_queries.md) §5.2 documents: enumerating candidates
in the shape of the grid rather than the shape of the question.** Craters are a
radial, Poisson-distributed phenomenon; sampling them per lattice cell puts one
in every cell and aligns them to the axes. It is the same bug as units
converging on eight headings, at a completely different scale, in a shipped AAA
game. Worth having as the second data point — **any per-cell enumeration
inherits the cell structure, and the artefact is always visible in the
aggregate even when each individual sample looks fine.**

**Odyssey (2021)** replaced it. **[ROSS]**

> "We now have **terrains and terrain materials of various types which are
> deterministically selected and blended together depending on Stellar Forge
> properties**."

> "Landable planets are still classified between rocky, icy, rocky ice, high
> metal content and metal rich, but **terrains depend on simulation values more
> than discrete titles**."

> "Planets now use a **mix of procedural features and pre-baked, hand-crafted
> assets, all blended together**."

> "Blending distinct regions together: flat areas and mountainous areas."

**[ROSS]** And the stated goal: "to make more terrain details readable from
afar, scaling down consistently as you approach a world's surface."

**[COMMUNITY] The cost, and it is real:** Odyssey's terrain visibly **tiles**.
The community diagnosis is direct and almost certainly correct — "these more
'realistic' surface features require pre-made height-maps. The old system
generated the terrain from noise." Frontier patched to reduce it; it is still
findable.

**[inferred] So the trade, stated plainly:**

| | Horizons (pure noise) | Odyssey (blended authored templates) |
|---|---|---|
| Variation | **unbounded** — never repeats | **bounded** by the template library |
| Plausibility | poor — noise does not know what a mesa is | **good** — an artist made the mesa |
| Artefacts | **structural** (grid-aligned craters) | **repetition** (visible tiling) |
| Readable at distance | poor | **the stated goal** |
| Fixable | only by changing the generator (§2.5) | **add templates** |

**[inferred] The lesson: a mature procedural system converges on authored
content selected and blended procedurally, not on better noise.** Frontier had
six years and a dedicated team, and they moved *away* from pure generation. The
reason is that noise produces variation without meaning — every hill is a
different hill and none of them is a landform. Selecting authored pieces by
simulated properties keeps the determinism and the coverage while buying back
the meaning, and the price is a repeat you have to manage.

Note also which half stayed: **selection is still deterministic and still driven
by Stellar Forge values**, so §1's whole architecture survives intact. They
changed *what gets placed*, not *how placement is decided*.

### 3.7 The scatter system

**[ROSS]**

> "We have what we call the '**scatter system**'. Different collections of things
> are expected on different planet areas — you get [a] wider range of things in
> different patterns and densities."

> "Scatter system for rocks, flora, etc will now be **more systemic and
> deterministic for each planet**, so that assets are in more of a natural
> place."

**[COMMUNITY]** This replaced Horizons' approach, where "rocks were placed
around the player by the CPU as they drove along."

**[inferred] That replacement is the interesting part, and it is a bug class
worth naming.** Camera-relative placement is cheap and it is wrong: the objects
are a function of *where the player has been*, not of *where the world is*. So
it cannot be deterministic across clients, it cannot survive the player leaving
and returning, and it cannot be seen from orbit because it only exists near
someone. Making scatter a deterministic function of position fixes all three at
once — and it is the same fix as §3.1's "position + planet ID" noise input,
applied to props instead of to height.

**The rule: anything a player can observe from two places must be a function of
the world, not of the observer.** Cheap observer-relative generation is a trap
that only shows up when a second observer arrives — which in a multiplayer game
is always.

### 3.8 "Terrain Work" — shipping the CPU/GPU split as a dial

**[COMMUNITY]** ED exposes a graphics setting called **Terrain Work** that does
not change quality at all. It **moves terrain generation between the CPU and the
GPU**: low = CPU-heavy, high = GPU-heavy. It sits alongside separate Terrain
Quality, Terrain LOD, Terrain Material and Terrain Sampler settings.

**[inferred]** Two readings and both are worth holding:

- **The honest one.** Terrain generation is a large, parallel, self-contained
  workload that can run on either processor, and which one is the bottleneck
  depends entirely on the machine. Frontier could not pick, so they shipped the
  dial. For a game whose hardware range spans a decade, that is defensible.
- **The uncomfortable one.** It means **two implementations of the generator**,
  and they must agree — see §3.2's identical problem with the two precision
  libraries. That is a real, permanent tax, and it is the kind of thing that
  looks like flexibility in a settings menu and like a maintenance burden in a
  bug tracker.

**[inferred]** For this project the takeaway is the *diagnosis*, not the
feature: when a workload is genuinely portable between CPU and GPU, the right
answer is usually to pick one based on where the rest of the frame's pressure
is, and to only ship a dial if you cannot. `CLAUDE.md`'s GPU note already says
the readback is usually the problem rather than the dispatch — and note that
ED's terrain **has no readback** in the GPU path, because the output stays
resident for rendering. That is exactly the condition under which moving work to
the GPU pays.

### 3.9 What transfers

**[inferred]**

| Take | Why |
|---|---|
| **Bake geometry to textures past a LOD threshold** (§3.3), using the *same* generator | Highest-value technique here; satisfies the derived-cache rules by construction |
| **Per-patch lighting bakes with a neighbour-sharing contract** (§3.4) | Continuity across seams for free; cache by rate-of-change |
| **Slope-based blend + triplanar + Wang tiling** (§3.5) | Standard, cheap, and the right three |
| **Deterministic scatter from world position, never observer position** (§3.7) | Fixes multiplayer agreement, persistence and distance visibility at once |
| **Authored templates selected procedurally, not better noise** (§3.6) | The lesson six years of Frontier's iteration bought |
| **Never enumerate a radial phenomenon over a lattice** (§3.6) | Second confirmed instance of `spatial_queries.md` §5.2 |

---

## 4. Precision and scale

### 4.1 64-bit, and when they had to

**[FD]** David Braben, on why COBRA still exists:

> "Rendering planets, you've got such a big draw distance, unless you make them
> into 'Clangers' planets that you can represent with 32-bit floats. **When you
> need full 64-bit, it's a challenge and there isn't another engine that does
> that.**"

**[COMMUNITY]** And the timing: ED went 64-bit **only when planetary landing was
added**. Space alone did not force it; a surface you can walk on did.

**[inferred]** That is the correct instinct and worth stating as a rule.
Precision is not a scale problem, it is a **ratio** problem: the world's extent
divided by the smallest distinguishable feature. Empty space at 1:1 scale has
enormous extent and no small features, so 32-bit is fine. Add a surface with
millimetre detail and the ratio explodes — the *world did not get bigger, the
detail got smaller.*

**[inferred]** So the diagnostic to apply to any project: **what is the ratio of
your largest coordinate to your smallest meaningful difference?** Under ~10⁷,
float is fine. Over it, you need a plan, and the plan should be chosen before
the code has float in a thousand signatures. This is precisely the kind of
decision `CLAUDE.md` means by "expensive to change later".

### 4.2 Depth buffer — no published information

**[inferred]** Rendering a 1:1 solar system means a depth range from a
centimetre to an astronomical unit, and **Frontier have published nothing about
how they handle it.** The available answers are well known and any of them is
plausible here:

- **Reversed-Z with a float depth buffer** — the modern default; the 1/z
  nonlinearity cancels against the float exponent's distribution and gives
  near-uniform relative precision. Cheapest and best when available.
- **Logarithmic depth** — writes a log-distributed depth from the pixel shader;
  works on older hardware but disables early-Z, which is expensive.
- **Segmented frusta** — render the scene in several depth ranges with separate
  clears. Robust, costs extra passes, and is the traditional space-sim answer.

This note is not going to guess which. **It is recorded because the problem is
mandatory at this scale and a reader would otherwise assume it was solved
somewhere above.**

### 4.3 Two games, two answers to the same question

**[inferred]** ED and Space Engineers both have to run a physics and rendering
world at solar-system extent, and they answer it differently. The comparison is
the most useful thing in this section:

| | **Elite Dangerous** | **Space Engineers** ([`space_engineers.md`](space_engineers.md) §3.3) |
|---|---|---|
| World coordinates | 64-bit doubles throughout | 64-bit doubles |
| Physics coordinates | — (see below) | **32-bit, inside per-cluster local frames** |
| Mechanism | **raise the precision** | **partition into 20–100 km clusters with a 2 km margin** |
| Objects far apart can interact | n/a — instanced, §7 | **no, by construction** |
| Cost | two maths libraries, GPU coverage matrix | cluster assignment logic, no cross-cluster physics |

**[inferred]** SE's answer is cheaper and it is available because SE's physics is
*local by nature* — ships collide with things next to them. ED could not use it
for terrain, because §3.2's precision requirement is in the **generator's**
frame, which cannot be rebased. **So the choice is decided by whether your
precision problem is in the simulation or in a pure function**: simulation can
be partitioned, a pure function cannot.

---

## 5. Atmospheres and lighting

### 5.1 The Odyssey PBR pass

**[ROSS]** Odyssey brought "the new **Physically Based Rendering** system
throughout the game", explicitly including stars, plus "**per-pixel lit
particles**, more shadowed spotlights working together, and physically based
materials with **roughness information** for realistic light response."

**[inferred]** Standard 2021 modernisation, notable only for how late it is —
which is what a 2014 engine with a live service looks like. The per-pixel lit
particles matter more than they sound in a game whose signature effects are
thruster plumes and dust.

### 5.2 Scattering, driven by the simulated composition

**[ROSS]**

> "**Rayleigh scattering** for light refraction based on the star light's
> wavelength and planet atmosphere density"
>
> "**Mie scattering** for light absorption based on atmosphere elements and
> composition"

> "Different planets' atmospheres will have different colours based on their
> primary composition" — "Oxygen, Argon, Neon, etc. absorb light differently".

**[inferred] The technique is textbook; the input is the interesting part.** The
scattering coefficients are not art-directed per planet — they are **derived
from the Stellar Forge composition**, which is §2.1's correlation principle
applied to the sky. A world with a neon atmosphere gets its sky colour from the
same fact that determines its surface chemistry. That is why the skies read as
belonging to their planets, and it costs nothing beyond wiring the simulation
value into the coefficient.

**[FD]** Jonathan Bottone (Art Director) on sunsets: "A new algorithm considers
atmospheric composition, star colour and distance, plus other factors, resulting
in huge range of alien feels."

**[ROSS]** And the difference it makes to surface lighting: "The lighting on a
world with atmosphere differs from airless worlds — you don't have atmospheric
contribution as much, it's slightly **starker**."

### 5.3 The honest limit

**[COMMUNITY]** **Only tenuous atmospheres are landable.** Odyssey shipped thin
atmospheres — enough for scattering, sky colour and a horizon haze — and not
dense ones. No volumetric clouds, no weather, no aerodynamic flight.

**[inferred]** That boundary is informative. Thin atmospheres are an *analytic*
problem: two scattering integrals along a ray, evaluated per pixel, with no
participating-medium volume to march. Dense atmospheres are the
[`rdr2_atmospherics.md`](rdr2_atmospherics.md) problem — froxel volumes, cloud
raymarching, aerial perspective, and a flight model that has to care about lift.
**The gap between them is not incremental**, and Frontier's decade of not
crossing it is the strongest available evidence of that. Anyone budgeting "add
atmospheres" should read the two sections together.

---

## 6. Physics and flight

### 6.1 There are no orbital mechanics, and that is the right call

**[COMMUNITY]** Stated plainly by the community and never contradicted by
Frontier: planets and moons move on rails around their stars, and **the player
is not affected by orbital motion at all**. Drop out of supercruise beside a
planet and you hang there — no orbit, no free fall.

**[inferred]** This is a deliberate and correct decision, and it is worth
recording because it looks like a shortcut and is not:

- **Real orbits break determinism.** A body on rails is a closed-form function
  of time — `position(t)` — which is §1's whole architecture. An n-body
  integration is *stateful*, must be stepped, and diverges between clients.
- **Real orbits break the player's mental model.** Gravity-affected flight in a
  game with no fuel model for station-keeping means every parked ship
  deorbits.
- **And nobody would perceive them.** Orbital motion at planetary scale is
  invisible over a session.

**[inferred] The generalisation: simulate the thing the player can perceive at
the timescale they perceive it, and use a closed form for everything else.** The
closed form is not merely cheaper — it is *seekable*, *deterministic* and
*stateless*, which are three properties a numeric integration can never have.

### 6.2 Supercruise is a scale compressor, not a travel mode

**[COMMUNITY]** Supercruise spans 29.9 km/s to ~2001c, with maximum speed
**reduced by proximity to mass** — you slow near stars and planets.

**[inferred]** Read as engineering rather than fiction, this solves the problem
that a 1:1 solar system is mostly empty and takes hours to cross. But the
gravity-well speed limit is the clever part: **it converts a rendering and
streaming constraint into a game mechanic.** Near a planet you need detail and
time to generate it; supercruise slows you exactly there. Far from anything
there is nothing to load and you go fast. The dial the engine wants and the dial
the fiction wants are the same dial.

**[COMMUNITY]** Similarly, the descent to a surface is staged through flight
zones — supercruise, orbital cruise, surface flight — each with different
handling, and the transitions "mask any loading screens during the planetary
approach". **[inferred]** A state machine whose states are chosen so that each
transition is a natural place to have finished generating the next tier. The
seam is real; it is put where a player expects a change of pace anyway.

---

## 7. Networking — what determinism buys

**[COMMUNITY]** Two layers: a central **edServer** that players connect to and
that handles matchmaking and instancing, plus **direct peer-to-peer** links
between players once they are near each other. Instances form and dissolve as
players approach and separate, "seamless & invisible", with matchmaking biased
toward geographic proximity to keep latency down, and instance sizes kept small
because P2P quality degrades with peer count.

**[inferred]** The architecture only works because of §2.4. **There is no world
state to replicate** — the galaxy, the systems, the planets and the terrain are
all pure functions both peers already have. What crosses the wire is ships,
players and events. That is a tiny payload, and it is why an MMO-shaped game of
this extent can run without authoritative world servers.

**[inferred]** The costs are equally structural and the community feels all of
them: instance sizes are small, so the "massively" in multiplayer is thin;
peer-to-peer means one player's connection degrades everyone's; and anything
that *is* shared state (markets, the political layer) needs the central server
anyway and lags behind.

**The transferable point:** *how much of your world is derivable* directly sets
*what networking topology is available to you.* That is not usually thought of
as a networking decision, and it is made — irrevocably — when the world
representation is chosen.

---

## 8. COBRA — the engine

**[FD]** Frontier's in-house engine, and the case for it is put by Jonny Watts:

> the engine "has won hands down" in internal debates because "**we pick various
> parts of the game that we want to be better than anything**."

**[FD]** It has "been in development in various iterations for **30 odd years**",
is multi-platform, and spans an implausible range of games — "Elite Dangerous,
Kinectimals, Planet Coaster, Lostwinds, Jurassic World Evolution, Wallace and
Gromit: Dog's Life". **[COMMUNITY]** Planet Coaster runs COBRA v4.

**[FD]** The demonstrated benefit they cite is turnaround: Oculus Rift support
"within weeks of DK1 headsets becoming available".

**[inferred] Watts's sentence is the whole argument for an in-house engine and
it is worth quoting at anyone who asks.** Not "our engine is better" — it is
*"we pick various parts we want to be better than anything"*. A general engine
is uniformly good; an in-house engine is deliberately lopsided, excellent
exactly where the game needs excellence and merely adequate elsewhere. Braben's
64-bit point (§4.1) is the concrete instance: it is not that UE could not be made
to do it, it is that nobody else needs it enough to have done it, so the cost
falls on you either way — and it falls more cheaply on an engine you own.

**[inferred]** That is the same argument `CLAUDE.md` makes for cromwell being
liftable across RTS/FPS/third-person: the value is in owning the parts that are
load-bearing for *your* games. Note also that COBRA's breadth across genres is
the ambition cromwell has, achieved — the same engine under a space sim and a
theme park builder, which is a wider spread than RTS/FPS/third-person.

---

## 9. What this means for cromwell

**[inferred]** Ordered by what is actionable.

**Take — techniques, with the machinery here or nearly here:**

1. **Bake to textures past a LOD threshold, from the same generator.** §3.3.
   Albedo + normal + height on undisplaced geometry beyond a distance. The
   "same generator, different output target" property is what keeps it an
   optimisation rather than a second implementation — `CLAUDE.md`'s derived-cache
   rule 2, satisfied structurally.
2. **Bake slowly-changing lighting per patch, with an explicit neighbour-sharing
   contract.** §3.4. Cache by *rate of change*, not by expense, and let the
   sharing radius be the thing that guarantees seam continuity.
3. **Deterministic placement from world position, never from observer
   position.** §3.7. Frontier shipped the observer-relative version and replaced
   it. It fails on a second observer, on revisiting, and at distance — three
   bugs, one cause.
4. **Slope-blend + triplanar + Wang tiling** for any surface that has to cover a
   lot of area from one material set. §3.5.

**Take — architecture and judgement:**

5. **Decide the precision ratio before writing the code**, not the world size.
   §4.1. The question is `largest coordinate / smallest meaningful difference`,
   and it is the *detail getting finer* that breaks you, not the world getting
   bigger. Expensive to change later in exactly `CLAUDE.md`'s sense.
6. **A pure function's coordinate frame cannot be rebased.** §3.2/§4.3. Floating
   origin and cluster partitioning fix *simulation* precision and do nothing for
   a generator's input space. Know which kind of precision problem you have
   before reaching for the standard fix.
7. **Closed form over integration for anything the player cannot perceive
   changing.** §6.1. Seekable, deterministic, stateless — three properties an
   integrator cannot have, and the reason ED's planets are on rails.
8. **How much of the world is derivable decides your networking topology.** §7.
   This is chosen when the world representation is chosen, and never revisited
   cheaply.
9. **The generator is frozen the day it ships.** §2.5. If any part of the world
   must be fixable afterwards, that part needs a diff layer, designed up front.
   [`space_engineers.md`](space_engineers.md) §4.2 is what a diff layer looks
   like when you do build one — and the presence or absence of it is the single
   biggest difference between these two games.

**Take — the counter-intuitive one:**

10. **A mature procedural system converges on authored content selected
    procedurally, not on better noise.** §3.6. Six years and a dedicated team
    moved Frontier *away* from pure generation, because noise produces variation
    without meaning. Keep determinism in the *selection*; buy the meaning in the
    *content*. The price is repetition, which is manageable; the price of the
    alternative is a world of hills that are not landforms, which is not.

**Confirmations of existing rules:**

11. **Never enumerate a radial phenomenon over a lattice.** §3.6 — Horizons'
    grid-aligned craters are the second documented instance of
    [`spatial_queries.md`](spatial_queries.md) §5.2's grid distance bias, in a
    shipped AAA game, at a completely different scale from the first. The rule
    generalises further than that note claims: **any per-cell enumeration
    inherits the cell structure, and it shows in the aggregate even when each
    sample looks fine.**
12. **In-house engines are worth it precisely where you need to be lopsided.**
    §8. Watts's framing — "we pick various parts of the game that we want to be
    better than anything" — is the argument, and COBRA spanning a space sim and
    a theme park builder is proof the breadth is achievable.

**Explicitly not to take:**

- **ED as a rendering reference beyond terrain.** §4.2, §5.1. There is no
  published information on the frame, the lighting pipeline, or depth handling.
  The terrain pipeline is well described; nothing else is.
- **Two implementations of a generator** — neither the CPU/GPU split (§3.8) nor
  the double/dual-float split (§3.2). Frontier had reasons (hardware coverage
  across a decade) that a new project does not have, and both carry a permanent
  bit-agreement burden that determinism makes non-negotiable.
- **Dense atmospheres as an increment on thin ones.** §5.3. Different problem,
  different note — [`rdr2_atmospherics.md`](rdr2_atmospherics.md).

---

## Sources

**Frontier engineers (the primary material)**
- [Generating The Universe in Elite: Dangerous](https://80.lv/articles/generating-the-universe-in-elite-dangerous) — **the single richest technical source on this game.** Doc Ross on the cube-quadtree patches, compute-shader spherification and noise graphs, the millimetre-precision problem and the double / dual-float libraries, Wang tiling and triplanar, the flat-geometry-plus-generated-textures LOD switch, per-patch lighting shared with neighbours, the 64-bit object ID and the eight-layer sector octree, prioritised resource streaming
- [Odyssey: Full Breakdown](https://elitedangerous2016.wordpress.com/odyssey-the-full-picture/) and [Dr Kay Ross tag archive](https://elitedangerous2016.wordpress.com/tag/dr-kay-ross/) — community transcription of Dr Kay Ross's planetary-tech Q&A: terrain and material types selected by Stellar Forge properties, region blending, the scatter system, procedural-plus-authored mixing, PBR, Rayleigh and Mie scattering driven by atmospheric composition; plus Jonathan Bottone on the sunset algorithm
- [Frontier on scientific authenticity and why the studio still uses its own engine](https://mcvuk.com/development-news/frontier-on-bringing-scientific-authenticity-to-games-and-why-the-studio-still-uses-its-own-engine/) — David Braben on 64-bit floats and draw distance; Jonny Watts on COBRA's 30-year lineage and the "better than anything" argument
- [Here's how Frontier rebuilt a galaxy's worth of planets for Elite Dangerous: Odyssey](https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey/) — the Odyssey terrain rework interview (**paywalled/403 to automated fetch**; cited from search summaries, not read in full)
- [Planetary Tech with Dr Kay Ross: Recap](https://forums.frontier.co.uk/threads/planetary-tech-with-dr-kay-ross-recap.565755/) and [Ask Your Planetary Tech Questions!](https://forums.frontier.co.uk/threads/ask-your-planetary-tech-questions.565493/) — Frontier's own forum Q&A (**403 to automated fetch**; content reached via the WordPress transcription above)

**Stellar Forge and the galaxy**
- [How Planets are Made in Elite: Dangerous Horizons](https://wccftech.com/planets-elite-dangerous-horizons/) — tectonic simulation informing landmasses, chemical makeup driving colour and geologic layers (**403 to fetch**; via search summary)
- [Space Adventure 'Elite: Dangerous' Simulates Milky Way in Stunning and Accurate Detail](https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html) — Stellar Forge interview
- [Stellar Forge](https://elite-dangerous.fandom.com/wiki/Stellar_Forge) — the formation simulation, rolling time back toward the Big Bang
- [Marx's guide to boxels / subsectors](https://forums.frontier.co.uk/threads/marxs-guide-to-boxels-subsectors.618286/) and [Elite:Dangerous Astrometrics](https://edastro.com/mapcharts/descriptions.html) — **[COMMUNITY]** the empirically mapped sector/boxel scheme: 1,280 ly sectors, mass codes a–h from 10 ly upward, the naming encoding

**Networking**
- [Elite Dangerous Networking & netcode](https://www.lavewiki.com/network) and [Technical Information](https://lavewiki.com/technical) — **[COMMUNITY]** edServer plus peer-to-peer, instancing, matchmaking by geography

**Flight, supercruise, landing**
- [Supercruise](https://elite-dangerous.fandom.com/wiki/Supercruise) — speed range, gravity-well slowdown
- [Planetary Landing](https://elite-dangerous.fandom.com/wiki/Planetary_Landing) and [Dev Update 22/10/2015](https://community.elitedangerous.com/en/node/323) — flight zones and the staged descent (**the Frontier update is 403 to fetch**)
- [Physics of Elite Dangerous](https://forums.frontier.co.uk/threads/physics-of-elite-dangerous.331101/) — **[COMMUNITY]** the absence of orbital mechanics

**Graphics settings, as evidence about the pipeline**
- [Elite Dangerous Graphic Settings Fully Explained](https://www.edtutorials.com/general/elite-dangerous-graphic-settings-fully-explained/) and [Terrain Work setting!?](https://forums.frontier.co.uk/threads/terrain-work-setting.583056/) — **[COMMUNITY]** the Terrain Work CPU/GPU dial, and the separate Terrain Quality / LOD / Material / Sampler settings

**Technique background (not Frontier)**
- [Depth Precision Visualized](https://www.reedbeta.com/blog/depth-precision-visualized/) — Nathan Reed on reversed-Z versus logarithmic depth, for §4.2's menu of options
- [Logarithmic Depth Buffer](https://outerra.blogspot.com/2009/08/logarithmic-z-buffer.html) — Outerra, the planetary-renderer case for it
