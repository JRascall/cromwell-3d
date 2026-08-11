# Space Engineers 1 and 2 — reference notes

How Keen Software House build a game where **the world is made of editable
volume all the way down** — blocks on a grid, voxels in a planet, both
destructible, both simulated, both rendered. SE2 is the primary reference
because it is the *rewrite*: a new engine (VRAGE3) with a decade of hindsight
about what VRAGE2 got wrong, and the single most interesting decision in it is
the one the user asked about first — **SE1 had two grids and SE2 has one**.

This note covers the grid, physics, planets and voxels, rendering, and the
automation layer, in that order. Navigation is deliberately thin here because it
already has its own note — [`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md)
§4 reads SE1's two pathfinding systems from the released source, and §8 below
only adds what has surfaced since.

> **Read alongside:** [`voxel_terrain.md`](../../topics/world/voxel_terrain.md) — the layer below §4
> of this note, and the deeper read of the same source: the two meshers, the
> clipmap and its geomorph, editing and collision, triplanar materials, and the
> two GPU Gems chapters this subject is usually approached through. §4 here is
> the architecture; that note is the pipeline.
> [`elite_dangerous.md`](elite_dangerous.md) — the other
> solar-system-scale game in this folder, and the instructive contrast. SE
> stores a **diff** against a procedural generator so the player can dig; ED
> stores nothing at all and its terrain is therefore not editable. That one
> difference explains almost everything else about both. Its §4.3 reads the two
> large-world precision answers against each other directly.
> [`rainbow_six_siege.md`](../shooters/rainbow_six_siege.md) for the destruction comparison —
> Siege cuts planar surfaces in 2D and fakes every fragment, SE fractures in 3D
> with Havok and subtracts SDF spheres from a density field. Two very different
> answers, and Siege's is the better-documented one by a distance.
> [`re_engine_rendering.md`](../rendering/re_engine_rendering.md) for the
> caches-and-cheap-repair architecture, which SE's voxel storage independently
> arrives at; [`source2_rendering.md`](../valve/source2_rendering.md) §13 for clustered
> lighting, which VRAGE3 also uses; [`navigation.md`](../../topics/agents/navigation.md) for why
> spatial queries and pathfinding are two systems.

---

## Sourcing, and the honest caveat

The two games are sourced *very* differently and it would be dishonest to
present them at one confidence level.

| tag | source | strength |
|---|---|---|
| **[SRC]** | **SE1's released C# source** on GitHub — class names, constants, structures read directly | **Primary.** The strongest material in this note. |
| **[KEEN]** | Keen's own dev blogs — Marek Rosa's diaries, Jan Hloušek's VRAGE3 engineering post | Strong for *what*, weak for *how much*. Almost no millisecond figures anywhere. |
| **[ENG]** | A named Keen engineer's own write-up — Landon Townsend on SE2 rendering | Strong. First-person technical, but a portfolio post, not a talk with numbers. |
| **[WIKI]** | Official SE / SE2 wikis (wiki.gg) | Descriptive. Accurate about behaviour, silent about implementation. |
| **[COMMUNITY]** | Modding guides, support forums, player measurement | Weakest. Useful for numbers Keen never published; verify before depending on. |
| **[inferred]** | Our reading. Not anybody's word. | — |

**The caveat that matters:** SE1's source was public for about eighteen months
from May 2015 and is still on GitHub, so everything in §2.1, §3.3, §4.1 and §7.2
is read from real code. **SE2 has published no equivalent.** There is no VRAGE3
talk, no GDC session, no slide deck with costs. What exists is dev diaries,
patch notes, a marketing page and one engineer's portfolio.

So: **where this note says SE2 does X, that is Keen's claim, not a measurement**,
and where it explains *how* SE2 probably does X, it is marked **[inferred]** and
you should treat it as our reasoning from SE1's code plus the constraints. The
one thing that is not guesswork is the *shape* of the decisions, because Keen
have been explicit about which VRAGE2 problems VRAGE3 was written to fix.

**[KEEN]** VRAGE3 development began **January 2022**, after evaluating and
rejecting Unreal Engine 5, Unity 3D, UNIGINE, Stride and Flax on the grounds
that they "could not support unique use cases without major rewrites", and
"most of the systems are implemented from scratch, with very little code being
reused from VRAGE2." SE2 entered Steam Early Access January 2025; planets landed
December 2025 (VS2), mechanical blocks and weapons March 2026 (VS2.2),
automation and wheels July 2026 (VS2.3).

---

## 1. The one thing that matters most

**Both games are built on the same bet: every unit of the world is an editable
cell with authoritative state, and everything else — rendering, physics,
gameplay systems — is derived from it and must survive it changing at runtime.**

That is exactly this project's bet too, at a different scale, and it is why SE
is worth reading closely even though the genre has nothing in common with a
tactics game. The three consequences fall out identically:

1. **Nothing can be baked, only incrementally re-baked.** A ship's collision
   shape, a planet's mesh, a conveyor network's connectivity — all derived, all
   invalidated by a single cell edit. RE ENGINE arrived at the same place from
   the opposite direction ([`re_engine_rendering.md`](../rendering/re_engine_rendering.md)
   §1) and it is the same architecture.
2. **The authoritative representation must be cheap per cell**, because there
   are a great many cells and the player can touch any of them.
3. **The derived representations must be cheap to repair locally**, because a
   full rebuild is unaffordable at the rate edits arrive.

**[inferred]** SE1 gets (1) and (3) right for voxels and wrong for grids — §4.2
versus §3.2 — and SE2's rewrite is largely the story of fixing the grid side.

---

## 2. The grid — SE1's two, and SE2's one

### 2.1 SE1: two grids, and they cannot mix

**[WIKI]** SE1 has exactly two block sizes and they live on **separate,
incompatible grids**:

| | Large grid | Small grid |
|---|---|---|
| Cell edge | **2.5 m** | **0.5 m** |
| Volume ratio | 1 | 1/125 |
| Intended for | stations, capital ships, interiors | fighters, rovers, detail |

> **[WIKI]** "Each grid can consist only of blocks of a matching block size, and
> it's completely impossible to have a single grid having both small and large
> blocks."

**[COMMUNITY]** Keen's stated intent was that small grid would build small craft
"which lack interiors" and large grid would build everything with a walkable
inside. Mixing was, in their words, discouraged because it "creates issues for
the engine."

**[SRC]** The storage is a hash map keyed by integer cell:

```
MyCubeGrid:  private readonly Dictionary<Vector3I, MyCube> m_cubes  // capacity 1024
```

That is worth pausing on given this project's hot-loop rules. **A dictionary
lookup per cell is exactly the access pattern `CLAUDE.md` forbids inside a
loop** — and SE1 gets away with it because a grid is *sparse* (a ship is mostly
not-a-block) and because the per-frame consumers of grid data are systems that
walk their own cached structures, not code that re-queries the grid per cell.
The lesson is not "hash maps are fine"; it is that **SE chose sparsity over
indexability**, and then had to build a separate derived structure for every
query that wanted density. §3.2 and §7.2 are both those structures.

### 2.2 What the two-grid split actually cost

**[inferred]** The split was not a rendering or storage decision. It was a
*physics* decision that leaked into everything, and the bill came in four parts:

1. **You cannot detail a large ship.** A 2.5 m cell is the smallest thing you
   can attach to a capital ship. Every railing, pipe, light fitting and console
   is either a whole 2.5 m block or does not exist.
2. **The workaround is a second rigid body.** **[WIKI]** The only way to mix
   sizes is a **subgrid** — a separate grid connected through a rotor, hinge,
   piston or connector. That is not a modelling trick; it is a genuine second
   Havok body joined by a genuine constraint, with all the cost and instability
   that implies (§3.4).
3. **So detailing a ship makes it physically worse.** Every decorative small-grid
   addition is another constrained body in the solver. The game's own visual
   ambition fights its own simulation stability.
4. **[SRC]** And the engine grew machinery purely to paper over it —
   `MyCubeGridSmallToLargeConnection` exists solely to track small blocks
   touching large ones and keep grid groups consistent across server and client.

**[inferred] This is the clearest "one lattice or two" cautionary tale available
in a shipped game, and the answer it gives is: one.** Two granularities in one
world do not stay a content decision. They become a physics decision, a
networking decision, and eventually a class in the codebase whose only job is to
reconcile them.

### 2.3 SE2: the 25 cm unified grid

**[KEEN]** SE2's headline feature, in Keen's own phrasing, is a "new 25cm
unified grid system (**no more small vs. large grids!**)".

**[WIKI]** The mechanics as documented:

> "The placement grid has 25cm (~10 inches) granularity and supports building
> block sizes spanning from 25 centimeters, through 0.5 meters, 2.5 meters, and
> so on, **with no upper bound**."

and:

> "a 2.5-meter slope only occupies half of the cubic space, leaving the other
> half available for further construction."

**[WIKI]** Snapping adapts to the block being placed, but can always be
overridden down to the 25 cm quantum.

The numbers line up cleanly with SE1's:

| | SE1 large | SE1 small | SE2 quantum |
|---|---|---|---|
| Edge | 2.5 m | 0.5 m | **0.25 m** |
| In SE2 quanta | 10³ = **1000 cells** | 2³ = **8 cells** | 1 |

**[inferred]** So SE2 did not split the difference — it went **2× finer than
SE1's small grid and 10× finer per axis than SE1's large grid**, and made all
of it one address space. A 2.5 m block is not "a large block" any more; it is a
block that happens to be 10 quanta on a side.

**[inferred]** The corroborating detail is in the block naming. VS2.3 shipped
wheels at "100, 150, 250, 500, 750 and 1250", and merge blocks at "250 and 50" —
those are **centimetres**, i.e. 1 m through 12.5 m wheels and 2.5 m / 0.5 m
merge blocks. Block size is a *number* in SE2, not a *category*, and the UI
reflects it. That is the unification made visible.

**[KEEN]** And it is not only the build grid. The April 2026 water simulation
milestone was that water "now flows through **25 cm holes**" — so 25 cm is the
quantum for a *volumetric simulation* too, not just for placement. **[inferred]**
That is a strong hint that 25 cm is the engine's universal spatial unit rather
than a build-mode convenience.

### 2.4 How the unified grid is probably stored — and why it cannot be dense

**This is the part Keen have not published**, so what follows is reasoning from
the constraints. **[inferred]** throughout.

The naive reading of "25 cm grid" is a dense array at 25 cm. That is
immediately unaffordable, and the arithmetic is not close:

- A modest SE1 large-grid ship of 50 × 20 × 20 large cells is 20,000 cells.
- The same volume at 25 cm is 500 × 200 × 200 = **20,000,000 cells**.
- One byte per cell is 20 MB for a *small* ship, and SE2 has no upper bound on
  block size, so it has no upper bound on grid extent either.

So the storage is certainly **sparse and keyed by block, not by cell**: a block
records its origin in 25 cm units and its size, and the grid holds a collection
of those. That is SE1's `Dictionary<Vector3I, MyCube>` idea with the key
reinterpreted into finer units — and it is the only shape that survives "no
upper bound".

But sparse-by-block breaks the query that matters most: **"is this 25 cm cell
occupied, and by what?"** — asked by placement validation, by connectivity, by
the conveyor and power graphs, by damage, by the water simulation. Answering it
by scanning blocks is a linear scan, which is the other thing `CLAUDE.md`
forbids. So there is necessarily a **derived occupancy index** over the sparse
block set, and the interesting question is what shape it is:

| Candidate | Why it fits | Why it might not |
|---|---|---|
| **Hash of occupied 25 cm cells → block id** | Direct, simple, matches SE1's idiom | 1000 entries per 2.5 m block — the map is 1000× the size of the block list |
| **Coarse cell → block list, refine within** | Two-level: a 2.5 m "chunk" holding the few blocks overlapping it, tested exactly | Two lookups, but the second is over a handful of candidates. **Most likely.** |
| **Sparse octree** | Natural for unbounded extent and mixed sizes | Depth cost on every query; SE already uses octrees for *voxels* (§4.2) so the machinery exists |

**[inferred] The two-level version is the one we would bet on**, because it is
what the rest of the engine already wants. The physics shape (§3.2), the render
batching, and the conveyor graph all operate at ship-region granularity, not at
25 cm — so a chunk layer has to exist anyway for those, and hanging occupancy
off it is free. It is also exactly the pattern this project already uses:
`OccupancyGrid` is a coarse derived summary over authoritative `Tile` data, with
a fall-through for cells the summary cannot decide.

**Do not take this as fact.** It is the reasoning, and it is offered because the
reasoning transfers even if Keen's answer differs.

### 2.5 Grid splitting — the cost nobody budgets for

**[SRC]** SE1's `IMyCubeGrid` exposes the operation that gives the game away:

> a check for whether **removing a block will cause the grid to split**, plus
> `SplitBlocks` handling for when it does.

When a ship is cut in half, the engine must **detect that the block set is no
longer connected, partition it, and spawn a second grid** — with its own rigid
body, its own inertia tensor, its own conveyor and power graphs, its own network
identity. That is a connected-components problem over the block set, run at
runtime, potentially per weapon hit.

**[inferred]** Two consequences, and both get worse at 25 cm:

1. **Connectivity must be maintained incrementally, not recomputed.** A flood
   fill over a capital ship per hit is not survivable. The practical shape is:
   maintain per-block neighbour links on placement/removal (SE1's
   `RefreshBlockNeighbors` does exactly this), and only run the partition when a
   removal actually breaks a link that had no alternative path.
2. **The 25 cm grid multiplies the neighbour count.** A 2.5 m block's face is
   10 × 10 = 100 quarter-metre faces, each of which may abut a different
   neighbour. Adjacency is no longer "six neighbours"; it is "however many blocks
   overlap my surface". **[inferred]** This is, we suspect, the single largest
   hidden cost of unification, and it is invisible in every marketing
   description of the feature.

### 2.6 Read against this project

**[inferred]** The transferable conclusions:

- **One lattice.** SE1's two-grid split is a decade-long demonstration that a
  second granularity does not stay contained. This project has one tile size and
  should keep it; detail belongs in *geometry within a cell*
  (`StoreyGeometryEmitter`, `SurfaceFacing`), not in a second lattice.
- **Sparse storage plus a dense derived index** is the shape, and the index is a
  derived cache subject to `CLAUDE.md`'s three rules — fast path only skips
  provable no-ops, slow path unmodified, test against the source not against a
  second implementation.
- **Connectivity is a first-class runtime query, not a load-time fact**, the
  moment the player can remove a cell. `RoomPartition` is this project's version
  and the same rule applies: maintain incrementally, partition only when a link
  genuinely breaks.

---

## 3. Physics

### 3.1 Both games run Havok, and SE2 re-chose it deliberately

**[KEEN]** VRAGE2 used Havok. For VRAGE3, Keen evaluated **ChaosPhysics, PhysX3,
PhysX4, UniginePhysics and Havok 2021**, and chose **Havok 2021**. The three
VRAGE2 problems they name as the reason for the evaluation are precise and worth
recording verbatim:

> - phantom forces
> - unstable or wobbly constraint chains
> - poor support for high mass ratios

**[inferred]** That is a useful independent data point for this project's own
physics choice. The `physics-library-jolt` decision here was made on ragdoll
joint quality; Keen's list is about **constraint chains and mass ratios**, which
is the load a jointed vehicle puts on a solver, and it is a different axis. If
cromwell ever carries jointed constructions, those three failure modes are the
evaluation criteria — they are what a decade of shipping this exact problem
taught someone.

### 3.2 A ship is one rigid body made of many shapes

**[SRC]** SE1 builds a grid's collision from its blocks —
`MyCubeBlockCollector` gathers per-block shapes into the grid's Havok body, and
`MyGridShape` owns the result. Two details from the code are load-bearing:

- **[SRC]** The collector carries an explicit reference to **Havok's limit of
  254 body-to-body contact points**. A large ship resting on a large station can
  exceed that, and the engine has to care.
- **[SRC]** `MyCubeGrid.RayCastCells` goes through `MyGridIntersection` rather
  than through Havok — **the grid answers its own spatial queries against its own
  cell structure**, and only uses the physics engine for actual dynamics.

**[inferred] That second point is the architectural one and it matches
[`navigation.md`](../../topics/agents/navigation.md)'s core claim exactly: spatial queries and
physics are two systems.** A ray against a ship's cells is a DDA walk over an
integer lattice — cheap, exact, no broadphase. Routing it through Havok would
be strictly worse. SE arrived at the same split this project already has between
`OcclusionGrid`/`ReachField` and anything a physics engine would do.

**[inferred]** The rebuild cost is the flip side. Every block added or destroyed
invalidates the compound shape, and rebuilding a capital ship's shape from
scratch per hit is not viable — so this is another derived cache with a local
repair path, and it is the *hardest* one, because Havok owns the result.

### 3.3 Clusters — how a 32-bit physics engine survives a solar system

**[KEEN]** The best-documented single mechanism in either game, and the one most
worth stealing.

SE1 moved world coordinates to **64-bit doubles**, giving a safe radius of
"1,000,000,000 km, which equals to 6.6 AU". But Havok is single-precision and
converting it would have been slow. The answer:

> **[KEEN]** "the world in Space Engineers is split into independent
> **clusters**" — minimum size **20 km**, typically **50–100 km**, each object
> holding coordinates **relative to its cluster centre**, with a clustering
> algorithm that "guarantees that no dynamic object is closer than **2 km** to a
> cluster border."

**[SRC]** In the code this is `MyHavokCluster`, a `MyClusterTree`, and the
critical fact the source makes plain is that **there is one `HkWorld` per
cluster, not one globally** — clusters are created on demand as objects move,
via `OnClusterCreated`, which sizes the broadphase from the cluster's bounds.

**[inferred]** Three things make this better than plain floating origin, which
is the usual answer:

1. **It is not a rebase, it is a partition.** Floating origin shifts everything
   when the camera moves; clustering shifts nothing and just keeps independent
   local frames. There is no global stutter.
2. **The 2 km margin is the whole trick.** A hard border would thrash objects
   between clusters. A guaranteed clearance means the reassignment is always
   done well in advance and never in contact.
3. **Multiple `HkWorld`s means physics parallelises for free across clusters** —
   they cannot interact by construction.

The cost is honest and worth stating: **objects in different clusters cannot
collide or constrain**, so the clustering algorithm must guarantee that anything
that could interact shares a cluster. That is the entire complexity of the
system, and the 20 km minimum plus 2 km margin is how it is bought.

**[inferred]** For cromwell this is the reference design for large-world support
in the RTS and FPS targets — see [`map_scale.md`](../../topics/scale/map_scale.md), which is about
extent rather than count. It is not needed for a tile game and should not be
built until a project has the extent to need it, but the *shape* — 64-bit
authoritative world, 32-bit local frames, partition rather than rebase, margin
rather than border — is the thing to remember.

**[SRC]** One more detail from `MyPhysics.cs` worth having: SE1 defines
**22 collision layers, numbered 9–31**, with pairs explicitly disabled in
`InitCollisionFilters` "to optimize performance". Named ones include
`DefaultCollisionLayer` (15), `StaticCollisionLayer` (13),
`CharacterCollisionLayer` (18), `VoxelCollisionLayer` (28), `NoCollisionLayer`
(19), and `NotCollideWithStaticLayer` (12). **[inferred]** The lesson is dull but
real: broadphase filtering is a *design surface*, not a checkbox, and a game with
this many interacting kinds needs a deliberate layer matrix from the start.

### 3.4 Subgrids, constraint chains, and CLANG

**[KEEN]** SE1's November 2017 physics overhaul (v1.185) is the single most
technically candid thing Keen have published, because it documents a failed
approach being removed.

**What was removed: welding.** SE1 had tried to merge constrained grids into one
rigid body ("safety locking"). Keen's own account of why it went:

> "performance issues... simulation flaws like rigid bodies warping through each
> other... and risk of random disconnections."

**What replaced it:**

1. **Stronger constraints** rather than merged bodies, so subgrids stay
   independent but hold under stress.
2. **Shared inertia tensor** — a player-facing toggle that makes connected grids
   share physical properties.
3. **CoM-based thrust** — thrusters resolve against the true centre of mass,
   removing "subgrid drag".
4. **More aggressive deactivation of stationary grids**, which Keen credit with
   "massive performance improvements".

**[COMMUNITY]** And the clearest available statement of *why* CLANG — the
community name for SE's phantom-force instability — happens at all:

> "Since Havok constraints are solved **one by one rather than as an entire
> system**, smaller connected bodies can oscillate between larger ones as they
> fight over position, preventing the system from converging to a stable
> simulation."

**[inferred] That sentence is the whole thing, and it generalises well beyond
SE.** A sequential impulse solver iterating constraints in order does not
converge when the mass ratio between neighbours is extreme, because each
constraint's correction is undone by the next. The three mitigations available
are all visible in SE's history: **fewer constraints** (welding — tried,
abandoned for other reasons), **equalise the masses** (shared inertia tensor —
shipped), and **more solver iterations** (never free). SE2 attacks it from a
fourth direction — a newer solver and, per §2.3, *far fewer subgrids needed in
the first place*, because detail no longer requires a second grid.

**[KEEN/COMMUNITY]** SE2's claim is that VRAGE3 plus current Havok "largely
eliminat[es]" CLANG, with pistons, rotors and the new hinges "completely
reworked from the ground up". VS2.2 (March 2026) added movement limits to rotors
and pistons — **[inferred]** which is as much a stability feature as a usability
one, since a constraint with a hard limit is a constraint the solver can satisfy
rather than chase.

**[KEEN]** SE2 also ships a **safe speed system: no damage under 20 m/s**, with
grids reaching ~300 m/s. **[inferred]** That is a gameplay-shaped fix for a
numerical problem — collision response at high relative velocity is where a
discrete solver is least trustworthy, so the game simply declines to hurt you in
the regime where the physics is fine but the *damage* would be unfair.

### 3.5 What transfers

**[inferred]**

| Take | Leave |
|---|---|
| **Clusters** — partition, don't rebase; margin, not border | Havok specifically — the licence is not ours to have |
| **Spatial queries answered off the game's own lattice**, never off the physics engine | The 254-contact ceiling as a number; check whatever solver you use |
| **Constraint chains and mass ratios as the physics-library evaluation axis**, alongside ragdolls | Welding. Keen tried it and removed it. |
| **A deliberate collision layer matrix**, designed not accumulated | |

---

## 4. Planets and voxels

> This section is the **architecture** — what the representation is and why.
> [`voxel_terrain.md`](../../topics/world/voxel_terrain.md) is the **pipeline**: which mesher SE
> actually renders with (dual contouring, not marching cubes), how the clipmap
> hides its LOD switches, how a drill writes to the field, how the surface is
> textured and occluded, and how the asteroid and planet generators are built.

### 4.1 SE1's voxel constants, from the source

**[SRC]** `VRage/Voxels/MyVoxelConstants.cs`, which is the ground truth for
everything else in this section:

| Constant | Value | What it means |
|---|---|---|
| `VOXEL_SIZE_IN_METRES` | **1.0** | one voxel is one cubic metre |
| `DATA_CELL_SIZE_IN_VOXELS` | **8** (`_BITS = 3`) | storage granularity — 8³ = **512 voxels**, 8 m cube |
| `GEOMETRY_CELL_SIZE_IN_VOXELS` | **8** | meshing granularity — also 8 m |
| `GEOMETRY_CELL_MAX_TRIANGLES_COUNT` | **2560** (512 × 5) | worst-case triangles from one cell |
| `VOXEL_ISO_LEVEL` | **127** | surface threshold |
| `VOXEL_CONTENT_EMPTY` / `_FULL` | **0 / 255** | **one byte of content per voxel** |

**[inferred]** Read that table as a design, because it is a tight one:

- **1 m voxels** is coarse, and it is the reason SE1 terrain looks like SE1
  terrain. It is also why a 120 km planet is tractable at all.
- **One byte of density, not a boolean.** The isosurface at 127 means voxels
  interpolate — that is what gives smooth, mineable, deformable terrain rather
  than Minecraft cubes. It is the same choice as a signed distance field, at a
  byte's precision.
- **8³ cells for both storage and geometry**, with a hard triangle bound per
  cell. 5 triangles per voxel is the marching-cubes worst case, so the mesher's
  output buffer is statically sized. **No allocation in the meshing loop** —
  the same rule as this project's hot-loop discipline, arrived at by necessity.

### 4.2 Storage = a procedural provider plus an octree of the player's edits

**[SRC]** This is the best idea in either game and it is barely mentioned
anywhere.

SE1's voxel storage is `MyOctreeStorage : MyStorageBase`, and a storage has an
optional **data provider** — `MyPlanetStorageProvider` for planets, a noise
provider for asteroids. The source's own description of the reset operation:

> "resets data specified by flags to values **from data provider**, or default if
> no provider is assigned"

and, critically:

> reads "range of content and/or materials from specified LOD", but "if you want
> to write data back later, you must read LOD0 as that is the only writable one"
> — higher LODs "must be computed based on that".

**[KEEN]** Marek's 2014 post gives the payoff in plain terms: asteroids are
"procedurally generated at the moment they are required", unmodified ones cost
no RAM and are not saved at all, and **only modified voxels persist to storage**.

**[inferred] This is precisely `CLAUDE.md`'s derived-cache pattern, with the
polarity that matters:**

> **The procedural function is the authority. The octree is not a cache of it —
> it is the *diff* against it.** Nothing is stored until the player changes it,
> and what is stored is exactly what the function cannot reproduce.

Three properties follow, and all three are the reason this is worth copying:

1. **A pristine world is free** — no memory, no save data, no load time.
2. **Save size is proportional to what the player did**, which is the only
   quantity that should scale with play.
3. **The provider is re-runnable**, so LOD is a *query parameter* rather than a
   stored pyramid. Only LOD0 is writable because only LOD0 holds edits; every
   coarser level is recomputed.

**[inferred]** The failure mode is equally clear and SE hits it: **changing the
generator invalidates every world**, because the diff is meaningless against a
different base. That is why SE planets are shipped as fixed definitions and why
**[WIKI]** "when you start a Custom Game, there is no procedural generation for
Planets or Moons, they are always the same." The determinism is not a nicety; it
is load-bearing.

### 4.3 SE1 planets are a cube-map heightfield, not a noise field

**[COMMUNITY]** The modding documentation is the source here, and it is
consistent across several guides. An SE1 planet is defined by **six textures per
map type**, one per face of a cube, projected onto a sphere:

| Map | Contents |
|---|---|
| **Heightmap** | 16-bit elevation, **2048 × 2048 per face** is standard for a 120 km planet |
| **Biome / material map** | **R** = voxel material, **G** = foliage and voxel stones, **B** = ore placement |

**[COMMUNITY]** Constraints the modding guides state:

- Resolution "must be divisible by 8, and after that a power of 4 to enable
  important optimisations" — **[inferred]** i.e. the storage wants the face to
  tile evenly into 8-voxel data cells and to mip cleanly for LOD.
- "Anything higher than 2k... will break raycasts", so 2048 is a hard practical
  ceiling in SE1.
- A guide's worked example gives "1 pixel = 20 metres" at the shipped
  resolution. **[inferred]** Take that as illustrative for that planet rather
  than universal — it depends on diameter — but it fixes the order of magnitude:
  **the heightfield is one sample per ~20–50 m, and the 1 m voxels between
  samples are interpolation plus noise.**

**[inferred]** The arithmetic, since nobody states it: a cube-sphere face spans
~90° of arc, so its edge is about `(π/2)·R`. At 2048² per face that is **~46 m
per pixel on a 120 km planet** and **~7 m on a 19 km moon** —
[`voxel_terrain.md`](../../topics/world/voxel_terrain.md) §9.3 works it through and reads it against
SE2's published figure, which is a different regime entirely (a coarse base map
plus a tiled *detail* heightmap, claimed to reach 1 m effective).

**[WIKI/COMMUNITY]** Planet diameters are **60–120 km** (Earthlike, Mars, Alien
at 120 km), moons **19 km**.

**[inferred] The consequence is the honest limit of SE1's planets: they are
2.5D.** A cube-map heightfield has one elevation per direction. It cannot
express an overhang, an arch, or a cave, because the function is
height = f(direction). Everything SE1 has that looks like a cave is a separate
voxel object placed into the world, not part of the planet's own definition.

### 4.4 Rendering the voxels — clipmap LOD, async meshing

**[SRC]** `VRageRender.Voxels.MyClipmap` with a `VoxelKey` struct — so SE1's
voxel LOD is a **clipmap**: nested shells of geometry cells at doubling
resolution, centred on the viewer, with the shells scrolling as the camera
moves rather than being rebuilt.

**[KEEN]** Marek's 2014 post describes the LOD rework as replacing a two-tier
system with a graduated one, enabling "coarse low-detail procedural generation
which is faster than high-detail voxels" for distance — **[inferred]** i.e. the
provider is invoked *at the LOD being requested*, so distant terrain is not
generated at full detail and then decimated. That is §1's first rule: do less
work, not the same work faster.

**[KEEN]** Meshing runs asynchronously. Keen name "extending thread safety in
voxel polygonization and procedural generation" as a significant challenge,
"so the game doesn't lag when large terrains are requested". **[SRC]** The same
pattern appears on the navigation side — [`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md)
§4.1 records `MAX_TILES_TO_GENERATE = 7` per path request with a single-flight
guard, which is the same "bounded work per frame, off the main thread"
discipline applied to a different derived structure.

**[KEEN]** DX11 brought "smooth LOD transitions based on screen-space object
dithering (no popping)" — **[inferred]** dithered cross-fade between LOD levels,
resolved by TAA. Cheap and standard now; it was less so in 2015.

**[SRC]** The dither is only the outermost of *three* mechanisms, and the source
shows the other two: every cell is meshed **twice** (its own LOD and the next
coarser one) so each vertex carries a morph target, which the vertex shader lerps
by camera distance; and a cell will not be shown until its siblings, or hidden
until its children, have loaded. [`voxel_terrain.md`](../../topics/world/voxel_terrain.md) §4.2–4.4
has the whole recipe, and the reason SE therefore needs no Transvoxel.

### 4.5 SE2's planets — what actually changed

**[KEEN]** SE2's planets shipped December 2025. The claims, and what each one
implies:

**Terrain is now genuinely 3D.** This is the substantive change:

> **[KEEN]** VRAGE3 "can generate natural-looking overhangs, cliffs, and more
> complex landscapes by **spawning small voxel storages of overhangs and
> boulders** throughout the environment, with the procedural generator placing
> these in natural locations, resulting in **fully 3D terrain rather than just
> 2.5D**."

**[inferred] Read carefully, that is not a new terrain representation — it is a
composition strategy.** The base is still a heightfield (the phrase "rather than
just 2.5D" concedes the base is 2.5D). What is new is that the generator
*composites additional voxel storages* on top of it at chosen locations. That is
a good answer: it keeps the heightfield's cheapness for the 99% of terrain that
is height-like, and pays for real volume only where the world needs an overhang
or a cave mouth. It also drops straight into §4.2's architecture — an overhang
is just another storage, and the planet is a composition of a provider plus
placed storages plus player edits.

**[KEEN]** The August 2025 diary adds the other half, and it is the sharper
detail: caves exist because "the game now also **subtracts from the surface** to
generate them." **[inferred] So the generator uses the player's edit path.**
Anything expressible as a cut is free to generate and costs storage only where it
happens — [`voxel_terrain.md`](../../topics/world/voxel_terrain.md) §9.4.

**Dynamic voxel hardness.** **[KEEN]** "snow, sand and rock respond differently
to impacts, collisions, mining, and landings." **[inferred]** This is a per-
material property consulted by drilling, damage and the destruction system —
cheap to add given materials were already per-voxel, and it is the kind of thing
that reads as a physics upgrade while actually being a data one.

**Voxel destruction by contact impulse.** **[KEEN]** Hloušek's description is
specific and unusual:

> furrows are created using "**metaballs (SDFs) composed out of spheres in
> contact points with radius based on their impulse**"

**[inferred] That is the neatest detail in the VRAGE3 post.** A crash does not
run a destruction algorithm; it *subtracts an implicit shape* from a density
field. Because the terrain is already a density field at one byte per voxel
(§4.1), carving a sphere is a min/max over a small range — the same operation as
the player's drill, driven by the physics contact set instead. One mechanism,
two features, and the impulse maps directly onto the radius.

**Tessellation.** **[KEEN/ENG]** "tessellated terrain" and "tessellated voxel
materials" are in the feature list, with **[ENG]** an explicit note on
stabilising it: mip level for the displacement sample is chosen "using the
approximate triangle size" to stop vertices swimming as the camera moves.
**[inferred]** That is the classic displacement-mapping artefact and the fix is
the right one — the displacement sample's mip must track the *tessellated*
triangle density, not the screen-space texture density.

**Material transitions.** **[KEEN]** Across 2025 the diaries repeatedly return
to voxel material blending: transitions "extend across multiple voxels and
triangles" with "transition masks that include height information", tuned so
sand, soil, rock and snow "blend cleanly at the edges". **[inferred]**
Height-aware blending is height-blend/heightlerp — the standard fix for the
muddy linear crossfade you get from a plain alpha blend, and it costs one extra
channel.

### 4.6 The honest limits

**[inferred]** What SE's planets do *not* do, stated plainly because the
marketing does not:

- **They are not procedurally infinite.** Planets are authored definitions with
  fixed textures. The procedural machinery generates *within* an authored
  envelope. **[WIKI]** SE1's planets are always the same in a custom game.
- **1 m voxels are the floor of the terrain's resolution**, which is a fixed
  ratio against a 120 km planet. Detail below a metre comes from materials,
  tessellation and placed props, not from the voxel field.
- **The heightfield resolution is the real detail limit**, not the voxel size.
  At ~20–50 m per heightmap sample, everything finer is interpolation and noise.

---

## 5. Rendering

### 5.1 SE1: the number that explains VRAGE3

**[KEEN]** One figure from the VRAGE3 post frames the whole rewrite:

> Space Engineers "dedicat[ed] **over 3 cores to rendering**"

**[inferred]** That is a DX11-era, CPU-bound submission pipeline: a world made of
tens of thousands of distinct small objects (blocks) whose visibility must be
determined and whose draws must be recorded, per frame, by the CPU. It is the
worst possible workload for that model, and it is why "GPU-driven pipeline" is
the first thing Keen say about VRAGE3.

### 5.2 SE2: GPU-driven, DX12

**[KEEN]** The claim, verbatim:

> "**GPU driven pipeline – GPU feeding itself with draw calls, freeing CPU
> significantly**"

with DX12, and multi-platform from the start.

**[inferred]** No detail has been published on the mechanism — no mention of
mesh shaders, meshlets, or a two-phase occlusion cull. What *is* known is
consistent with a standard GPU-driven design: persistent geometry, GPU culling
producing indirect draw arguments, and the CPU submitting a handful of indirect
calls. The one corroborating detail is from VS2.3's notes — **[KEEN]** Keen
"improved how new geometry is prepared for rendering, which helps reduce
performance spikes when the game suddenly needs to process a lot of objects",
which is the residency/upload half of a GPU-driven pipeline behaving exactly as
you would expect it to misbehave.

**[KEEN]** VS2.3 also reports "roughly a **10% improvement to GPU frame
performance** with more stable frame times" from better vegetation culling and
shadow optimisation. **[inferred]** That is the only percentage figure Keen have
published about SE2's renderer, and it is a patch-note number, not a profile.

### 5.3 Lighting — clustered, with two light types worth noting

**[KEEN]** VRAGE3's light clustering is described as a **sparse representation**
supporting "more lights per tile with better performance than VRAGE2's tiled
rendering approach", and **[ENG]** Townsend describes "deferred clustering with
real time adjustment to light radii in extreme edge cases, to maintain
performance."

The light types:

| Type | Detail |
|---|---|
| **Spot** | cookie projection, onto solid *and transparent* surfaces |
| **Point** | cube map shadows, physically-based inverse-quadratic attenuation |
| **Capsule** | **new in VRAGE3** — neon tube lighting, emitting along the capsule centreline |
| **Area** | rectangle emission, specifically so LCD blocks reflect correctly — absent from VRAGE2 |

**[inferred]** Three things to take:

1. **Sparse cluster storage over dense tiles** is the same conclusion as
   [`source2_rendering.md`](../valve/source2_rendering.md) §13 and this project's
   clustered-forward plan. A ship interior is the pathological case for tiled
   lighting — hundreds of small lights, most affecting a handful of clusters —
   and it is exactly where sparsity pays.
2. **"Real time adjustment to light radii"** is a graceful-degradation valve
   worth remembering: when a cluster overflows, shrink the offending lights'
   radii rather than dropping lights or blowing the budget. It degrades
   continuously instead of popping.
3. **Capsule and area lights are cheap analytic wins** for a built environment.
   A strip light rendered as a capsule rather than a row of point lights is one
   light instead of eight, and it looks right instead of scalloped.

### 5.4 Shadows — and one trick worth stealing outright

**[KEEN]** VRAGE3's sun cascade work:

- **Directional antialiasing** — analyse a **4-pixel quad** of sampled shadow
  texels to estimate edge direction, then **stretch the sampling kernel into an
  ellipse along that direction**. **[ENG]** Townsend's own verdict: "a novel
  technique. but in hindsight, a bit expensive."
- **Fibonacci spiral Poisson sampling** — better distribution from fewer
  samples than a fixed Poisson disk.
- **Cascade blending** — sample the *complete* kernel in each cascade and blend,
  rather than splitting the sample budget across two cascades.
- **Moiré at grazing angles** — fixed by adjusting kernel *radius* rather than
  dropping to a coarser mip.

And the one that is worth the whole section:

> **[KEEN]** shadow stabilisation uses the **pivot object's position (the
> cockpit)** rather than the camera's, to prevent jitter while the vehicle
> moves.

**[inferred] That is the correct answer to a problem this project will have.**
Cascade shadow maps are snapped to texel boundaries to stop shimmering; the snap
is normally relative to the camera. Inside a moving vehicle the camera is
translating continuously in world space even when the player is standing still
*relative to their surroundings*, so the snap fights the motion and the interior
shimmers. Snapping to the frame the viewer is *stationary in* fixes it exactly.
This is the rendering counterpart of
[`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md) §3 — **the right
reference frame is the one the observer shares with the geometry**, and that note
already argues the same thing for navigation queries and steering.

**[ENG]** Also: characters get "extra higher-texel-density shadow maps", plus
separate maps and stencils "so that a third person shadow can render while the
character is in first person" — **[inferred]** the standard fix for the
first-person-with-a-body problem, worth knowing exists.

### 5.5 Ray-traced GI and reflections, and the constraint that shaped them

**[ENG]** Townsend implemented "Raytraced Global Illuminations and Reflections",
and is explicit that the priorities were **"performance, stability, lack of
noticeable artifacts"** over physical accuracy, requiring:

> "novel work to make it effective in our **highly dynamic environment, which
> requires visual stability within the interiors of moving ships**."

**[inferred] That constraint is more interesting than the feature.** Every
standard real-time GI scheme — irradiance probe grids (DDGI), screen-space GI,
surfel caches — accumulates temporally in **world space**. A ship interior
moving at 300 m/s invalidates a world-space cache completely, every frame. So
either the cache lives in the ship's local frame (which means one cache per
moving grid, and a way to blend between them at the boundary) or the accumulation
has to be robust to full invalidation. Keen have not said which. **[inferred]**
Given §5.4's cockpit-pivot stabilisation, the local-frame reading is the more
likely one, and it is the same architectural move a third time.

No technique name, no probe count, no cost has been published. **Do not treat
SE2 as a reference implementation for GI** — treat it as a statement that the
moving-frame problem exists for lighting caches too.

### 5.6 Dynamic armour bevels — the most directly transferable idea here

**[ENG]**

> "Armor faces on a grid are **processed on the CPU** and data is sent to the
> **GPU in order to render bevels on edges**."

**[KEEN]** And from the wiki side: the render material system's parallax mapping
"generates subtle rounded bevels on armor edges **and between blocks of
different colors**."

**[inferred] This is the thing to actually copy, and this project has the
machinery for it already.** The problem is identical: a world built from
axis-aligned cells looks like a world built from axis-aligned cells, because
every edge is geometrically perfect and reads as cardboard. Modelling a bevel
into every block is unaffordable — the block count is the whole point — and it
cannot express the *conditional* case, where an edge should only be bevelled
where it is genuinely exposed.

So: **the CPU already knows which faces are exposed and which edges are
silhouette**, because it computed that to build the mesh in the first place. Ship
that classification to the GPU as per-face data, and let the material round the
edge in the shader — parallax or normal perturbation, no extra geometry.

The parallel to this project is exact. `SurfaceFacing.hpp` and
`StoreyGeometryEmitter` already classify which surfaces face where when emitting
storey geometry, and `SurfaceBuffers` already carries per-surface data to the
GPU. The exposed-edge set is a by-product of work already being done, and a
bevel term in the surface shader is the cheapest visual upgrade available to any
lattice-built world. **[inferred]** Keen's second clause — bevelling "between
blocks of different colors" — is the detail that makes it read as *construction*
rather than as a rounding filter, and it costs one comparison.

### 5.7 Two small stabilisation notes

**[ENG]** Both are about temporal upscaling fighting high-frequency detail, and
both are worth knowing before hitting them:

- **Grates under FSR/TAA.** Parallax cutout patterns "flicker/streak" under
  temporal accumulation; the fix was modified "parallax/cutout/sample bias
  behavior specifically for these grates". **[inferred]** i.e. a per-material
  temporal policy, not a global one — some materials need to bias toward
  blurrier samples than the rest of the frame.
- **PBR.** VRAGE3 moved from Blinn-Phong to **GGX**, with importance-sampled
  environment maps and **normal variation baked into roughness mip generation**.
  **[inferred]** That last one is Toksvig/LEAN-style normal-variance-to-roughness
  and it is the single cheapest fix for specular aliasing on detailed normal
  maps. [`source2_rendering.md`](../valve/source2_rendering.md) covers the same ground.

### 5.8 What transfers to a GL 4.3 renderer

**[inferred]**

| Take now | Take later | Not applicable |
|---|---|---|
| **CPU-classified edges → GPU bevels** (§5.6) | Sparse light clusters (§5.3) — already the plan | Ray-traced GI |
| **Shadow snap to the observer's frame, not the camera** (§5.4) | Capsule and area lights | DX12 / mesh shaders |
| **Normal-variance → roughness mips** (§5.7) | Light-radius shrink as an overflow valve | GPU-driven indirect submission |
| **Height-aware material blending** (§4.5) | | |

---

## 6. Automation — and how you tick thousands of things

The question this section exists to answer: **SE grids contain thousands of
active blocks. How does that not cost a frame?**

The answer is four separate mechanisms, and none of them is "make the per-block
update fast".

### 6.1 What automation is

**[KEEN/WIKI]** SE1 got it in the 2023 *Automatons* update; SE2 shipped its
version in VS2.3, July 2026. Three blocks, and the design goal is stated
explicitly: **"You don't need to know programming or scripting."**

| Block | Role |
|---|---|
| **Event Controller** | watches a parameter on one or more blocks; fires toolbar actions when it crosses a threshold. **Separate true and false action sets.** |
| **Sensor** | reacts to players, voxels and grids in proximity |
| **Timer** | fires actions immediately or after a delay; chains into sequences |

**[KEEN]** SE2's Event Controller watches, at grid level, "linear/angular speed,
mass, gravity, atmosphere percentage, dampener status", and at block level
"functional states, inventory fill percentages, door positions, connector
readiness, weapon ammo levels, and rotor/piston distances". Keen describe it as
built "on top of new VRAGE3 systems that let it track many different grid and
block parameters."

**[inferred]** "New VRAGE3 systems" is doing a lot of work in that sentence, and
§6.2 is our reading of what it means.

### 6.2 Mechanism one: don't tick — make it event-driven

**[inferred]** The Event Controller's name is the design. The naive
implementation polls: every controller, every frame, reads its watched parameter
and compares. A thousand controllers is a thousand reads and a thousand
comparisons per frame, plus — much worse — a thousand *pointer chases* to
scattered blocks.

The implementation the design implies is the reverse: **the watched property
publishes when it changes, and the controller is invoked only then.** A door
that does not move costs nothing. A tank that is not filling costs nothing. The
cost becomes proportional to *events*, not to *watchers*, and in a working ship
the event rate is orders of magnitude below the watcher count.

The tell is the "separate true/false actions" design. A polling controller would
naturally expose one action list and a current state; **separate rising-edge and
falling-edge action sets are what you build when the controller is invoked on a
transition** — because the transition is the thing you have.

**[inferred] The generalisable rule, and it is the one worth writing down:**

> **A watcher costs nothing if the watched thing pushes. Polling turns N
> watchers into N reads per frame; publishing turns them into one dispatch per
> actual change. The asymmetry is the entire performance story of any "make X
> react to Y" system.**

This project's `Entity::onComponentsChanged` is the same idea applied to a
different problem — cache the answer at bind time, invalidate on change, never
re-query in the loop.

### 6.3 Mechanism two: tiered update rates

**[SRC]** SE1's entity update system is explicit about this and it is the
cheapest possible win:

```
MyEntityUpdateEnum.BEFORE_NEXT_FRAME
MyEntityUpdateEnum.EACH_FRAME
MyEntityUpdateEnum.EACH_10TH_FRAME     -> UpdateBeforeSimulation10()
MyEntityUpdateEnum.EACH_100TH_FRAME    -> UpdateBeforeSimulation100()
```

An entity sets `NeedsUpdate` to the tier it needs and the engine calls only the
matching method. **[SRC]** The same tiering is exposed to mods through
`MyGameLogicComponent`, so third-party blocks are opted into the discipline by
default rather than having to discover it.

**[inferred]** Two observations:

- **The tiers are 1 / 10 / 100, not 1 / 2 / 4.** That is a factor of ten between
  neighbours, which is coarse enough that choosing is easy and the saving is
  real. A finer ladder invites agonising over which rung.
- **Nothing here staggers phase.** A naive reading has every `EACH_100TH_FRAME`
  entity firing on the same frame, producing a 100-frame sawtooth. **[inferred]**
  Whether SE1 buckets by entity id is not something we could confirm from the
  code we read — but *any* implementation of this pattern should, and it is one
  line: dispatch entity `e` when `frame % 100 == e.id % 100`.

### 6.4 Mechanism three: per-grid systems, not per-block updates

**[SRC/WIKI]** The heavier gameplay systems are not per-block at all. Power,
conveyor routing and gas distribution are **grid-level components** that own a
graph, and blocks register into that graph rather than each simulating
themselves.

**[WIKI]** The behavioural evidence is that conveyor transfer is *instantaneous*
when a path exists — items do not travel along tubes as simulated entities. The
system answers a reachability question over a connectivity graph and moves the
item. **[COMMUNITY]** And the community optimisation advice confirms the graph is
the cost: simplifying a network to "same distances but fewer nodes" can "reduce
the amount of conveyors by 50%–100%", which only makes sense if the expense is
**graph traversal**, not per-tube work.

**[inferred] The pattern, stated generally:**

> **N blocks participating in one system is one system with N registrations, not
> N systems.** The per-block object holds configuration and identity; the
> per-grid component holds the structure and does the work. The per-frame cost
> scales with the number of *grids*, and the per-edit cost scales with the
> locality of the graph repair.

That is the same conclusion as [`crowd_scale.md`](../../topics/scale/crowd_scale.md)'s central
finding from a completely different direction — AC Unity's 40 real AI behind
10,000 bodies is the same trade: **the expensive thing is a small shared system,
and the many cheap things register into it.**

### 6.5 Mechanism four: budget it — PCU

**[WIKI]** SE's *Performance Cost Unit* system deserves recording because it is a
design answer to a performance question, and those are rare.

Every block definition carries a **PCU value representing its performance impact
under full load**. **[WIKI]** Armour is 1 PCU on PC (2 on console);
**[COMMUNITY]** measured examples put doors and turrets near 115 and thrusters
near 15. A world has a global PCU budget, factions get a share, players get a
share of their faction's, and when it is exhausted the HUD says so and you cannot
build.

**[KEEN]** SE2 kept the system and Keen note that its "PCU costs reflect more
accurate performance measurements than SE1."

**[inferred]** What makes this worth reading is not the mechanism but the
admission inside it: **Keen concluded they could not make arbitrary player
constructions fast, so they made the cost visible and capped it.** The budget is
denominated in the currency the player spends (blocks), it is enforced at build
time rather than discovered at frame time, and the per-block figures are tuned
from measurement.

The alternative — optimise until anything is affordable — is not available for a
sandbox, because the player's ambition is unbounded by construction. **[inferred]**
Any project that lets players build unboundedly needs a PCU equivalent, and it
should be designed early, because retrofitting a budget onto an existing
economy is a balance change and players notice.

### 6.6 So: how do you handle thousands of these per frame?

**[inferred]** In descending order of what it buys:

1. **Don't run them.** Event-driven, not polled. Cost tracks events, not
   watchers. (§6.2)
2. **Run the survivors rarely.** 1 / 10 / 100 tiers, phase-staggered. (§6.3)
3. **Run one system, not N.** Per-grid graphs with block registrations, repaired
   locally on edit. (§6.4)
4. **Cap what you cannot make cheap**, in the player's own currency. (§6.5)

Note that this is `CLAUDE.md`'s order of attack exactly — every one of those is
"do less work", and **not one of them is "do it closer"**. Layout does not appear
until you have exhausted these, and for this class of system you never do.

---

## 7. Two systems SE1's source settles

Short section, because both are already covered elsewhere and this only adds the
SE evidence.

### 7.1 Spatial queries are not physics

**[SRC]** `MyCubeGrid.RayCastCells` walks the grid's own cell structure through
`MyGridIntersection`. The physics engine is not consulted.

**[inferred]** Independent confirmation of [`navigation.md`](../../topics/agents/navigation.md) §1
and [`spatial_queries.md`](../../topics/agents/spatial_queries.md): a game with an authoritative
lattice should answer lattice questions arithmetically. Handing them to a
broadphase is slower, less exact, and couples gameplay to a middleware's
threading model.

### 7.2 Navigation — and a finding that updates the existing note

[`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md) §4 already reads SE1's
two navigation systems from source: `MyGridNavigationMesh` in **grid-local**
coordinates for ships, Recast/Detour for the voxel world. That remains the
sharpest buy-versus-build line in the study folder and nothing here changes it.

**What is new: [KEEN] Jan Hloušek's VRAGE3 post lists Kythera AI among the
middleware VRAGE3 adopted**, alongside DirectX 12, Havok and FMOD.

**[inferred]** That is a notable finding, because
[`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md) §3.1 quotes Kythera's
own material as the clearest published description of the moving-navmesh
mechanism — bake in local space, update a 4×4 matrix per frame, never
regenerate — and §9 lists moving-frame support as "the one genuinely unavoidable
build" because only Kythera and Mercuna sell it. **So Keen, having hand-written
exactly that system for SE1, licensed it for SE2.** A studio with a shipped
implementation and a decade of domain expertise chose to buy rather than
re-build. That is the strongest available evidence that the build cost is real.

**Two honest caveats.** First, the Kythera line is from a **2023** post and
middleware choices change during a four-year development. Second, SE2's 2026 dev
diaries describe behaviour trees authored **in the VRAGE3 editor** and "our first
experiments with AI pathfinding", which reads like in-house behaviour on top of
whatever supplies navigation — consistent with Kythera doing navmesh and
pathfinding while Keen own behaviour, but also consistent with the choice having
changed. **Do not state as fact that SE2 ships Kythera**; state that Keen
listed it in 2023.

---

## 8. What this means for cromwell

**[inferred]** Ordered by what is actually actionable here.

**Take now — cheap, and this project has the machinery:**

1. **CPU-classified edges → GPU bevels.** §5.6. `SurfaceFacing` and
   `StoreyGeometryEmitter` already compute the exposed-face classification; a
   bevel term in the surface shader is the highest visual return available to a
   lattice-built world, and it needs no new geometry. Bevel differently where
   materials differ — that clause is what makes it read as construction.
2. **Snap shadow cascades to the observer's frame, not the camera's.** §5.4. Free
   today (identity transform), and it is the difference between a shimmering and
   a stable interior the moment anything the player stands on moves.
3. **Normal-variance folded into roughness mips.** §5.7. Standard, cheap, and the
   correct fix for specular aliasing.

**Confirmations of decisions already made — no action, but the evidence is
worth having:**

4. **One lattice, not two.** §2.2. SE1's large/small split is a decade-long
   demonstration of how a second granularity metastasises from a content
   decision into a physics and networking one.
5. **Derived caches with local repair.** §4.2 and §3.2. SE's voxel storage is
   `CLAUDE.md`'s pattern with the polarity clarified: **the generator is the
   authority and the stored data is the diff**, so a pristine world is free and
   save size tracks player action.
6. **Sparse light clusters.** §5.3. Matches the clustered-forward plan and
   [`source2_rendering.md`](../valve/source2_rendering.md) §13. The addition is the
   **light-radius-shrink overflow valve** — degrade continuously rather than
   dropping lights.
7. **Spatial queries off the lattice, never off the physics engine.** §7.1.

**New rules for any per-frame system, from §6 — these are the answer to "how do
you tick thousands of things":**

8. **Push, don't poll.** §6.2. A watcher costs nothing if the watched thing
   publishes. This is the single largest factor and it is an *interface*
   decision, so it is cheap now and expensive later.
9. **Tiered update rates with phase stagger.** §6.3. 1 / 10 / 100, dispatched on
   `frame % N == id % N` so a tier does not fire as one spike.
10. **One system with N registrations, not N systems.** §6.4. Per-grid graphs,
    repaired locally on edit.
11. **A visible budget for anything the player can multiply.** §6.5. PCU is a
    design answer to a performance question and it has to be designed early.

**For the engine's other target genres, not for this game:**

12. **Physics clusters.** §3.3. 64-bit authoritative world, 32-bit local frames,
    **partition rather than rebase**, and a guaranteed margin rather than a hard
    border. This is the reference design for the RTS and FPS extent problem
    ([`map_scale.md`](../../topics/scale/map_scale.md)) and should not be built until a project
    needs it.
13. **Constraint chains and mass ratios as physics-library evaluation criteria**,
    alongside the ragdoll-joint axis that decided Jolt. §3.1. Keen's three named
    VRAGE2 failures — phantom forces, wobbly constraint chains, poor high mass
    ratio support — are what a decade of shipping jointed constructions teaches,
    and a sequential-impulse solver has all three by construction (§3.4).

**Explicitly not to take:**

- **SE2 as a rendering reference implementation.** §5.5. There is no published
  technique, no probe count, no cost. What SE2 supplies is the *constraint* —
  world-space lighting caches do not survive moving interiors — not a solution.
- **The two-grid split, in any form.** §2.2.
- **Welding constrained bodies together for stability.** §3.4. Keen tried it and
  removed it.

---

## Sources

**Space Engineers 1 — released source (primary)**
- [KeenSoftwareHouse/SpaceEngineers](https://github.com/KeenSoftwareHouse/SpaceEngineers) — the released C# source
- [`MyVoxelConstants.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyVoxelConstants.cs) — voxel size, data/geometry cell sizes, iso level, triangle bounds
- [`MyPhysics.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Physics/MyPhysics.cs) — `MyHavokCluster`, per-cluster `HkWorld`, the 22 collision layers, stepping and threading
- [`MyCubeGrid.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Game/Entities/Cube/MyCubeGrid.cs) — `Dictionary<Vector3I, MyCube> m_cubes`, `RayCastCells`, split handling
- [`MyCubeBlockCollector.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Game/Entities/Cube/MyCubeBlockCollector.cs) — compound shape assembly, the 254-contact-point note
- [`MyEntity.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage.Game/Entity/MyEntity.cs) and [`MyGameLogicComponent.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage.Game/Entity/EntityComponents/MyGameLogicComponent.cs) — `MyEntityUpdateEnum`, the 1/10/100 update tiers
- [`MyStorageBase.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Storage/MyStorageBase.cs) — provider-plus-octree storage, LOD0-only writes
- API reference mirrors: [`MyOctreeStorage`](https://fresc81.github.io/SpaceEngineers/class_sandbox_1_1_engine_1_1_voxels_1_1_my_octree_storage.html) · [`MyGridShape`](https://fresc81.github.io/SpaceEngineers/class_sandbox_1_1_game_1_1_entities_1_1_cube_1_1_my_grid_shape.html) · [`MyClipmap.VoxelKey`](https://fresc81.github.io/SpaceEngineers/struct_v_rage_render_1_1_voxels_1_1_my_clipmap_1_1_voxel_key.html) · [`MyCubeGridSmallToLargeConnection`](https://fresc81.github.io/SpaceEngineers/class_sandbox_1_1_game_1_1_entities_1_1_cube_1_1_my_cube_grid_small_to_large_connection.html)

**Keen — engine and design (dev blogs)**
- [Guest post: Jan Hlousek — VRAGE3 Engine Update](https://blog.marekrosa.org/2023/04/guest-post-jan-hlousek-vrage3/) — **the single most technical published source on VRAGE3**: data-oriented architecture, GPU-driven pipeline, DX12/Havok/Kythera/FMOD, the physics evaluation, shadow and light clustering work, metaball voxel destruction, hybrid fracturing
- [Space Engineers: Super-large worlds, Procedural asteroids and Exploration](https://blog.marekrosa.org/2014/12/space-engineers-super-large-worlds_17/) — 64-bit coordinates, the cluster system, procedural asteroids, LOD rework
- [Space Engineers: Planets, oxygen, DirectX 11, optimizations and multi-player](https://blog.marekrosa.org/2015/02/space-engineers-planets-oxygen-directx_18/) — planet scale, spherical gravity, dithered LOD transitions
- [Space Engineers: Major Physics Overhaul](https://blog.marekrosa.org/2017/11/physics/) — welding removed, stronger constraints, shared inertia tensor, CoM thrust, deactivation
- [Space Engineers: Automatons](https://blog.marekrosa.org/2023/04/space-engineers-automatons/) — SE1's automation blocks
- [Space Engineers 2: Alpha Reveal](https://blog.marekrosa.org/2024/12/space-engineers-2-alpha-reveal/) — VRAGE3 from January 2022, the unified grid, safe speed, PCU
- [SE2: VS2 — Planets & Survival Foundations](https://support.keenswh.com/spaceengineers2/pc/announcement/space-engineers-2-alpha-vs-2-planets-survival-foundations) — planets, dynamic voxel hardness, overhangs and caves
- [SE2: VS2.2 — Mechanical Blocks & Weapons](https://blog.marekrosa.org/2026/03/space-engineers-2-vs2-2/) — reworked rotors/pistons/hinges, movement limits
- [SE2: VS2.3 — Drive, Automate & Detonate](https://blog.marekrosa.org/2026/07/se2-vs2-3/) and [patch notes](https://support.keenswh.com/spaceengineers2/pc/announcement/space-engineers-2-alpha-vs2-3-drive-automate-detonate) — Event Controller / Sensor / Timer, wheel sizes, the ~10% GPU figure
- [Marek's Dev Diary: April 30, 2026](https://blog.marekrosa.org/2026/04/mareks-dev-diary-april-30-2026/) — water through 25 cm holes
- Material-transition and biome-blending diaries: [Aug 21 2025](https://blog.marekrosa.org/2025/08/mareks-dev-diary-august-21-2025/) · [Oct 2 2025](https://blog.marekrosa.org/2025/10/mareks-dev-diary-october-2-2025/)
- [VRAGE — Keen Software House](https://www.keenswh.com/VRAGE/) — the marketing feature list, useful for scope, not for cost

**Keen engineers, first person**
- [Space Engineers 2: Rendering Engineering Work](https://www.landontownsend.com/single-post/space-engineers-2-rendering-engineering-work-wip) — Landon Townsend on RT GI/reflections, heuristic AA shadow sampling, character shadows, GGX refactor, deferred clustering, tessellation stabilisation, **dynamic armour bevels**, grate/TAA fixes

**Wikis (behaviour, not implementation)**
- [Unified Grid System](https://spaceengineers2.wiki.gg/wiki/Unified_Grid_System) · [VRAGE3](https://spaceengineers2.wiki.gg/wiki/VRAGE3) · [SE2 PCU](https://spaceengineers2.wiki.gg/wiki/PCU)
- [SE1 Large Grid](https://spaceengineers.wiki.gg/wiki/Large_Grid) · [SE1 Voxels](https://spaceengineers.wiki.gg/wiki/Voxels) · [SE1 Planets](https://spaceengineers.wiki.gg/wiki/Planets) · [SE1 PCU](https://spaceengineers.wiki.gg/wiki/PCU) · [Conveyor system](https://spaceengineers.wiki.gg/wiki/Conveyor_system)

**Community (weakest tag — verify before depending on)**
- [Creating a Planet](https://spaceengineers.wiki.gg/wiki/Modding/Tutorials/Creating_a_Planet) and [How to create your own planet](https://spaceengineers.fandom.com/wiki/How_to_create_your_own_planet) — cube-face heightmaps, 2048² resolution, RGB channel assignments, the raycast ceiling
- [Planet Modding — Full Guide (Medieval Engineers)](https://medievalengineerswiki.com/w/Keen:Planet_Modding_-_Full_Guide) — the same generator, documented more fully
- [Conveyor performance — code optimization design](https://support.keenswh.com/spaceengineers/pc/topic/22920-proposal-conveyor-performance-code-optimization-design) — network simplification and where the cost sits
- [How are the physics?](https://steamcommunity.com/app/1133870/discussions/0/577172807462543902/) — the sequential-constraint-solving explanation of CLANG
