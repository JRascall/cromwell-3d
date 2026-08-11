# Navigation architecture for cromwell — design note

**This is a design note, not research.** Everything here is a decision and its
justification. The evidence sits in four companion files and is cited rather
than repeated:

| Reads from | For |
|---|---|
| [`navigation.md`](navigation.md) | the four layers; where navmeshes stop scaling; why agent count and world size are different problems |
| [`crowd_scale.md`](crowd_scale.md) | L4D's deliberately crude representation; why coarse + strong steering beats fine + weak |
| [`moving_frame_navigation.md`](moving_frame_navigation.md) | moving frames, open volume, the library survey, the parallelism pass |
| [`spatial_queries.md`](spatial_queries.md) | the neighbouring system this one must *not* absorb; §5.2's grid distance bias |

Nothing here is built yet. The point of writing it before building is that the
**shape** is the expensive part to change — per `CLAUDE.md`'s performance
discipline, layout and access patterns are decided once and lived with.

---

## 1. The rule that generates all the others

> **There is no navigation *system*. There are representations, and there are
> algorithms that read them. Both are plain constructed objects with explicit
> lifetimes. No manager, no singleton, no `NavSystem::findPath()` that decides
> internally which backend to use.**

Every other decision in this note falls out of that one.

The requirement it serves is **concurrent use, not just swappable use**. An RTS
wants flow fields for the mass, a navmesh for hero units and a coarse graph for
strategic AI — all live at once, over the same world, queried by different
agents in the same frame. A global system with a mode cannot express that; a
constructed object trivially can.

It also makes "use none of it" free rather than a special case. A project that
constructs nothing from `nav/` has no navigation, and nothing downstream
notices — see §7.

**Rejected:** a single `INavigation` facade. [`navigation.md`](navigation.md)
§13 already warns that one navigation system for all three genres "produces a
system that serves none of them", and a facade is how that happens by accident —
it starts as a convenience and ends as the place every backend's special case
gets bolted on.

---

## 2. The representation catalogue

Five, and the fifth is nothing.

| | Node is | Lives in | Frame | Primary genre | Source |
|---|---|---|---|---|---|
| **Lattice** | cell index | `game/` | world (identity) | tactics — this game | exists today |
| **NavMesh** | `dtPolyRef` | `cromwell/nav/recast/` | world **or** hull-local | FPS, third-person | **Recast/Detour**, zlib |
| **AreaGraph** | area id | `cromwell/nav/area/` | world **or** hull-local | horde, director AI, RTS coarse layer | **written** — L4D-style rectangle growing |
| **VolumeGraph** | octree node | `cromwell/nav/volume/` | world | space, flying | **written** — SVO, Game AI Pro 3 ch. 21 |
| **— none —** | — | — | — | open space, arcade | construct nothing |

`Lattice` stays in `game/`. It is genuinely tile-specific — ramps, mantles,
drops, `MoveKind` — and the engine must not assume a discrete world. It
implements the engine's interface, which is the correct direction for the
dependency arrow and satisfies the one architectural rule in `CLAUDE.md`.

**Rejected:** a generic `GridGraph` in cromwell "for other projects". Nothing
needs it yet, and `Lattice` would not use it — its connectivity comes from
height deltas, not adjacency (see `Move.hpp`). Build it when a second grid
project exists, not before.

---

## 3. Requirements matrix

What each representation must provide, and what it can therefore support. This
is the table that says whether the interface is cut correctly: if a column is
all-yes or all-no it does not belong in the interface.

| | Lattice | NavMesh | AreaGraph | VolumeGraph |
|---|---|---|---|---|
| **Dense node index** | yes | **no** — `dtPolyRef` is sparse | yes | yes |
| **Flow fields possible** | yes | **no** (follows from the above) | yes | yes |
| **Bake** | none — the grid *is* the graph | offline / on load, per tile | on load | on load, per region |
| **Bake parallelises** | n/a | **yes**, per tile | yes, per region | yes, per region |
| **Runtime mutation** | cheap — per cell | tile rebuild (`dtTileCache`) | region regrow | region rebuild |
| **Moving frame capable** | n/a | **yes, if baked local** — §5 | yes, if baked local | not useful (world-space volume) |
| **Needs steering** | **no** — cell-to-cell movement | yes | yes | yes |
| **Path realisation** | identity | funnel / string-pull | direction-only | funnel-ish or direct |
| **Buy or build** | exists | **buy** (Recast, zlib) | **build** | **build** |

Two rows carry real design weight:

- **Dense node index** splits the interface in two (§4.2). It is not a
  performance detail — it decides which algorithms are legal on which
  representation, and the type system should enforce that rather than a runtime
  check.
- **Needs steering** is why steering is not part of navigation at all (§6.4).
  The one representation that needs no steering is the one this project
  currently ships.

---

## 4. The interfaces

### 4.1 Vocabulary

```cpp
namespace cromwell::nav {

enum class NodeId : std::uint64_t { kNone = ~0ull };

/* One legal step out of a node. `tag` is the backend's own move
 * classification — MoveKind for a lattice, an area type for a navmesh —
 * opaque to every algorithm above this line. */
struct Step {
    NodeId        to;
    float         cost;
    std::uint32_t tag;
};

}
```

`NodeId` is 64-bit because `dtPolyRef` is. Grids and area graphs waste four
bytes per node in the open list and it is not worth two node types to save
them — a single `NodeId` is what allows one planner to serve every backend.

### 4.2 The two graph interfaces

```cpp
class NavGraph {
public:
    virtual ~NavGraph() = default;

    /* Appends `from`'s legal steps to `out`, which is cleared first.
     * `ctx` is the backend's own filter type — see 4.3. */
    virtual void steps(NodeId from, const QueryContext* ctx,
                       std::vector<Step>& out) const = 0;

    virtual bool traversable(NodeId, const QueryContext* ctx) const = 0;

    /* Graph-local coordinates. NEVER world. See §5. */
    virtual Vec3 position(NodeId) const = 0;

    /* Defaults to Euclidean over position(); override where the graph knows
     * better — octile for a lattice, and it matters, because a bad heuristic
     * costs node expansions in the hot loop. */
    virtual float heuristic(NodeId a, NodeId b) const;
};

/* Refinement for representations with contiguous integer node numbering.
 * Anything that needs an array indexed by node requires this — which in
 * practice means every field-based algorithm. */
class DenseGraph : public NavGraph {
public:
    virtual std::uint32_t index(NodeId) const = 0;
    virtual std::uint32_t nodeCount() const = 0;
};
```

**Why the split is a type and not a flag.** Flow fields need an array indexed by
node. Detour's `dtPolyRef` encodes salt, tile and polygon — it is not a
contiguous index, and making it one means a hash map, which `CLAUDE.md`'s
hot-loop rules forbid outright. So a flow field over a Detour navmesh is not
merely slow, it is the wrong structure. Taking `DenseGraph&` in `FieldBuilder`
makes that a compile error instead of a runtime disappointment, and documents
the constraint in the only place nobody can skip reading.

Nobody flow-fields a navmesh in practice either — the shipped technique is flow
fields over grids and tiles ([`navigation.md`](navigation.md) §9;
[`moving_frame_navigation.md`](moving_frame_navigation.md) §8.2). The interface
agrees with the industry rather than inventing a constraint.

### 4.3 `QueryContext` — the piece that is expensive to get wrong

Every backend filters differently. `MoveGraph` today takes `const BlockedMask*`.
Recast takes a `dtQueryFilter` with an area cost table and include/exclude
flags. A volume graph wants an agent radius. There is no union of these worth
writing.

**Decision:** `QueryContext` is an empty polymorphic base. Each backend defines
its own derived type; the caller constructs the one matching the graph it is
querying, and the backend `static_cast`s on a documented precondition.

```cpp
class QueryContext { public: virtual ~QueryContext() = default; };
```

This looks loose and is safe in practice, because **the caller already knows
which graph it is talking to** — it constructed it. The looseness is confined to
one pointer at one call site.

**Rejected — templating the planner on the graph type.** It is type-safe and it
is what a library would do, but it forces every algorithm into headers,
multiplies the code by the number of backends, and destroys the runtime
swapping in §1. An RTS choosing between a navmesh and a flow field at runtime
cannot do so if the choice is a template parameter.

**Cost of the virtual.** One indirect call per `steps()`, which is one per *node
expansion*, not per *edge* — the batched fill amortises it across the whole
fan-out, and the node body does far more work than the dispatch. This is the
shape `MoveGraph` already chose and it was the right call.

### 4.4 What is deliberately not in the interface

| Absent | Why |
|---|---|
| world-space positions | §5 — the frame belongs to the owner |
| any notion of an agent | the graph answers about *space*; who is moving is the caller's business |
| path storage | the planner owns its output |
| a transform | a graph does not know where it is; something above it does |
| mutation | invalidation is backend-specific and happens at the boundary that owns the geometry |

---

## 5. Frames — one line of contract

> **`position()` returns coordinates in the representation's own frame. The
> owner holds the transform.**

For `Lattice` and a static FPS level that transform is identity and its
existence is invisible. For a ship's hull it is the hull's world matrix, and
that single fact is what lets a navmesh ride a moving ship without regenerating
— the mechanism both Kythera and Mercuna ship, and the one Keen wrote by hand
for grids ([`moving_frame_navigation.md`](moving_frame_navigation.md) §3).

It costs nothing today and cannot be retrofitted once world-space positions have
leaked into callers.

**Deferred, explicitly:** a `{graph, node}` location type for querying across
several frames at once, and dynamic off-mesh links between frames. Frame
*transitions* are unsolved industry-wide and the pragmatic answer is a scripted
traversal, not a navigation query
([`moving_frame_navigation.md`](moving_frame_navigation.md) §6). Do not build
for boarding until a project has boarding.

---

## 6. Algorithms, and what each requires

### 6.1 The mapping

| Algorithm | Requires | Produces | Valid on |
|---|---|---|---|
| **A\*** | `NavGraph` + heuristic | `NodeId` path | all four |
| **Coarse-first / hierarchical** | two graphs + a node mapping | `NodeId` path | Lattice+Area, NavMesh+Area |
| **Dijkstra → `CostField`** | `DenseGraph` | cost + predecessor per node | Lattice, Area, Volume |
| **Flow bake** | a `CostField` | 1 byte/node direction | as above |
| **Funnel / string-pull** | polygon portals | corner list | **NavMesh only** |
| **Reactive following** | a path + an LOS test | a direction | Area especially — L4D's method |
| **ORCA / boids** | positions + `SpatialHash` | a velocity | **no graph at all** |

Two observations that justify the whole arrangement. **A\* is universal** — it
needs nothing a graph cannot provide, which is why one planner serves four
backends. **Funnel is not** — it needs portal geometry that only a polygon mesh
has, which is why path realisation is a separate, per-backend concern (§6.3) and
not a method on the graph.

### 6.2 Consumers

- **`PathPlanner`** — A* over any `NavGraph`. Owns its open list and scratch,
  reused across queries. **Sliced/resumable from day one** (§8).
- **`FieldBuilder` → `CostField`** — multi-source Dijkstra over a `DenseGraph`.
  `ReachField` in this game already *is* a `CostField`; the movement-range field
  and a flow field are the same computation with different budgets.
- **`FlowField`** — baked down from a `CostField`, 1 byte/node, the only thing
  agents touch per frame (§8).

### 6.3 `PathRealiser` — the seam between a path and movement

A `NodeId` path is not movement, and the conversion is where the backends
diverge hardest:

| Variant | For | Produces |
|---|---|---|
| **Identity** | Lattice | the cell sequence, unchanged — this game needs nothing else |
| **Funnel** | NavMesh | string-pulled corner list |
| **Direction-only** | AreaGraph | "generally that way" — the furthest unobstructed node ahead, re-evaluated per frame ([`crowd_scale.md`](crowd_scale.md) §1.2) |

Keeping this separate is what lets the coarse representation stay coarse. L4D's
mesh works *because* the realiser refuses to give a precise route and forces the
movement layer to own the navigating.

### 6.4 Steering is a peer, not a consumer

```
cromwell/steer/   ← position, velocity, desired direction, neighbours (SpatialHash)
cromwell/nav/     ← optional; supplies the desired direction when it exists
```

`Steering` must not take a `NavGraph`. A ship in open space has no floor, no
graph and no nodes, but it steers — if steering depends on navigation it cannot
serve that case at all
([`moving_frame_navigation.md`](moving_frame_navigation.md) §7.3). When
navigation exists it feeds steering a direction; when it does not, an AI goal
feeds it directly, and nothing downstream can tell which happened.

---

## 7. The plug-in / plug-out matrix

The point of all of the above. Each row constructs only what it needs.

| Project | Representation(s) | Planner | Field | Realiser | Steering | Recast linked |
|---|---|---|---|---|---|---|
| **This game** (tactics) | Lattice | ✓ | ✓ *(movement range)* | identity | — | **no** |
| **FPS / third-person** | NavMesh | ✓ | — | funnel | ✓ | yes |
| **RTS** | AreaGraph **+** NavMesh | ✓ *(heroes, on the mesh)* | ✓ *(mass, on areas)* | both | ✓ | yes |
| **Horde / L4D-like** | AreaGraph | — | ✓ | direction-only | ✓ | no |
| **Space — flying** | — none — | — | — | — | ✓ | **no** |
| **Space — walkable ships** | NavMesh *(hull-local)* **+** none outside | ✓ | — | funnel | ✓ | yes |

Three things this table is meant to prove:

1. **Row 1 links no Recast and constructs no steering.** "Have none" is the
   absence of a constructor call, not a configuration.
2. **Row 3 runs two representations concurrently**, serving different agents in
   the same frame. That is the requirement §1 exists for.
3. **Row 5 uses `steer/` with nothing from `nav/` at all**, which is only
   possible because of §6.4.

---

## 8. The five constraints, and why each is now-or-never

| | Constraint | Free now / expensive later because |
|---|---|---|
| **1** | `NavGraph` methods `const` and safe for **concurrent readers**; every scrap of mutable search state in a per-thread query object | This is what makes N simultaneous path queries embarrassingly parallel. Detour's `dtNavMeshQuery` is exactly this object. Retrofitting means finding every cached scratch member across every backend. |
| **2** | The planner is **sliced and resumable** | A resumable search is a *shape*. Retrofitting resumability into a running recursive A* is a rewrite. It is also what actually prevents frame spikes, which is a different goal from throughput. |
| **3** | `CostField` (9 B/node, written once) is a **separate type** from `FlowField` (1 B/node, read per agent per frame) | 9× fetch reduction on the only path that runs 500×/frame. Once callers reach into the fat struct, splitting it touches all of them. |
| **4** | Scratch owned by the query object and reused; `steps()` fills a **caller-supplied** vector | No allocation in the hot loop. Already the shape `MoveGraph` chose. |
| **5** | `position()` is **graph-local** (§5) | Retrofitting means auditing every caller for a frame assumption that was invisible when it was made. |

Everything else — Morton node ordering, GPU flow fields, transform caching — is
deliberately *not* decided here, because `CLAUDE.md` says a measurement should
say so first ([`moving_frame_navigation.md`](moving_frame_navigation.md) §8.5).

**Prerequisite, and it is not a navigation task:** constraint 1 buys nothing
without a job system, and `cromwell` has none. It is an engine primitive that
rendering and streaming want too — `nav/` must not grow a private thread pool.
enkiTS (zlib) is the candidate.

---

## 9. Layout

```
cromwell/nav/graph/     NavGraph, DenseGraph, NodeId, Step, QueryContext
cromwell/nav/search/    PathPlanner  (A*, sliced)
cromwell/nav/field/     CostField, FlowField, FieldBuilder
cromwell/nav/path/      PathRealiser + variants
cromwell/nav/area/      AreaGraph — the L4D-style grower          [later]
cromwell/nav/volume/    VolumeGraph — SVO                          [later]
cromwell/nav/recast/    Recast/Detour backend    [separate CMake target, optional]
cromwell/steer/         peer of nav, not under it                  [later]
```

Four units under `nav/` at first, inside the 6–7 threshold, and mirroring
`game/movement/{graph,search,occupancy}/` so the two read the same way.

**Recast is a separate CMake target behind an option.** The nav core has zero
dependency on it; this game never links it. That keeps `game_core`'s fast
no-raylib build fast, and it is the same discipline `CLAUDE.md` already applies
to the renderer.

---

## 10. Not building

| | Why |
|---|---|
| A navmesh generator | Recast is zlib and a voxelising generator is a project, not a feature. Keen reached the same split — library for the static world, hand-written only for the part that had to move ([`moving_frame_navigation.md`](moving_frame_navigation.md) §4.1). |
| One system for all genres | [`navigation.md`](navigation.md) §13. The attempt serves none of them. |
| Anything for agent counts nobody has | Flow fields answer a problem this project does not have. Build them with the RTS. |
| Frame transitions / boarding | §5. Unsolved industry-wide; scripted traversal is the pragmatic answer. |
| An archetype ECS for agents | `CLAUDE.md` is explicit — the cost is in the queries each entity makes, not in iterating them. |
| GPU flow fields | Needs steering on the GPU too or the readback eats the win. Whole-pipeline decision, not a nav one. |

---

## 11. Build order, and how each step is checked

1. **Interface headers** — `nav/graph/`. Cold, no dependencies, no behaviour.
   *Checked by:* nothing yet. This step is a claim, and step 2 tests it.
2. **Lift `PathPlanner` + `CostField`; make `Lattice` implement `DenseGraph`.**
   *Checked by:* the game's search results must be identical before and after,
   and `xcom_perf` must not regress. **This is the real test of the design** — an
   interface with no implementations is a guess, and this is the only
   implementation available to falsify it.
3. **Job system** (`enkiTS`) — engine-wide, not a nav task. Unblocks constraint 1.
4. **Per project, later:** `recast/`, `area/`, `volume/`, `steer/`.

Step 2 costs real work and **this game gains nothing from it**. That is the
honest trade: it is paid to find out whether the interface survives contact with
its only implementation, before three more depend on it.

---

## 12. Open questions

Written down so they are not silently decided by whoever types first.

- **Does `Lattice` fit `DenseGraph` cleanly?** Its connectivity comes from
  height deltas across a column, so one cell can offer up to `kMaxMovesPerCell`
  = 64 steps and a "node" may not be a cell. If a node has to become
  (cell, surface), the dense index is still fine but `NodeId` packing needs
  deciding. **Step 2 answers this, and it is the main risk to the design.**
- **Where does the graph↔transform pairing live?** Not in `cromwell/nav/` (a
  graph does not know where it is) and not in `game/` (an FPS needs it too).
  Probably an entity-layer component. Undecided, and deferred with §5.
- **Does `AreaGraph` need to be a `DenseGraph`?** Almost certainly yes for flow
  fields, but rectangle growing produces areas that split and merge on
  invalidation, so stable dense indices need a free-list. Not hard, not decided.
- **Heuristic admissibility across backends.** Octile on a lattice with
  `MoveKind` costs is not obviously admissible once ladders and portals are in.
  Inadmissible A* is fine for games but should be a decision, not an accident.
