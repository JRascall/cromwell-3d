# Navigation in moving reference frames and open volume — reference notes

Working notes on the two cases that break every off-the-shelf navigation system:
**the floor is moving**, and **there is no floor**. A space game with flyable,
walkable, boardable ships needs both at once, and neither is what Recast was
built for.

> **This is the third layer down.** [`navigation.md`](navigation.md) is the base
> note — two systems, four layers, where navmeshes stop scaling.
> [`crowd_scale.md`](crowd_scale.md) covers agent count. This one covers the
> *frame* the navigation data lives in, which those two both assume is the world
> and stationary.

Everything marked **[KEEN]** is from Space Engineers' released source or Keen's
own dev diaries. **[VENDOR]** is middleware marketing and product documentation —
Kythera and Mercuna — which is **the weakest tag in this file**: it describes
capability without cost, and neither vendor has published a talk with numbers.
**[BOOK]** is a *Game AI Pro* chapter (curated practitioner writing, edited but
not peer-reviewed). **[PAPER]** is peer-reviewed or a preprint, marked as such.
**[RECAST]** is the Recast/Detour project's own documentation. **[COMMUNITY]** is
developer write-ups and forum reports. **[inferred]** is our reading, not
anybody's word.

---

## 1. Three problems, routinely conflated

"Navigating around a moving ship" is at least three different problems, and they
have three different answers. Almost every forum thread about this fails because
it picks one answer and applies it to all three.

| | Problem | Frame | What it needs |
|---|---|---|---|
| **A** | Fly *around* a ship from outside | world, obstacle moves | volumetric representation, or none at all |
| **B** | Walk *on or inside* a ship that is moving | ship-local | navmesh in local space + a transform |
| **C** | Cross *between* ships, or ship to station | two frames at once | frame handoff — **the hard one** |

**[inferred]** A and B are solved problems with known answers. C is where
shipped games still visibly struggle, and it is not a representation problem —
it is a *steering and continuity* problem that the representation cannot fix.

---

## 2. The methods, and what each is actually for

The full menu, with verdicts. Only two of these are right, and one of them is
"do nothing".

| # | Method | Verdict |
|---|---|---|
| **M1** | **Rebake the navmesh each frame** as the ship moves | **Never.** A tiled Recast rebuild is milliseconds; a ship moves every frame. This is the naive answer and it is not close to viable. |
| **M2** | **Bake once in local space, transform on query** | **The answer for B.** What both middleware vendors sell and what Keen wrote by hand. §3. |
| **M3** | **Attach the agent to the platform**, simulate entirely in local space | **The answer for B's *movement* half.** Complements M2 — M2 fixes the data, M3 fixes the physics. |
| **M4** | **World-space navmesh + dynamic carving/obstacles** (Unity `NavMeshObstacle`, Detour `dtTileCache`) | **For slow, rare movers only** — doors, rubble, a drawbridge. Re-triangulating locally at runtime is affordable occasionally, not continuously. **[COMMUNITY]** |
| **M5** | **Waypoint graph in local space** | **The cheap version of M2.** No triangulation, hand-or-auto-placed nodes. Barotrauma ships this. §5.2. |
| **M6** | **No representation at all** — pure steering | **The answer for open space.** No floor, no graph, no nodes. §7.3. |
| **M7** | **Volumetric octree (SVO)** | **The answer for A when you need real routing** — around asteroids, through a hangar mouth. §7. |

**[inferred]** Note that M2 and M6 are not alternatives — a space game with
walkable ships needs M2 *inside* the hulls and M6 *outside* them, live at the
same time, with M3 gluing an agent to whichever hull it is standing on. That is
the concrete argument for representations being constructed objects rather than
a global system with a mode.

---

## 3. Local-space navmesh + transform — how it actually works

### 3.1 The mechanism

**[VENDOR]** Kythera's stated approach is the clearest description of it:

> *"The NavMesh used by NPCs aboard a ship moves with the ship without requiring
> any regeneration ... achieved by keeping the coordinate system in synch with
> the frame of reference of the actor it's linked to — updated through a 4x4
> matrix once per frame."*

That is the whole trick. The navmesh never moves. **The query moves into the
mesh's frame, and the answer moves back out.** Per frame the cost is one matrix
update per platform; per query it is two transforms. Nothing regenerates,
because nothing in the mesh's own frame has changed.

**[VENDOR]** Mercuna sells the same thing for ground navigation — *"attach
navmesh to moving platforms, enabling agents to pathfind across large, moving
objects such as the deck of a rocking ship."* Two independent middleware vendors
converging on one mechanism is about as strong a signal as this subject offers.

**[KEEN]** And Space Engineers arrived at it independently in its own code:
`MyGridNavigationMesh` *"operates in grid local coordinates"*, with `FindPath`
taking start and end points in that local space and `GlobalToLocal()` /
`LocalToGlobal()` bridging. §4.1.

### 3.2 What this means for an interface, concretely

**[inferred]** The design consequence is one line, and it is very cheap now and
very expensive later:

> **A navigation representation's `position(node)` returns coordinates in the
> representation's own frame, never world space. The owner holds the transform.**

For a tile game or a static FPS level that transform is identity and nobody ever
notices it exists. For a ship it is the hull's world matrix. Getting this wrong
means world-space positions leak into every caller, and unpicking that after
three backends exist is the expensive kind of change.

### 3.3 What it does *not* solve

Being honest about the boundary, because the vendor material is not:

- **Steering.** An agent's velocity is meaningless unless it is expressed in the
  same frame as the mesh. Avoidance run in world space on a ship doing 200 m/s
  sees every agent aboard moving at 200 m/s in the same direction and concludes
  nothing is about to collide. Steering must run **local**, which is M3.
- **Anything spanning two frames.** §6.
- **Deformation.** The mesh is rigid in its own frame. A hull that bends, or a
  ship that loses a section, needs a rebake of the affected tiles like any other
  geometry change — the transform trick buys nothing there.

---

## 4. Space Engineers — the best-sourced case, because the source is public

SE1's code was released, which makes it the only primary-source implementation
in this note. **[KEEN]**

### 4.1 SE1 runs *two* navigation systems, and the split is the lesson

| | `MyGridNavigationMesh` | `MyNavmeshManager` / `MyRDPathfinding` |
|---|---|---|
| For | cube grids — ships, stations | voxels — planets, asteroids |
| Frame | **grid-local** | world, gravity-aligned |
| Built from | triangulating navigable surfaces on blocks; `AddTriangle()`, `RegisterTriangle()` per cube position | Recast/Detour (`m_rdWrapper`) over an OBB tile, Y aligned to gravity |
| Generation | incremental as blocks change; `MakeStatic()` finalises | async tiles, `MAX_TILES_TO_GENERATE = 7` per path request |
| Invalidation | per-cube | `VoxelMapRangeChanged()` → `InvalidateArea()`, drops coords from `m_coordsAlreadyGenerated` |

**[inferred] The important fact is that Keen used Recast for the voxel world and
hand-wrote the grid one.** Recast could bake a ship's interior perfectly well —
it is ordinary geometry. What Recast cannot do is *belong to a moving frame*.
So the system that had to move got written by hand, and the system that sits
still got the library. That is precisely the buy/build line this note is trying
to draw, discovered by a shipped game a decade ago.

**[KEEN]** The tile manager is also a usable reference for the parallelism
question in §8: generation runs off the main thread via
`ParallelTasks.Parallel.Start()`, one tile per update, with
`m_navmeshTileGenerationRunning` as a single-flight guard.

### 4.2 Why SE1's NPCs are nonetheless bad

**[COMMUNITY]** Worth recording, because it is a warning rather than a
technique. SE1's NPCs *"have been neglected since they broke in 2018"*, though
spiders worked in 2015–17. The stated difficulty is *"implementing AI pathing on
a grid that players could be changing virtually without limit."*

**[inferred]** That is not a failure of the local-frame approach — it is the
*other* problem, unbounded runtime mutation, and it is the same thing Gears
Tactics hit ([`gears_tactics.md`](gears_tactics.md) §2.2: content provoking
navmesh rebuilds). A fully player-editable ship is the worst case for any baked
representation, and the incremental-per-cube design in §4.1 is the mitigation,
not a cure.

### 4.3 SE2, as of 2026

**[KEEN]** Early and honest: Marek Rosa's dev diaries report *"We've started our
first experiments with AI pathfinding"*, with behaviour trees running in the
VRAGE3 editor and NPC work targeted at VS4. Demonstrated so far: coming to the
player, following, basic patrolling, patrolling with obstacle avoidance, plus
separate videos on **voxel surface navigation** and a **3D flight navigation**
project. NPCs have cover use, peek-and-shoot, and an alert meter that responds
to gunshots.

**[inferred]** Nothing published yet says how SE2 handles a navmesh on a moving
grid, and the "first experiments" framing suggests it is not solved there yet
either. **Do not treat SE2 as a reference implementation** — treat it as
confirmation that the problem is still live for a studio that has shipped in
this exact space for a decade.

**[KEEN]** One later finding sharpens this, and it cuts against §9's "write it"
verdict. Jan Hloušek's 2023 VRAGE3 engineering post lists **Kythera AI** among
the middleware VRAGE3 adopted, alongside DirectX 12, Havok and FMOD — and
Kythera is the vendor whose moving-navmesh description §3.1 quotes.

**[inferred] So Keen, having hand-written a grid-local navmesh for SE1 (§4.1),
licensed the commercial version for SE2.** A studio with a shipped
implementation and a decade of domain expertise chose to buy rather than
re-build, which is the strongest evidence available that §9's "the one
genuinely unavoidable build" is also a genuinely expensive one. Two caveats:
the post is from **2023** and middleware choices change over a four-year
development, and SE2's 2026 diaries describe behaviour trees authored in the
VRAGE3 editor, which reads like in-house behaviour over bought navigation but is
equally consistent with the choice having changed. **State that Keen listed
Kythera in 2023, not that SE2 ships it.**
[`space_engineers.md`](space_engineers.md) §7.2 carries the same note.

---

## 5. What everyone else does

### 5.1 Sea of Thieves — custom system, hooked into the engine's framework

**[RARE]** Rare hit the wall head-on: *"navigation meshes don't support that"*
for open water. UE4 was chosen partly for *"the most robust set of AI tools
available in a commercial engine"*, and they then used its source access to
*"build their own custom water navigation system and then hook it into the
existing navigation framework."*

**[inferred]** That last clause is the reusable idea, and it is exactly the
`NavGraph` interface argument: they did not fork the AI stack, they wrote a new
*representation* behind the existing interface, and behaviour trees and steering
carried on unchanged. Note the sourcing limit — the published part 1 does not
detail how skeletons handle a rocking deck, and says only that they *"use the
same controls as humans."*

### 5.2 Barotrauma — the cheap version, and it works

**[COMMUNITY]** Submarines move; AI aboard them navigates a **waypoint graph in
submarine-local space**, auto-generated in the sub editor with manual fixups.
Doors and ladders get explicit waypoints; exterior navigation gets a sparse loop
around the hull.

**[inferred]** This is M5, and it is worth taking seriously as a *first*
implementation rather than a lesser one. A hand-placeable local graph gets a
moving ship navigable for a fraction of the work of a triangulating baker, and
the documented failure modes are all annotation problems (bots cutting corners
because a waypoint fell inside their detection radius), not architectural ones.

### 5.3 Space Station 14 — chunked graph over a moving grid

**[COMMUNITY]** Grids *are* the moving object in SS14 (players call them
shuttles). Notable for the coarsening decision: rather than pathfind per tile,
which *"would be prohibitively expensive"*, **groups of tiles on the same chunk
are turned into nodes** and the search runs over those.

**[inferred]** Independent arrival at the same conclusion as
[`navigation.md`](navigation.md) §8 — coarsen the representation, and this time
from a codebase where the grid is already the natural fine representation and
they *still* went coarser.

### 5.4 Unity and Unreal, out of the box

**[COMMUNITY]** Neither supports it. The standard advice is M4 (obstacle
carving) or "parent the agent and fake it", and the forum threads on moving
platforms are perennial. Both engines' navmeshes are world-space by
construction. **[RECAST]** Upstream Recast/Detour has no moving-platform concept
either — the closest primitive is off-mesh connections, which are static links.

---

## 6. The transition problem — the part nobody has made easy

**[VENDOR]** Kythera claims the strongest position here: *"library functions
enabling navigation meshes to merge, allowing for exciting gameplay moments such
as boarding sequences between moving ships"*, and that when a ship docks,
*"NPCs can move back and forth between the ship and shore with no special
handling, and without expensively regenerating large areas."*

**[inferred]** Take that as a statement of the *goal*, not a recipe — no
mechanism, cost or failure mode is published. What the problem decomposes into
is at least four things, and only the first is representation:

1. **A link between two frames** — dynamic off-mesh connections, created and
   destroyed as ships come into contact. Detour's off-mesh links are static, so
   this needs building.
2. **Deciding when a link is valid** — two hulls in contact, relative velocity
   under a threshold, the gap crossable. This is a physics query, not a nav one.
3. **The handoff itself** — the frame an agent belongs to changes mid-step.
   Position, velocity and the path's remaining nodes all have to convert, and
   the conversion must not produce a visible pop.
4. **Steering across the seam** — for the duration of the crossing the agent is
   in neither frame cleanly. **[inferred]** This is where it goes wrong in
   practice, and it is why boarding in shipped games is so often an animation
   rather than a navigation event.

**[inferred] The pragmatic recommendation is (4): make the crossing a scripted
traversal, not a navigation query.** A boarding animation that owns the agent
for its duration sidesteps the entire continuity problem, and it is what an
off-mesh link is *already* for — a link says "get from here to there by some
means the path planner does not model." Making that means "play the boarding
action" is consistent with how ladders and jumps already work.

---

## 7. When there is no floor at all

### 7.1 Sparse voxel octrees — the published technique

**[BOOK]** Daniel Brewer, *3D Flight Navigation Using Sparse Voxel Octrees*
(Game AI Pro 3, ch. 21) is the reference, and it shipped in **Warframe**.

The structure: recursively subdivide the world into cubic nodes; **only keep
children where collision geometry exists**, so a node containing nothing has no
children and stays one large node. Largest voxels serve coarse pathfinding
across the whole volume, down to leaves sized to the agent radius. A* is
modified to search this adaptive-resolution graph.

**[inferred]** The reason it works is entirely §1's first rule from
`CLAUDE.md` — it does less work. Open space is nearly all empty, so the sparse
octree collapses the vast majority of the volume into a handful of huge nodes,
and the search never subdivides where nothing is. A uniform 3D grid over the
same volume is unaffordable at any useful resolution; this is the same trade the
lattice already makes in 2.5D, taken one dimension up.

**[VENDOR]** Mercuna sells the commercial version — *"full volumetric
pathfinding through an octree ... maps the entire navigable space of the level
and allows unconstrained navigation across in three dimensions"*, with cuboidal
obstacle representation specifically so *"long, thin objects such as capital
ships"* fit their bounds properly.

**[inferred]** That last detail is worth stealing regardless: a sphere bound on
a kilometre-long capital ship is a catastrophic over-estimate, and in a space
game most of the interesting obstacles are long and thin.

### 7.2 It fits the graph interface

**[inferred]** An SVO node is a node. It has neighbours, it has a cost, it has a
position, and it is densely indexable if you number the nodes. So a volumetric
octree **implements the same representation interface as a tile lattice and a
navmesh**, and everything above it — the planner, the hierarchical search, even
flow fields — works unchanged. This was not designed for; it is a genuine
confirmation that the interface is cut at the right place.

### 7.3 And most of the time, use nothing

**[inferred]** A ship flying between two points in empty space needs no
representation whatsoever. Desired direction from an AI goal, steering to
follow it, spatial index for neighbour avoidance, done. The volumetric octree
earns its place only where routing genuinely matters — an asteroid field, a
station's interior approach, a canyon.

**This is the case that proves steering must not depend on navigation.** If the
steering layer takes a graph, it cannot serve open space at all. Navigation
supplies a direction when it exists; when it does not, the goal supplies one
directly, and nothing downstream can tell the difference.

---

## 8. Optimisation and parallelism

### 8.1 What is actually hot

**[inferred]** Applying `CLAUDE.md`'s own test — how many times does this really
run?

| Hot | Cold |
|---|---|
| A* node expansion (per node, per query) | navmesh bake |
| Flow-field *sampling* (per agent, per frame) | octree build |
| Steering + neighbour queries (per agent, per frame) | graph construction, link setup |
| Frame transforms (per query, per platform) | invalidation bookkeeping |
| Flow-field *build* (per destination — **not** per agent) | anything on level load |

The bakes are cold **even though they are expensive**, because they run on load
or on a geometry change. They want *parallelism*, not micro-optimisation — those
are different asks and §8.4 handles them separately.

### 8.2 Do less work — the wins that are orders of magnitude

1. **Flow field instead of per-agent A\*.** [`navigation.md`](navigation.md)
   §9's economics: a shared destination collapses pathfinding from per-agent to
   per-destination. **[BOOK]** Elijah Emerson's *Crowd Pathfinding and Steering
   Using Flow Field Tiles* (Game AI Pro, ch. 23 — the Supreme Commander 2
   technique) is the canonical write-up, and the *tiles* part matters: the field
   is built per tile so only tiles the crowd will actually enter get built.
2. **Coarse-first, refine inside.** Search the coarse graph, expand the fine one
   only inside the winning corridor. SS14 (§5.3) does exactly this and says so.
3. **Sparse octree over uniform 3D grid** for volume. §7.1.
4. **Sliced, resumable search.** **[RECAST]** Detour ships
   `initSlicedFindPath`/`updateSlicedFindPath` for this. **[inferred]** This is
   the one to design in rather than add: a resumable search is a *shape*, and
   retrofitting resumability into a running A* is painful. It is also what
   actually prevents frame spikes, which is a different goal from throughput.

### 8.3 Layout — one structural point

**[inferred]** A cost field and a flow field are **different products with
different lifetimes**, and merging them is the mistake.

| | Written | Read | Holds | Size |
|---|---|---|---|---|
| **Cost field** | once, by the search | rarely — path reconstruction | cost + predecessor + arrival kind | ~9 B/node |
| **Flow field** | once, baked down from the cost field | **per agent, per frame** | a packed direction index | **1 B/node** |

`ReachField` in this project is already the first of those, exactly. The second
does not exist yet and should not be bolted onto it — agents asking "which way?"
500 times a frame should touch one byte per node, not nine. This is the same
derived-cache pattern `CLAUDE.md` documents at 2 bytes/cell, and the same rule
applies: the summary must be derivable from the source and tested against it.

### 8.4 What parallelises, and what does not

| | Parallelises | How |
|---|---|---|
| **Many independent path queries** | **Yes, embarrassingly** | requires read-only graph + all mutable search state in a per-thread query object. **[RECAST]** `dtNavMeshQuery` is exactly this object, and it is why Detour scales across threads at all. |
| **Navmesh / octree bake, per tile** | **Yes** | tiles are independent. **[KEEN]** SE1 does this — `ParallelTasks.Parallel.Start()` per tile, single-flight guarded. |
| **N flow fields for N destinations** | **Yes, trivially** | far more practical than parallelising one field. |
| **Steering** | **Yes** | parallel-for with a strict read-then-commit split: read all positions, compute all velocities, *then* write. The spatial hash is built once and read by every thread. |
| **One flow field (Dijkstra)** | **Partly** | uniform-cost fields parallelise as a wavefront; non-uniform needs bucketing. Do this last, if ever. |
| **A single A\* query** | **No** | parallel A* is a research topic with poor speedup. Don't. |

**[PAPER]** A 2026 preprint on a multi-threaded Recast-based framework reports
*"350+ FPS with 1000 simultaneous agents"* against a single-threaded baseline
that *"degraded below 20 FPS at 200 agents"* — a **~4.5× improvement factor**,
with path computation at 8.98 ms over 5,328 nodes on complex 3D terrain. Treat
the absolute numbers as indicative only (preprint, unspecified hardware), but
the shape — near-linear scaling on independent queries — matches the theory and
Detour's design.

**[inferred] The prerequisite nobody mentions: none of this is real without a
job system**, and there is none in `cromwell` today. It is an engine-level
primitive that rendering and streaming want too, and navigation must not grow
its own thread pool. §9 has the candidates.

### 8.5 Measure first

**[inferred]** Deliberately *not* recommended without a measurement, per
`CLAUDE.md`'s standing rule:

- **Morton / tiled node ordering** on grid graphs. The ±width neighbour jump is
  the cache miss, but row-major may well be fine at this project's sizes.
- **GPU flow fields.** [`navigation.md`](navigation.md) §10 has the precedent at
  100k agents, but a field computed on the GPU needs a direction *per agent*,
  which means readback — the exact trap `CLAUDE.md` names. It only pays if
  steering is on the GPU too, which is a whole-pipeline decision.
- **Per-frame transform caching** for platforms. One matrix multiply per query
  is probably beneath measurement; find out before building a cache for it.

---

## 9. Libraries — what to take, what to write

Constraint: open source, permissive enough to sell a closed-source game. That
rules out GPL and, for practical purposes, anything LGPL-static. It does **not**
rule out zlib, MIT, BSD or Apache 2.0.

| Layer | Take | Licence | Verdict |
|---|---|---|---|
| **Navmesh bake + query** | [Recast & Detour](https://github.com/recastnavigation/recastnavigation) | **zlib** | **Take it.** No credible alternative exists in C++ — every "alternative" found is a port of it to another language. Dependency-free, vendors as a subdirectory. |
| **Job system** | [enkiTS](https://github.com/dougbinks/enkiTS) | **zlib** | **Take it.** Lightweight, C++11, written for a game engine (Avoyd). Alternatives: [Taskflow](https://github.com/taskflow/taskflow) (MIT, heavier, more general) and [Intel GTS](https://github.com/GameTechDev/GTS-GamesTaskScheduler) (MIT). enkiTS is the closest fit to this codebase's size and style. |
| **Local avoidance / ORCA** | [RVO2](https://github.com/snape/RVO2) | **Apache 2.0** | **Take it, or read it.** UNC's reference implementation, C++98, 2D (a 3D variant exists). Apache 2.0 is sellable — it does carry a NOTICE-preservation requirement and a patent grant, which zlib does not. Worth reading even if the steering ends up bespoke. |
| **Coarse area graph** (L4D-style) | — | — | **Write it.** Flood-fill walkable, grow rectangles, link neighbours. A few hundred lines, and inherently game-specific — Valve wrote theirs, Epic wrote ZoneGraph. Nothing to take. |
| **Flow fields** | — | — | **Write it.** Small, and the shape must match the graph interface. **[BOOK]** Emerson's chapter is the spec. Existing repos are all engine-bound (Unity/UE). |
| **Volumetric 3D / SVO** | — | — | **Write it, reluctantly.** Searched: there is **no engine-agnostic open-source C++ SVO navigation library**. Everything is a UE or Unity plugin ([UESVONavigation](https://github.com/TheEmidee/UESVONavigation), [Nav3D](https://github.com/darbycostello/Nav3D)) or a book chapter. **[BOOK]** Brewer's chapter is a complete enough spec to implement from, and the UE plugins are readable references. |
| **Moving-frame support** | — | — | **Write it — nothing offers this.** Confirmed: not in Recast/Detour, not in Unity, not in Unreal. The only products that have it are **Kythera** and **Mercuna**, both commercial closed-source middleware. This is the one genuinely unavoidable build, and §3.2 says it is nearly free if the interface is designed for it up front. |

**[inferred] The pattern is worth naming:** everything that is *general* has a
library, and everything that touches the *frame* has to be written. That is not
a coincidence — a library cannot know what owns your transforms.

---

## 10. What this means for cromwell

**[inferred]** Ordered, and nothing here contradicts
[`navigation.md`](navigation.md) §13 — it refines it with two things that note
did not have.

**The two additions:**

1. **`position(node)` is representation-local, not world.** §3.2. One line of
   contract, free now, expensive later. Identity transform for the tile game and
   an FPS level; the hull matrix for a ship.
2. **Steering is a peer of navigation, not a consumer of it.** §7.3. It takes a
   direction, a position and neighbours — never a graph. This is what lets a
   space game use steering with no navigation at all, and it is the arrangement
   that stops `nav/` becoming mandatory.

**Unchanged from §13, and reconfirmed by this note:**

- Do not write a navmesh generator. §9. Keen's own split (§4.1) is the
  independent confirmation — they took Recast for the world and wrote by hand
  only the part that had to move.
- Do not build one navigation system for all genres. This note adds a fourth
  representation (volumetric) to the three §13 already listed, and they share an
  interface, not an implementation.
- Do not build for agent counts nobody has yet.

**New prerequisite, and it is not a navigation task:**

- **A job system in `cromwell`.** §8.4. Nothing under `src/cromwell/` currently
  does threading, and every parallelism win in this note depends on one.
  enkiTS (zlib) is the recommended candidate. It should land as an engine
  primitive serving rendering and streaming too — navigation must not grow a
  private thread pool.

**Deliberately deferred:**

- Frame transitions (§6). Hard, unsolved industry-wide, and the pragmatic answer
  is a scripted traversal rather than a navigation query. Do not design for it
  until a project has boarding.
- Volumetric navigation (§7). Only once something needs to route through a
  non-empty volume. The steering-only path (§7.3) covers open space today.

---

## Sources

**Space Engineers**
- [`MyNavmeshManager.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Game/AI/Pathfinding/MyNavmeshManager.cs) — released source; tile generation, async, voxel invalidation, the Recast/Detour wrapper
- [`MyGridNavigationMesh`](https://fresc81.github.io/SpaceEngineers/class_sandbox_1_1_game_1_1_a_i_1_1_pathfinding_1_1_my_grid_navigation_mesh.html) and [`MyRDPathfinding`](https://fresc81.github.io/SpaceEngineers/class_sandbox_1_1_game_1_1_a_i_1_1_pathfinding_1_1_my_r_d_pathfinding.html) — API reference for the grid-local mesh
- [Marek's Dev Diary, March 26 2026](https://blog.marekrosa.org/2026/03/mareks-dev-diary-march-26-2026/) and [SE2 Dev Diary, May 21 2026](https://steamcommunity.com/games/1133870/announcements/detail/702143076369432617) — SE2 NPC/pathfinding state
- [Character NPCs](https://support.keenswh.com/spaceengineers/pc/topic/character-npcs) — the community record of SE1's NPC decay

**Middleware (vendor material — capability without cost)**
- [How to Create Dynamic Game Play on Moving Platforms](https://kythera.ai/news/how-to-create-dynamic-game-play-on-moving-platforms/) — Kythera's moving navmesh, the 4×4 matrix, navmesh merging
- [Kythera AI](https://www.kythera.ai/) and [Kythera — Star Citizen Wiki](https://starcitizen.tools/Kythera)
- [Mercuna Ground Navigation — Features](https://mercuna.com/ground-navigation/features/) and [3D Navigation](https://mercuna.com/3d-navigation/) — moving platforms; volumetric octree

**Other shipped games**
- [Building a Pirate's Paradise: The AI of Sea of Thieves](https://www.gamedeveloper.com/programming/building-a-pirate-s-paradise-the-ai-of-sea-of-thieves-part-1-) — custom water navigation hooked into UE4's framework
- [Barotrauma Submarine Editor](https://regalis11.github.io/BaroModDoc/Editors/SubmarineEditor.html) and [bot pathfinding threads](https://steamcommunity.com/app/602960/discussions/0/600784815203113775/) — local-space waypoint graphs
- [SS14 Grids](https://docs.spacestation14.com/en/robust-toolbox/transform/grids.html) — chunk-node coarsening over moving grids

**Technique**
- [3D Flight Navigation Using Sparse Voxel Octrees](https://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter21_3D_Flight_Navigation_Using_Sparse_Voxel_Octrees.pdf) — Daniel Brewer, Game AI Pro 3 ch. 21 (Warframe)
- [Crowd Pathfinding and Steering Using Flow Field Tiles](https://www.gameaipro.com/GameAIPro/GameAIPro_Chapter23_Crowd_Pathfinding_and_Steering_Using_Flow_Field_Tiles.pdf) — Elijah Emerson, Game AI Pro ch. 23 (Supreme Commander 2)
- [Advanced Techniques for Robust, Efficient Crowds](https://www.gameaipro.com/GameAIPro2/GameAIPro2_Chapter17_Advanced_Techniques_for_Robust_Efficient_Crowds.pdf) — Graham Pentheny, Game AI Pro 2 ch. 17
- [Multi-threaded Recast-Based A* Pathfinding](https://arxiv.org/html/2602.04130v1) — 2026 preprint; thread pool architecture and the 1000-agent figures

**Libraries**
- [Recast & Detour](https://github.com/recastnavigation/recastnavigation) (zlib) · [enkiTS](https://github.com/dougbinks/enkiTS) (zlib) · [RVO2](https://github.com/snape/RVO2) (Apache 2.0) · [Taskflow](https://github.com/taskflow/taskflow) (MIT) · [Intel GTS](https://github.com/GameTechDev/GTS-GamesTaskScheduler) (MIT)
- [UESVONavigation](https://github.com/TheEmidee/UESVONavigation) · [Nav3D](https://github.com/darbycostello/Nav3D) — UE-bound SVO references
