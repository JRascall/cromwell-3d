# Battlefield: Bad Company 2 — destruction, read from DICE's own decks

How DICE built "Destruction 2.0" on Frostbite 1.5 — what actually happens when a
wall dies, what the renderer, the physics and the network each pay, and where
the whole thing is authored rather than simulated.

The one-line answer, up front, because it reframes everything below:

> **Nothing in Bad Company 2 is ever cut. Every destructible is an authored
> entity with a small set of pre-built states, and "destruction" is the
> replicated act of stepping between them.** A wall panel swaps to its holed
> variant behind a smoke burst; a building whose destroyed-segment count
> crosses a threshold plays a canned collapse and kills through a volume, not
> through falling debris; and the one genuinely continuous system in the game
> is the terrain heightfield. The simulation budget is spent on *state*; the
> physics budget is spent on *decoration*.

That makes it the exact counterpart to
[`rainbow_six_siege.md`](rainbow_six_siege.md), where the hole in the wall is
genuinely cut at runtime by arbitrary polygons. The two games sit at opposite
ends of the same trade and arrive at several identical conclusions anyway —
§5.2 and §10 collect the convergences. The third position on the spectrum —
the world as real editable volume — is
[`voxel_terrain.md`](../../topics/world/voxel_terrain.md) / [`space_engineers.md`](../space/space_engineers.md).

> **Read alongside:** [`rainbow_six_siege.md`](rainbow_six_siege.md) — the
> procedural-cut answer, and the same "every system pays for destruction"
> lesson from the other direction.
> [`red_faction_guerrilla_destruction.md`](red_faction_guerrilla_destruction.md) —
> the third corner: pre-fracture the whole city offline and *simulate the
> connectivity*, which is the only one of the three with a real solver, and
> which converges with this note on "replicate the transition, never the
> physics". [`terrain_rendering.md`](../../topics/world/terrain_rendering.md)
> for terrain generally; §3 here is the destruction-specific slice of the same
> Frostbite material. [`vehicle_animation.md`](../../topics/animation/vehicle_animation.md) §2.5 for
> another instance of authored-states-over-simulation shipping well.

---

## Sourcing

Unusually good on the **rendering** side — DICE published constantly in the
Frostbite 1 era, and two decks were read verbatim for this note. Unusually bad
on the **gameplay/physics** side — DICE never gave a talk on how Destruction
2.0 itself works, so that half rests on interviews and on the community's
measurements, and §10 is explicit about which claims stand on what.

| tag | source | strength |
|---|---|---|
| **[DICE-S07]** | Johan Andersson, *"Terrain Rendering in Frostbite Using Procedural Shader Splatting"*, SIGGRAPH 2007 course chapter. **Downloaded and text-extracted locally; quotes are verbatim.** The primary source for §3. | **Very strong.** Working engineer's course notes with formats, budgets and shader listings. |
| **[DICE-S10]** | Robert Kihl, *"Destruction Masking in Frostbite 2 using Volume Distance Fields"*, SIGGRAPH 2010 (Advances in Real-Time Rendering). Read from a full slide transcript. Mostly about Frostbite **2**, but its history slide is the only primary statement of how Frostbite **1** masked destruction on meshes. Primary source for §4.1 and §4.3. | **Strong**, with the caveat that slide text is terse. |
| **[DICE-S09]** | Johan Andersson, *"Parallel Graphics in Frostbite — Current & Future"*, SIGGRAPH 2009 (Beyond Programmable Shading). Read from a slide transcript. Primary source for §4.4. | **Strong.** |
| **[DICE-G09]** | Andersson & Daniel Johansson, *"Shadows & Decals: D3D10 techniques from Frostbite"*, GDC 2009. Read from the authors' own abstract and secondary writeups — the deck itself was not fully extracted. | Medium. |
| **[DICE-G07]** | Andersson & Tatarchuk, *"Frostbite Rendering Architecture and Real-time Procedural Shading & Texturing Techniques"*, GDC 2007. **Abstract only — the AMD-hosted PDF is dead** and Wayback serves a corrupt copy. | Weak-medium; used for one framing claim its abstract states directly. |
| **[AUDIO]** | Anders Clerwall, *"How High Dynamic Range Audio Makes Battlefield: Bad Company Go BOOM"*, GDC 2009. | Strong for §8. |
| **[INTERVIEW]** | Karl-Magnus Troedsson (DICE GM) in Game Developer's *"How DICE fell in love with destruction"*; Alan Kertz (DICE, BF2142→BFV) and Embark staff in The Ringer's 2024 destruction retrospective. | Strong for design intent and history; no implementation detail. |
| **[COMMUNITY]** | The Battlefield wiki's *Destruction* page; **Den Kirson** (the era's respected stats miner) on building internals; the BC2 modding toolchain (`.fbrb` archives, `.dbx` object data, `.terrainheightfield` as 16-bit grayscale); MobyGames credits; PC tech press (Guru3D, NVIDIA's own tweak guide) for renderer facts. | Mixed — Den Kirson and the modding tools are effectively primary for what ships on disc; the wiki is player observation. |
| **[inferred]** | Our reading. | — |

Marketing note before anything else: **"Destruction 1.0 / 2.0" are marketing
version numbers, not architecture versions.** Bad Company (2008, Frostbite 1.0)
and Bad Company 2 (2010, Frostbite 1.5) run the same destruction architecture;
2.0 *adds* the building-collapse layer and finer chipping on top of it.

---

## 1. The one thing that matters most

Two theses, and every section below hangs off one of them.

### 1.1 Destruction is an authored state machine, not a simulation

**[COMMUNITY]** The single most load-bearing description of the system anywhere
in public is from Den Kirson, quoted by the Battlefield wiki:

> "A building is a group of entities linked together. The entities are a bunch
> of wall and roof segments. Destroy a part, the wall disappears behind a smoke
> particle and is replaced by a hole. When a percentage of the walls are
> destroyed, the building plays its collapse animation. There's little else to
> it. For example, the building behind the first MCOM on Port Valdez. Break
> about 26 of those parts, and the thing goes down."

Every word of that is doing work. *Entities linked together* — a building is
composition, not geometry. *Replaced by a hole* — a mesh swap, so the hole was
modeled by an artist, with collision to match. *Behind a smoke particle* — the
transition is hidden, the same trick every mesh-swap system uses. *A
percentage* — the structural model is a counter, not statics. *Plays its
collapse animation* — the most spectacular moment in 2010's most spectacular
destruction game is **canned**.

**[inferred]** In the vocabulary
[`rainbow_six_siege.md`](rainbow_six_siege.md) §2.1 takes from Ubisoft's deck —
procedural destruction is "a change in the state of an object generated at
runtime, where the **outcome is unique**", as opposed to pre-fragmented, which
is "pre-determined and has a fixed outcome" — **Bad Company 2 is pre-fragmented
end to end.** Shoot the same wall with the same rocket a hundred times and the
same hole appears. Nobody noticed, and nobody cared, because at Battlefield's
engagement ranges (tens to hundreds of meters, 32 players, vehicles) what
matters tactically is *that* the wall is open, not the artistic silhouette of
the opening. Siege cuts for real because its whole game happens at the 2–15 m
range where the shape of the hole *is* the gunfight.

### 1.2 Destruction's deepest cost is that it outlaws precomputation

**[DICE-G07]** The GDC 2007 architecture talk's own framing: Frostbite's design
was driven by Bad Company's requirements, "with a big focus on dynamic
memory-efficient systems and semi-procedural techniques **due to destruction
and non-linear environments**."

That sentence explains half the engine. Everything a 2007-era renderer would
normally bake offline assumes the world holds still, and Frostbite could assume
no such thing:

| Standard 2007 bake | Why destruction forbids it | What Frostbite does instead |
|---|---|---|
| Unique terrain color/mask maps (Battlefield 2 style) | can't re-author compressed textures when a crater appears | procedural shader splatting — composite materials *in the shader* from height/slope/normal + masks, §3 |
| Offline vegetation placement | the field the grass stood in is now a crater | undergrowth *generated at runtime* per 16×16 m cell, regenerated on destruction, §3.4 |
| Precomputed visibility (PVS, portals) | occluders stop existing mid-round | software occlusion rasterization every frame on SPU/CPU, §4.4 |
| Fully baked lighting | walls that cast the shadows are gone | dynamic sun + limited static contribution [inferred from constraints; see §4.4] |
| LOD meshes assuming intact geometry | every destructible needs states, not just distances | per-state meshes authored, §2.2 |

**[inferred]** This is the same table as
[`rainbow_six_siege.md`](rainbow_six_siege.md) §1, generated independently four
years earlier by a different studio: **the cost of destructible geometry is not
the destruction system, it is every other system's lost assumptions.** DICE
paid it at build-the-engine time, which is the cheap time to pay it —
L'Heureux's Siege deck says destruction "must be tackled early on", and
Frostbite is the proof by construction.

---

## 2. The destruction model — what actually happens to a wall

### 2.1 The hierarchy

**[COMMUNITY]** From Den Kirson's description plus what the modding tools
expose (`.dbx` object definitions linking part entities): a map's destructible
content is a tree —

- **Building** — a logical grouping of linked part entities plus a collapse
  state. Not itself a mesh.
- **Wall / roof segments** — the destructible unit. Each is an entity with
  health, a material class, an intact mesh, a destroyed ("holed") mesh, and
  collision for both.
- **Small destructibles** — fences, barricades, poles, trees, furniture: single
  entities with one or two states and simple removal.

**[INTERVIEW]** Kertz, on what stays standing and why: "Iron girders,
reinforced concrete, and elevated terrain would endure." **[COMMUNITY]** The
wiki adds the BF3-era formalisation: stairwells and structural cores "resist
total destruction for the sake of maintaining gameplay." And in Bad Company 1,
*interior* walls were deliberately indestructible — the wiki: internal walls
stayed standing "to preserve spacing and pacing." **[inferred]** So the
destructibility map of a level is itself level design: the designers choose the
graph of what can open, exactly as Siege's designers choose which walls are
soft. Neither game lets the simulation decide what the pacing can afford.

### 2.2 The segment lifecycle

**[COMMUNITY] [DICE-S10]** A wall segment has three visual conditions and two
real states:

1. **Intact.**
2. **Damaged but standing** — bullet scars, chipped corners, scorch. This is
   *rendering only*: decals (§4.2) and the destruction mask blending a damaged
   material over the intact mesh (§4.1). Collision unchanged. BC2's marketing
   called the finer end of this "micro-destruction" — chipping cover away bit
   by bit — and BF3 later expanded the term to bullets demolishing furniture.
3. **Breached** — the state flip. The intact mesh vanishes "behind a smoke
   particle", the holed variant appears, debris chunks fly, and — the part
   that matters — **the collision mesh swaps with the visual**, because
   players and rounds now pass through the opening. Blast weapons breach in
   one hit; enough bullet fire breaches weak materials (wood, corrugated
   metal); hand tools work too — the knife fells a tree in three hits
   **[COMMUNITY]**.

**[inferred]** Note what is *absent*: no fracture solver, no structural stress
propagation between segments, no per-segment physics until the moment debris
spawns. A segment is a hit-point pool with two meshes. That is why 32-player
servers could afford hundreds of them per map in 2010.

### 2.3 Collapse — Destruction 2.0 itself

**[COMMUNITY]** The mechanism, assembled from Den Kirson and the wiki's
unusually precise observation:

- Each building counts its destroyed segments. Cross the authored threshold —
  "about 26 of those parts" for the Port Valdez example — and the collapse
  **plays**. It is an authored sequence: "all roof segments will cave in; most
  of the wall segments will disappear, although some on the ground floor may
  remain standing."
- The lethality is **a volume, not the debris**: "the collapsing building has
  a hitbox which kills any opponents who go inside or are very close; this
  hitbox goes numerous feet around the building; almost like an invisible
  perimeter fence." Players die "even if no visible debris has fallen on
  them"; players *on the roof* die; "even jumping a split-second before the
  building plays the proper collapse sequence will generally still kill the
  player." The kill feed credits **"Destruction 2.0"** — the marketing term is
  literally the weapon name.
- The special case proves the rule: **watchtowers**. Under one, you're crushed
  and the demolisher gets the kill; *on top* of one, you fall with it and die
  as a "suicide", no credit. Two different authored outcomes for two positions
  relative to the same object — a design table, not an emergent physics
  result.
- The aftermath is real gameplay space: rubble with collision, navigable "by
  crouching or jumping through small paths formed by the debris."
  **[INTERVIEW]** Kertz: "There's got to be craters, there's got to be
  negative space, there's got to be rubble leftovers… There's still places for
  players to hide." Wreckage is not cleanup — it is the *replacement cover*,
  which is what keeps a fully-leveled map playable in the last ten minutes of
  a round.

**[inferred]** Read as engineering, the kill volume is the correct decision,
not a shortcut. Resolving crush deaths against tumbling debris would be
nondeterministic across clients, unfair at 100 ms ping, and expensive — and
the *rule* the players learn ("get out of the building that is falling on
you") is identical either way. The volume resolves the gameplay in the cheap,
authoritative representation and lets the expensive representation be purely
visual. It is the same split Siege makes between the cut hole (gameplay) and
the instanced debris (decoration) — see §5.2.

### 2.4 What the collapse is for

**[COMMUNITY]** Collapse is wired into the objective game, not just the murder
game. In Rush, an M-COM station inside a building is destroyed if the building
comes down on it — flattening the objective is a legitimate (and notorious)
attacking strategy. The scoring system treats the mechanism as a first-class
actor: collapsing a building onto teammates is **the only way to team-kill in
a non-hardcore match** (−100 per teammate, −250 for crushing your own
objective). And the coverage is authored per map: Laguna Presa ships no
collapsible structure at all, and the Vietnam expansion's huts are back to
1943-level destruction — **[inferred]** collapse is expensive authored
content, and where the fiction doesn't support concrete buildings, DICE simply
spent the budget elsewhere.

---

## 3. Terrain — the one continuous system, verbatim from the source

Terrain is where the "everything is authored states" rule stops. Heightfield
craters are genuinely arbitrary: any shell, anywhere, deforms the ground.
This is the system DICE documented best — [DICE-S07] throughout, quotes
verbatim.

### 3.1 The ambition predates the engine

> "Our main problem is that they are static. We have wanted to be able to
> destroy the terrain ever since Battlefield 1942, both geometrically and
> texture-wise, but haven't had the performance or memory to support arbitrary
> geometric destruction of the heightfields."

Six years of wanting, and the blocker was **texturing, not geometry** —
Battlefield 2's baked color maps and detail masks could not be re-authored at
runtime when a crater appeared. Procedural shader splatting (compositing
terrain materials in the shader from height / slope / normal plus masks)
exists substantially *because of destruction*: when the compositing happens
per-pixel at runtime, changing the inputs changes the ground.

### 3.2 A crater is two edits

When a ground destruction event fires:

1. **The heightfield pixels around the crater are updated** — the actual
   16-bit heightfield textures the terrain renders and collides from. The
   crater is *real*: it blocks LOS, it is cover, vehicles drive into it.
2. **A crater decal is rendered into the destruction mask** — a separate
   low-res mask texture that tells the terrain shader to blend in destroyed
   materials ("burnt dirt", cracked tarmac) over the crater area.

### 3.3 The destruction mask — a sparse texture budgeted by honesty

The numbers, verbatim: the mask is **4 pixels per meter**, "because the mask
only needs to contain rough circular gradients" — detail is added back in the
shader by the same detail-mask tricks as every other terrain material. But
even at that resolution, a 2048×2048 m destructible area costs
(2048×4)² = **64 MB** uncompressed — "hardly desirable on any platform."

The fix is a budget argument worth quoting in full:

> "The worst case scenario for ground destruction is not really that 100% of
> the terrain area can be fully destroyed and need to be masked at the same
> time. The percentage we can get away with is much lower, perhaps 10%. But we
> do not want to restrict where on the terrain the destruction can happen, so
> the 10% destroyed area can be arbitrarily scattered over the entire
> terrain."

So: a **dynamic sparse tile atlas**. A fixed-grid indirection texture maps
terrain area → atlas tile; tiles are allocated on demand when a crater touches
unallocated ground; the CPU copy of the indirection table is updated and
re-uploaded; and each crater is drawn as "a small 2D texture-mapped decal that
is rendered into the destruction mask texture atlas tiles by setting the
viewport to match the tile." Few tiles change per frame, but the *accumulated*
crater count over a round can be high — the incremental update is the win.

**[inferred]** This is CLAUDE.md's derived-cache discipline in 2007 clothing:
cap the budget by the honest worst case (10% scattered anywhere, not 100%),
keep the authoritative data (heightfield) separate from the derived
presentation (mask), and pay updates incrementally at the mutation boundary.

### 3.4 What else re-derives when the ground moves

Every derived structure over the heightfield re-derives, and the deck names
each one:

- **Culling:** the terrain quadtree "every node knows the maximum and minimum
  height… The minimum height of a node may change when the heightfield is
  altered by ground destruction." Mutation dirties the acceleration structure
  — no way to forget.
- **Geometry LOD:** every visible leaf renders as a fixed **33×33 vertex
  grid** fetching height in the vertex shader. Verbatim: "The fixed vertex
  grid resolution is important to be able to support the worst case scenario
  with arbitrary ground destruction at a fixed cost and quality. This 'wastes'
  triangles in non-altered flat areas but we found the cost to be worthwhile
  because of the simplicity and generality of this approach." — **a fixed
  cost bought deliberately with waste**, so a shell landing anywhere never
  spikes the frame.
- **Undergrowth:** grass/stones/debris instances are generated at runtime per
  16×16 m cell (top-down render of material masks → CPU/SPU scan → jittered
  placement), precisely so that "areas affected by ground destruction" can be
  regenerated — the grass dies when the crater appears, and burnt-dirt debris
  can grow in its place *through the same material system*.

### 3.5 The best networking fact in the whole note

Verbatim, from the undergrowth section:

> "To get deterministic results when generating pseudo-random numbers within a
> cell, the cell position in the grid structure is hashed and used as a seed.
> This is important both on the local client when regenerating cells but also
> when running multiple clients of the network so that everybody sees the same
> geometry."

**Replicate the cause, regenerate the effect deterministically.** The server
never sends a blade of grass; it sends the crater, and every client's
regeneration agrees because the seed is a pure function of position. This is
the only place in the public record where DICE states the destruction
networking philosophy outright, and it generalises: §7.

---

## 4. Rendering the damage

### 4.1 The damaged look is a mask, not a mesh

**[DICE-S10]** Kihl's history slide is the entire Frostbite-1 mesh story in
four lines, verbatim:

> "Used in BFBC, BF1943 and BFBC2. Good visual quality. Time consuming
> workflow: main issue to address. Requires UV mapping mask for each
> destructable part."

So on meshes, exactly as on terrain, **damage-short-of-breach is a mask
blending a destroyed material over the intact one** — scorch, exposed brick,
crumbled plaster — evaluated in the surface shader. The mask lives in a UV
space that an artist had to author **per destructible part**, and that
workflow line item ("main issue to address") is worth pausing on: the visual
quality was fine, the runtime was fine, and the thing that hurt at Battlefield
scale — hundreds of destructible parts per map — was *artist hours per part*.

### 4.2 Decals — generated on the GPU, from the visual mesh

**[DICE-G09]** The GDC 2009 decal technique carries the bullet-scar /
micro-destruction layer: decal geometry is extracted **on the GPU** with the
geometry shader + stream-out — cull and transform the receiving triangles into
a decal buffer, clip, and reuse — instead of duplicating vertex/index buffers
into system memory for CPU projection. Two details matter for destruction:

- The deck explicitly rejects generating decals from **physics collision
  meshes** and extracts from the **visual mesh** — collision proxies are too
  coarse for a scar to sit on convincingly. A system that swaps collision per
  destruction state (§2.2) has *guaranteed* visual/collision divergence, so
  the decals must follow the visuals.
- Stream-out means the projection cost is paid **once per decal creation**,
  then the buffer redraws for free — the right shape for damage that only
  accumulates. (Same conclusion as Siege's cut-once-render-forever holes.)

### 4.3 The successor diff — what Frostbite 2 changed and why

**[DICE-S10]** Kihl's actual talk is the fix for §4.1's workflow problem, and
even though it shipped in BF3, it is the sharpest lens on what BC2's approach
cost. The Frostbite 2 destruction mask:

- **Spheres, not UVs.** Artists (or tools) place spheres on the geometry where
  destruction can appear; a **signed distance field** is computed from the
  sphere group and stored in a **volume texture at ~2 m/pixel**, one per
  masked geometry, packed into atlases (Xbox 360 requiring volume dimensions
  in multiples of 32×32×4, hence "one atlas per dimension" and padding
  against border leakage). High-frequency edge detail is added by combining
  the coarse field with a tiled detail texture:
  `distanceField += detail * g_detailInfluence` then a scale-bias into an
  opacity. **The mask moved from per-part UV space into world space, and the
  per-part authoring evaporated** — that is the whole point of the change.
- Where FB1 drew the mask with the geometry, FB2 (now a deferred renderer)
  can draw it as a **deferred decal**: render a convex volume, reconstruct
  local position from depth, sample the distance field, blend onto the
  G-buffer — with the attendant pain Kihl lists honestly (fixed-function
  blending fighting the G-buffer layout — "good use case for programmable
  blending"; normals in the G-buffer already containing normal-map detail, so
  a material index is written instead for a tangent-space lookup; mip
  selection artifacts at volume borders forcing explicit `tex2Dlod`).
- The PS3 perf table for one masked scene, ms per technique: forward with
  mask **0.77**; forward with `[branch]` around the mask **0.45** (dynamic
  branching costs 6 cycles on RSX and needs 800–1600-pixel coherent
  segments); deferred volume **0.31**; and **0.20** with SPU triangle culling
  — each triangle tested against the distance field, two index buffers
  emitted (masked/unmasked), *cached until the distance field changes*. Cache
  the classification at mutation time, not per frame — the same
  invalidate-at-the-boundary rule again.

### 4.4 The renderer around the destruction

- **[COMMUNITY]** Frostbite 1.5 on PC is a **forward renderer** with DX9/10/11
  paths (DX10 the design baseline; DX11 adds soft shadow filtering and runs
  fastest; HBAO on 10/11) — the deferred switch is Frostbite 2 [DICE-S10
  slide 7: "Frostbite 2 uses deferred rendering"]. Destruction's fingerprints
  on the feature list are the **particle systems "optimized for huge amounts
  of particles" with soft blending** — dust and smoke are the destruction
  renderer's biggest consumable, both as spectacle and as the curtain over
  every mesh swap (§2.2's "smoke particle").
- **[DICE-S09]** Visibility is computed, not baked: Frostbite rasterizes "a
  coarse zbuffer on SPU/CPU, 256×114 float" from "low-poly occluder meshes,
  manually conservative", capped at 10,000 occluder vertices/frame, costing
  "a few milliseconds" in parallel SPU jobs, then tests all objects against
  it "before passed to all other systems = big savings." **[inferred]** A PVS
  or portal bake was never an option — §1.2 — and the occluder meshes for
  destructible buildings must be conservative against their *destroyed*
  states or drop out with them; the deck doesn't say which (§10). Siege hit
  the identical issue from the other side: "poking holes degrades occlusion
  efficiency" ([`rainbow_six_siege.md`](rainbow_six_siege.md) §4.9).
- **[DICE-S09]** The frame is job-based throughout — the deck's own list:
  "terrain geometry processing, undergrowth generation, decal projection,
  particle simulation, frustum culling, occlusion culling, occlusion
  rasterization, command buffer generation, PS3: triangle culling." Every
  destruction-adjacent system on that list is there because it now runs at
  runtime instead of in the bake.

---

## 5. Physics and collision

### 5.1 What Havok does and does not do

**[COMMUNITY]** Bad Company 2 ships **Havok** as its physics middleware
(MobyGames credits; carried into BF3-era Frostbite). Rigid bodies — vehicles,
ragdolls, debris, physicalized furniture ("furniture can be destroyed by
pushing it into other objects" is a wiki-documented Frostbite constant) — are
Havok's job.

**[inferred, but the evidence is consistent]** What Havok does *not* do is the
destruction itself. Segment breach is a health threshold; collapse is a
triggered animation; crush death is a volume test. Nothing in the public
record or the observable game suggests a structural solver, and everything —
identical collapse per building, the perimeter kill volume, roof players dying
without debris contact — suggests its absence. Havok decorates the state
machine's transitions; it never decides them.

### 5.2 Debris is decoration — the third shipped game to agree

**[COMMUNITY]** Breach debris and collapse rubble-in-flight behave as short-
lived local rigid bodies: they bounce outward with plausible spin (player
forums noted rotation consistent with impulse application at the impact
point), interact with nothing that matters, and fade. Kill credit never
touches them (§2.3 — the volume does the killing).

**[inferred]** Line this up with
[`rainbow_six_siege.md`](rainbow_six_siege.md) §2.7 — debris pre-made,
instanced, box-collided, recycled, never authoritative — and with
[`nuclear_option_damage.md`](../flight/nuclear_option/nuclear_option_damage.md)'s blast model damaging
through a wavefront rather than simulated fragments. Three games, three eras,
one conclusion: **spend simulation on what the player decides around (the
hole, the state, the crater) and nothing on the fragments, which no player
decision ever touches.** BC2 is arguably the purest case, since even its
*collapse* is on the decoration side of the ledger.

### 5.3 Collision follows state

**[COMMUNITY] [inferred]** The parts of collision that are load-bearing:

- **Per-state collision meshes.** A breached wall must pass players, bullets
  and grenades through the hole and stop them at the jamb — so the collision
  swap is simultaneous with the visual swap. (This is also *why* §4.2's decal
  system refuses to use collision meshes as decal receivers: they are proxies,
  and they change.)
- **Rubble is real.** Post-collapse debris fields have collision players
  crouch and jump through — authored rubble meshes, part of the collapse
  state, not settled simulation output.
- **Terrain collision is the heightfield** — the same data the renderer
  fetches, so a crater is instantly and consistently cover, concealment, and
  a vehicle trap with no second representation to update. One authority,
  many consumers: the cleanest instance of the pattern in the whole game.

---

## 6. Gameplay — what destruction is *for*

**[INTERVIEW]** The design history in the principals' own words:

- Differentiation: "One of the big things was how do you differentiate
  yourself in, at that time, a pretty crowded first-person shooter space"
  (Kertz). And the promise that drove the tech: "When you have vehicles, it's
  quite unsatisfying to drive your 60-ton tank into a tree and the tree just
  stands there and says no."
- The rule the whole game answers to: **"If the player shoots at it,
  something should happen."** Note it does not say *the right thing* should
  happen — §1.1's canned outcomes satisfy it fully. The rule is about
  responsiveness, not fidelity.
- The discovery it worked: Troedsson on the first playtest — "almost like we
  opened Pandora's Box." And the anxiety before it: destruction threatened
  Battlefield's core spacing until playtests showed it enhanced it.

**[COMMUNITY] [inferred]** What Destruction 2.0 does to the moment-to-moment
game, assembled from a decade of the community relitigating it:

1. **Cover is a consumable.** The wall you defend from has hit points; the
   longer a position holds, the less of it remains. Camping decays
   structurally, without a designer timer.
2. **The map's cover budget is roughly conserved.** Walls leave as rubble and
   craters arrive — Kertz's "negative space" line is the design articulation.
   The late-round map is *different*, not *empty*, which is the difference
   between destruction as a system and destruction as an effect.
3. **Occupancy is bounded by structure.** A building is only as safe as its
   segment count, and the collapse threshold converts "they're fortified
   inside" into "bring rockets." The wiki's blunt phrasing: players "couldn't
   camp for very long", and defenders in Rush are forced to play forward of
   the objective building rather than inside it.
4. **The objective game is physical.** M-COMs die under rubble; the C4-the-
   building play is a legitimate alternative to arming the charge. Destruction
   isn't beside the mode; it's a verb *in* the mode.
5. **And it is fenced where it must be** — indestructible girders, cores and
   elevated terrain (§2.1), maps that opt out entirely (§2.4). The authored
   state machine makes the fence trivial to build: a segment you must not
   lose is simply a segment that doesn't exist.

---

## 7. Networking

**[INTERVIEW]** Kertz, verbatim, on the cost profile:

> "A lot of games are a lot about receiving data, but when you're impacting
> the world, you're sending that information to the server and then to other
> clients. So our bandwidth requirements were substantially higher than other
> shooters."

And on relevancy: the netcode builds "a bubble around a player" — "everything
within 25 meters is super important, send that data."

**[DICE-S07 + inferred]** The architecture that keeps the bandwidth *merely*
"substantially higher" rather than impossible is §3.5's principle scaled up:
**replicate causes and state transitions, never results.** A breach is an
entity-state flip — a few bytes. A crater is an event whose heightfield edit,
mask decal and undergrowth regeneration every client re-derives identically
(the hashed-seed quote is the primary evidence the philosophy was explicit).
A collapse is one trigger for an animation every client owns on disc. Debris
is not replicated at all, because nothing reads it (§5.2). The state
accumulates server-side over the round — which is also what makes
late-joining a half-destroyed server cheap: the join payload is the set of
flipped states and craters, not geometry.

**[inferred]** Contrast Siege §3, which must replicate *procedural* outcomes
and therefore ships symmetrical compression and impact-seeded RNG so every
client cuts the same unique hole. BC2 never needs the machinery: when
outcomes are authored, "which state" is the whole message. That is the
networking dividend of pre-fragmented destruction, and it is large.

---

## 8. Audio, briefly

**[AUDIO]** The destruction soundscape is what forced DICE's **High Dynamic
Range Audio** (Clerwall, GDC 2009): with 155 mm shells, collapsing buildings
and footsteps in one mix, static mixing is hopeless, so Frostbite mixes by
loudness window — real-world-scaled sound levels, and the loudest events
duck everything below the window. The talk's pitch is workflow ("transformed
the usually tedious… act of mixing the game into an enjoyable play-through"),
but the enabling observation is destruction's: a game where any wall can
become a 130 dB event at any moment cannot be mixed by hand. BC2 does *not*
attempt Siege-style propagation-through-holes ([`rainbow_six_siege.md`](rainbow_six_siege.md)
§6) — outdoors at Battlefield ranges, the loudness problem dominates the
occlusion problem.

---

## 9. What transfers to this project

Ranked. The frame: this project's world is a tile grid with derived caches
(`OcclusionGrid`, `ReachField`), an entity layer with a one-field
`DestructibleComponent` (wreck stamping), and demolition already dirtying the
occlusion grid through `World::at()`. BC2 is unusually close to home — closer
than Siege — because a tile world is *already* pre-fragmented.

1. **Destruction as authored states, and be at peace with it.** The most
   spectacular destruction game of its generation never cuts geometry. For a
   tile game the lesson is direct: a destructible prop needs intact /
   damaged / destroyed states and honest transitions, not a fracture system.
   The existing `DestructibleComponent` + wreck-stamping design is BC2's §2.3
   aftermath rule (rubble becomes cover, all systems pick it up with no
   special case) already in miniature — extend it with states before ever
   considering geometry.
2. **Resolve gameplay in the cheap authoritative representation; decorate
   with the expensive one.** The crush volume, not the debris; the state
   flip, not the fragments. For us: destruction outcomes resolve on the grid
   (occupancy, cover, LOS), and any visual tumble is fire-and-forget. Never
   let a visual fragment carry a gameplay consequence.
3. **Threshold + authored sequence for compound structures.** "N of M
   segments destroyed → the whole collapses, killing occupants" is a potent
   mechanic and costs a counter plus authored content. If multi-tile
   buildings ever matter here, this is the shape — with BC2's warning that
   the kill region must read fair (§2.3's "invisible perimeter fence" is the
   wiki's phrasing of a player grievance).
4. **Damage-short-of-destruction is a mask, not geometry** — and budget it
   the DICE way. A scorch/rubble overlay splatted into a sparse tile atlas
   sized for the honest worst case ("10%, scattered anywhere") with
   incremental updates is exactly how this renderer should show accumulated
   battle damage. §3.3 is a complete, numbers-included blueprint.
5. **Replicate causes, regenerate results from position-hashed seeds.** If
   this project ever networks, §3.5 is the sentence to build on. It also
   pays offline: deterministic regeneration means saves store events, not
   world snapshots.
6. **Fixed-cost worst case beats adaptive cleverness where destruction is
   arbitrary.** The 33×33 grids "waste" triangles so a shell landing anywhere
   costs the same. The project's fixed-grid spatial layers already follow
   this instinct; keep following it for anything destruction can touch.
7. **Invalidate derived data at the mutation boundary** — quadtree min-heights
   dirty when the heightfield changes; FB2's triangle-culling cache refreshes
   when the distance field changes. Identical to `World::at()` dirtying the
   occlusion grid. BC2 is a second shipped proof that this is the pattern
   that survives contact with destruction.
8. **Author the indestructible.** Girders, cores, opt-out maps: pacing is
   protected by *choosing* what can never open. In a tactics game this is
   doubly true — a map's teaching moments depend on some cover being
   load-bearing for the design.

---

## 10. What was not established

Stated plainly, in the folder's tradition:

- **DICE never published on Destruction 2.0's own mechanics.** The published
  decks are about *masking* (rendering), terrain, parallelism, shadows,
  decals and audio. §2's model — segments, thresholds, canned collapse, kill
  volume — rests on Den Kirson (a data miner of shipped files, effectively
  primary for on-disc facts but not for engine internals) and on player
  observation. It is consistent and repeatedly confirmed by behaviour, but no
  DICE engineer has vouched for it in public.
- **"~26 parts" is one building on one map**, quoted as an example — the
  per-building thresholds across maps were never mined out publicly.
- **Havok's exact perimeter is unverified** — credits confirm its presence,
  not which systems route through it (vehicle sim certainly; whether debris
  uses full Havok bodies or a cheaper particle path is unknown).
- **Whether occluder meshes track destruction state** (§4.4) is our inference
  from necessity; the SIGGRAPH 09 deck doesn't address it.
- **The BC2 network protocol was not read.** §7's cause-replication model is
  one verbatim primary quote (undergrowth seeding), two interview quotes, and
  inference — the actual message formats live in the server binaries and were
  not examined.
- **The GDC 2007 and GDC 2008 architecture decks were not fully read** (dead
  AMD links, corrupt Wayback copies); [DICE-G07] rests on the talk's own
  abstract.
- **No frame-time budget for destruction exists anywhere public** — unlike
  Siege, where L'Heureux published benchmark tables. BC2's costs are only
  visible in their consequences (the fixed-grid decisions, the sparse
  atlases, the SPU job lists).
