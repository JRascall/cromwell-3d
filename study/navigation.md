# Navigation and spatial queries — reference notes

Working notes on how shipped engines answer two questions that get confused for
each other, and what cromwell should own if it is to serve an RTS, an FPS and a
third-person game rather than only this tile game.

> **Three companion notes go deeper on individual games**, and this one is the
> layer underneath all of them:
> [`crowd_scale.md`](crowd_scale.md) (AC Unity, World War Z, Left 4 Dead — when
> the problem is *agent count*), [`battle_scale.md`](battle_scale.md) (Total War,
> Men of War — when it is *simulation depth*), and
> [`map_scale.md`](map_scale.md) (R.U.S.E., Wargame — when it is *extent*).
> Those three axes are independent, and a technique from one is often useless in
> another.

Everything marked **[EPIC]** is from Epic's own documentation or public roadmap.
**[VALVE]** is from the Valve Developer Community wiki. **[RECAST]** is from the
Recast/Detour project's own documentation. **[PAPER]** is peer-reviewed.
**[COMMUNITY]** is developer write-ups and forum reports — useful for what
actually hurts in practice, weaker than a primary source. **[inferred]** is our
reading, not anybody's word.

---

## 1. The one thing that matters most

**"What is near this point" and "how do I walk from A to B" are two different
systems with two different data structures, and conflating them is the most
common way an engine's navigation ends up wrong.**

| | Spatial index | Navigation |
|---|---|---|
| Question | what entities are near XYZ | what route gets me from A to B |
| Holds | entities, by position | walkable surface, by connectivity |
| Changes | every frame, as things move | rarely, when geometry changes |
| Used by | perception, targeting, overlap, audio, LOD, interest management | pathfinding, steering |
| Structure | hash grid / uniform grid / BVH | navmesh / grid / lane graph / flow field |

A spatial hash will never tell you a wall is in the way. A navmesh will never
tell you which five enemies are within 20 metres. An engine needs both, and they
should not know about each other.

**This matters for the question that prompted these notes.** A spatial hash is
worth building for cromwell — but not *because* of navigation. It is worth it
because `Entity` today has a `Vec3` and no index over it, so "what is near this
point" has no answer but a walk over every entity. Navigation is a separate
build, described from §4 down.

---

## 2. Why the tile game does not need the spatial index, and the engine does

`game/` already has a spatial index and it cost nothing: the lattice **is** one.
A position maps to a cell by rounding down, and a cell maps to an array slot by
one multiply-add (`Lattice::index`). That is the whole service an RTS spatial
hash provides, available for free because the coordinates are already integers.
`OccupancyGrid` is the contents array laid over it.

An engine cannot assume that. cromwell's `Entity` holds a world-space `Vec3` and
the header is explicit that this is deliberate — *"The engine must not learn
what a tile is."* So cromwell needs the general structure the lattice happens to
give the game for free.

**[inferred]** This is the correct shape: the game gets the cheap exact answer
because its world is discrete, the engine gets the general one, and neither is
built on the other.

---

## 3. Which spatial structure, and why

The three candidates, and what each is actually good at:

| Structure | Good at | Bad at |
|---|---|---|
| **Uniform grid** | many small, evenly spread dynamic objects — RTS units, particles | bounded worlds only; wasted memory when sparse; mixed object sizes |
| **Spatial hash** | the same, but unbounded and sparse — an open world that is mostly empty | pathological clustering into one bucket |
| **BVH** | mixed-size, mostly-static geometry; ray queries | dynamic updates — rebuilding is often cheaper than refitting |

**[COMMUNITY]** The consensus is to match the structure to the data
distribution and the object-size variance, not to pick a favourite: a BVH over a
uniform particle field is slower than a grid, and a grid over mixed-size objects
hits the large-object problem, where one big entity has to be inserted into
every cell it overlaps.

### The recommendation for cromwell

**A spatial hash grid for dynamic entities. Not a BVH, not an octree.**

Reasons, in order of weight:

1. **Unbounded.** An engine cannot require the world size up front. A hash takes
   any coordinate; a dense grid needs extents at construction. For a project
   that wants to support open worlds this is decisive on its own.
2. **Sparse-friendly.** Open worlds are mostly empty. A hash spends memory on
   occupied cells only.
3. **Movement is the common case.** Entities move every frame. Insert, move and
   query are all O(1) with a small constant; a tree pays a rebalance.
4. **One tuning knob.** Cell size, chosen as roughly the median query radius.

The known failure — everything clustering into one bucket — is a real risk in a
game where the whole army stands in one place. It is also cheap to detect: track
the longest bucket and log when it passes a threshold.

**What it does not solve.** Large static geometry, and ray queries against it.
That wants a BVH, and it is a *second* structure with a different lifetime, not
a replacement. **[inferred]** Build the hash first; add the BVH when there is
static geometry to query, not before.

---

## 4. Navigation is four layers, and mixing them is the mistake

No shipped engine has one navigation system. They have a stack, and the reason
one engine can serve several genres is that the layers swap independently.

| Layer | Job | Typical implementations |
|---|---|---|
| **1. Representation** | where is walkable, and what connects | navmesh, tile grid, lane graph, voxel |
| **2. Global planner** | a route across the representation | A*, hierarchical A* for long paths |
| **3. Local steering** | follow the route, avoid what moved since | RVO/ORCA, Detour Crowd, context steering |
| **4. Group movement** | move N units to one place without a traffic jam | flow fields, formations |

A first-person game needs 1–3 and can skip 4. An RTS needs 4 badly and can often
get away with a coarse 1. This tile game has its own 1 and 2 already
(`MoveGraph` + `Pathfinder`) and needs neither 3 nor 4.

**[inferred] `MoveGraph` is already the right abstraction, at the wrong altitude.**
Its interface is `neighbors(from, blocked, out)` and `isTraversable(cell)` —
which is exactly a layer-1 representation with a layer-2 search on top, and
`Pathfinder` searching that interface and nothing else is why one Dijkstra
already serves both infantry and vehicles. If cromwell ever owns navigation,
this is the shape the engine-side interface should take, with the lattice as one
implementation and a navmesh as another.

---

## 5. What Unreal does — and exactly where it stops scaling

### The representation

Unreal's navmesh is **Recast/Detour**, Epic's fork of the industry-standard
toolset. **[RECAST]** Recast generates the mesh by voxelising world geometry;
Detour does runtime loading, pathfinding and queries.

Recast builds either a **solo** or a **tiled** navmesh. **[RECAST]** Solo meshes
suit *"simple, static cases and are easy to work with"*; tiled meshes are
*"more complex to work with but better support larger, more dynamic
environments"* and are what enable *"re-baking, hierarchical path-planning, and
navmesh data-streaming"*. **DetourTileCache** exists specifically for *"large
levels and open-world games"* — it keeps a compressed heightfield per tile so a
temporary obstacle can be added and the tile rebuilt without going back to
source geometry. **[COMMUNITY]** The cost is a little extra memory per loaded
tile, generally reported as moderate.

### The big-world story

**[EPIC]** World Partition splits the world into a grid of cells streamed by
distance from a streaming source. A **World Partitioned Navmesh** follows: the
navmesh is split into chunk actors loaded and unloaded like any other
partitioned resource. Runtime modes build on a **base navmesh** — the version
streamed in with the cells — which stays fixed in Static mode, while Dynamic
modes let navigation-relevant actors dirty it and trigger tile rebuilds.

Two limits are documented rather than folklore:

- **[EPIC]** Dynamic tile building is **limited to the loaded space**. Nothing
  outside streamed-in cells rebuilds.
- **[EPIC]** Navigation dirtiness is **deliberately ignored** for objects that
  load and unload as part of the base navmesh — otherwise streaming itself would
  dirty the world continuously.

### Where it actually hurts

**[COMMUNITY]** The recurring complaints are tile-boundary discontinuities —
small gaps between triangles of neighbouring tiles, because each tile is built
independently with no cross-tile post-process, and runtime rebuilds can
reintroduce misalignment — and hitching on dynamic rebuilds, where most of the
wait is recalculating the navmesh.

**[EPIC]** Epic's own public roadmap still lists **"Navmesh Generation for Large
Worlds"** as *experimental*. That is the most honest available answer to "does
Unreal's navmesh scale to big worlds": Epic does not yet consider it finished.

### The part that answers the real question

**[EPIC] [COMMUNITY]** For large agent *counts*, Epic did not scale the navmesh.
They built a different system. **Mass Entity** is UE5's built-in ECS; **MassAI**
sits on it; and navigation for mass agents runs on **ZoneGraph** — authored lane
networks, not a navmesh. Zone graphs are explicitly *not* a navmesh replacement:
you author where crowds should flow and entities follow those lanes. Behaviour
moves from Behavior Trees to **StateTree** for the same reason — per-entity BTs
are prohibitively expensive at scale.

**[inferred] This is the single most useful data point in these notes.** Epic
has more navigation engineering than this project will ever have, and when they
needed thousands of agents their answer was *stop pathfinding per agent and
follow authored lanes instead*. Anyone planning "navmesh, but for 10,000 units"
is planning something Epic chose not to do.

---

## 6. What Source 2 does — and why it is not the reference here

Source 2 is this project's rendering reference (see `source2_rendering.md`). It
is **not** a big-world navigation reference, and it is worth being explicit
about that so the good rendering research does not get over-extended.

**[VALVE]** Dota 2's navigation mesh controls all unit movement and defines
where units can and cannot go. It is a **2D plane generated top-down**, a grid
of **64×64-unit cells** covering the whole map. It is generated from materials:
a surface is walkable if its material carries the attribute `dota.nav.walkable 1`.

**[VALVE] [COMMUNITY]** Movement runs **two pathfinders at once**:

| | Considers | Structure |
|---|---|---|
| **Long pather** | static blockers only — terrain, trees | fixed grid, classic RTS A* |
| **Short pather** | stationary units, local avoidance | not grid-based |

The long pather finds the route; the short pather follows it while avoiding what
the long pather ignored. That is layers 2 and 3 from §4, cleanly separated.

**[inferred]** Note what this is: a **grid**, not a navmesh, over a small
hand-authored fixed map. Dota and CS2 maps do not stream, do not change size and
are measured in hundreds of metres. Source 2 has no answer to open-world
navigation because none of its shipped games asked the question. Take the
two-pather split — that idea is excellent and genre-independent — and take
nothing else from here about scale.

---

## 7. RTS is a different problem, and flow fields are the answer

**[COMMUNITY]** Modern RTS group movement is flocking over **flow fields**, an
approach popularised by Supreme Commander 2's implementation of *Continuum
Crowds*. A flow field is a grid where every cell stores a direction vector
pointing the cheapest way to the destination.

The economics are the whole point:

- A* costs **per agent**. 200 units to one place is 200 searches.
- A flow field costs **per destination**. 200 units to one place is one field.

**[COMMUNITY]** The initial cost for a *single* agent is higher than a navmesh
query, so this is a bad trade below roughly a hundred agents and an increasingly
good one above it. The reported weakness is large maps: a field covering a huge
world is expensive to compute and mostly wasted, so practical systems compute
fields locally around the units or over a coarse grid.

**[COMMUNITY]** The other thing flow fields buy is the case traditional
pathfinding handles worst — two groups moving *through* each other, which with
per-agent A* plus avoidance degenerates into a jam.

---

## 8. Swarms — coarse mesh, strong steering

Left 4 Dead and World War Z are the reference for "lots of entities", and they
agree on the shape: **make the navigation representation coarse and cheap, and
put the effort into the local layer.**

### Left 4 Dead

**[VALVE] [COMMUNITY]** The nav mesh is inherited from Counter-Strike: Source.
It is not a triangle mesh — every area is an **axis-aligned quad**, and the
generator *"tracks walkable areas from the map and then grows rectangles in a
way that best fits"* them, with bidirectional links between neighbours. Paths
are plain A* over that graph, and the result is *"an ordered sequence of
navigation rectangles rather than a precise point-to-point route."*

The important sentence, because it is the whole design: **[COMMUNITY]** bots
*"extract a rough direction from the pathfinding system, not an exact guide on
how to get from point A to point B."* The imprecision is deliberate — it forces
the movement layer to own the actual navigating.

**[COMMUNITY]** Path following is **reactive** and frame-by-frame: rather than
aiming at the furthest reachable rectangle, an agent picks *"the unobstructed
node further down the path in the direction the bot is facing"*, which keeps
movement fluid and avoids constant repathing. Movement runs as a separate phase
that decides how to cross between rectangles — jumping, climbing, crouching —
and several systems submit movement requests through a priority mechanism.

**[inferred]** So L4D is §4's four layers with layer 1 deliberately crude and
layer 3 doing the work. The mesh is also the AI Director's spawn-placement data,
which is a second reason to keep it coarse: it is queried as *regions*, not as a
surface.

### World War Z

**[COMMUNITY]** Saber built the **Swarm Engine** rather than licensing one, and
it targets around **500 zombies on screen at once**. The horde *"moves as one,
then splits off as it acquires targets"* — described by the developers as
behaving like a school of fish — with individuals breaking away near the player.

The behaviours it is known for are emergent from local rules rather than
authored: zombies pile into narrow corridors, climb over each other to reach
higher ground, and form ramps out of their own bodies. **[inferred]** None of
that is pathfinding. A navmesh cannot represent "stand on your friend"; it comes
out of a dense local simulation where agents collide, push and stack.

### What the research says makes this affordable

**[PAPER] [COMMUNITY]** Crowd avoidance — ORCA and its RVO ancestors — is
naively O(n²), because every agent considers every other. The standard fixes are
both spatial:

1. **Spatial binning.** Discretise into bins and only consider agents in the
   same or neighbouring bins. This is the thing that changes the complexity
   class; published GPU implementations reach 100,000 agents in real time with
   ORCA plus spatial partitioning, and uniform-grid subdivision is reported as
   the single largest win for boids.
2. **Neighbour limiting.** Cap the number considered at some *k*, on the
   observation that people avoid only a handful of others at a time. Bounds the
   worst case when a crowd bunches up.

### Measured here

`xcom_perf` times exactly this question against cromwell's `SpatialHash` — every
agent asking "who is near me" once, in a 60×60 space with a 2-unit avoidance
radius:

| agents | every-pair | spatial hash | speed-up |
|---|---|---|---|
| 100 | 0.011 ms | 0.018 ms | **0.6x — slower** |
| 500 | 0.282 ms | 0.098 ms | 2.9x |
| 2000 | 4.474 ms | 0.675 ms | 6.6x |
| 5000 | 31.008 ms | 3.967 ms | 7.8x |

**[inferred]** Two things worth reading off that table. The crossover is around
**a hundred agents** — below it the index is genuine overhead and the honest
answer is not to use one. And at 5000 the every-pair loop costs **31 ms, nearly
two whole 60Hz frames**, for one subsystem; that is the wall, and it arrives
suddenly.

---

## 9. Building a swarm — the blueprint

**[inferred]** Drawn from §8 and the sources under it. Nothing here is built;
this is the design to come back to, so the research does not have to happen
twice.

### How entities get into the index

There is **no registration and no deregistration**, deliberately.
`SpatialHash` has `clear()` and `insert()` and nothing else:

```cpp
/* once per frame, by whoever owns the entity list */
hash.clear();
for (Entity& e : entities)
    hash.insert(e.id(), e.location());
```

| The usual concept | What it is here |
|---|---|
| register | inserted this frame |
| deregister | not inserted next frame |
| update position | the rebuild — there is no separate update |
| stale entry | cannot exist; the index lives one frame |

For a crowd this is not a compromise, it is the cheaper option: everything moves
every frame anyway, so an incremental path would pay unlink-and-relink for every
agent and get nothing back. The rebuild is a linear write over contiguous
memory, which is the access pattern hardware is fastest at, and it is included
in every timing in §8 — the 5000-agent row rebuilds the whole index and still
comes in 7.8x ahead.

**Static things go in a second index**, rebuilt when that set changes rather
than per frame. Cover markers, spawn points, props. Two indexes with obvious
lifetimes beat one with a removal protocol.

**[inferred] The gap this exposes is not in the hash.** cromwell has `Entity`
but nothing that *owns a set of entities* — no level, no world, no registry. So
the rebuild loop above has nowhere to live yet, and the id passed to `insert` has
no engine-wide meaning (the game's ids are `UnitRoster` indices). That container
is the real prerequisite for using this, and it should be built when something
needs it rather than invented now.

### The four layers, for a horde

Following §4, with what each becomes at swarm scale:

| Layer | For a swarm | Why |
|---|---|---|
| **1. Representation** | coarse — L4D's grown rectangles, or a tile grid | it only has to supply a *direction*; §8's key quote |
| **2. Global route** | one **flow field per destination**, not one path per agent | 500 zombies chasing one player is one field, computed once |
| **3. Local steering** | boids/ORCA over the spatial hash, with a neighbour cap | this is where the behaviour actually comes from |
| **4. Group movement** | folded into 2 — the flow field *is* the group layer | |

**[inferred] The economics are the whole design.** A horde shares a destination.
That collapses the expensive layer — pathfinding — from per-agent to
per-destination, and leaves the per-agent cost as steering, which the spatial
hash makes linear. Both of the things that would have been quadratic are gone,
and neither was removed by making the navigation mesh better.

### What to steal, specifically

- **A direction, not a path** (L4D). The route layer returns "generally that
  way"; the steering layer works out how. This is what makes a coarse mesh
  sufficient and stops repathing dominating.
- **Reactive following** (L4D). Aim at the furthest *unobstructed* node ahead in
  the direction already faced, re-evaluated per frame, rather than walking
  waypoints in order.
- **Steering is a separate phase** (L4D). Several systems submit movement
  requests and a priority mechanism resolves them — which is what lets "avoid
  neighbours", "follow the field" and "climb that" coexist without one
  hard-coding the others.
- **Neighbour cap** (crowd research). Consider at most *k* neighbours, nearest
  first. Bounds the worst case exactly when a crowd bunches — which is the
  moment the naive version falls over.
- **AI level of detail.** Distant agents steer less often, or not at all. The
  think-interval staggering already in `Component` is the mechanism.

### Do not build a navmesh generator for this

**[inferred]** A horde needs the *coarsest* representation of the four layers.
Writing a voxelising mesh generator to serve the layer that matters least is the
clearest possible misallocation. Wrap Recast if a real mesh is ever needed, and
for a tile or grid world skip layer 1 entirely — the grid is already it.

---

## 10. Can the GPU do this? Yes, with one caveat that matters

**[PAPER]** Charlton et al., *Fast Simulation of Crowd Collision Avoidance*
(CGI 2019), runs ORCA on the GPU with a specialised linear-program solver and
spatial partitioning for the neighbour search. It reports **over 100,000 agents
in real time at 60fps**, and **up to 30x** a multi-core CPU implementation.

So the number is real and it is peer-reviewed.

**[inferred] The caveat: that is the SIMULATION only.** The paper addresses the
cost of the avoidance model. It says nothing about drawing 100,000 animated
characters, and drawing them is the harder half.

### Which is the actual ceiling

**[COMMUNITY]** Rendering large animated crowds is a solved-ish but separate
problem, and the technique is GPU skinning plus instancing — bone matrices baked
into a texture and looked up in the vertex shader, so each instance can sit at a
different frame of a different animation. Reported results:

| | Agents | Note |
|---|---|---|
| Skinned instancing (GPU Gems 3) | ~10,000 | independently animating, 30fps |
| Assassin's Creed Unity | 10,000 on screen | but see §10.1 — only **40** have real AI |
| Spider-Man (PS4) | 100+ | ~0.5 ms CPU for all crowd animation |
| Horizon Zero Dawn | 30–50 | but 200 bones each — quality, not count |

**[inferred] Which puts World War Z's ~500 in context: that is a quality
decision, not a technical ceiling.** Ten thousand animated agents is achievable
and shipped. Five hundred *high-fidelity* ones — full bone counts, ragdolls,
gore, per-agent physics interactions and climbing — is a different budget
entirely. The count a game advertises is set by what each agent is allowed to
do, not by how many the hardware can move.

### 10.1 The two costs of skeletal animation, which are not the same cost

**[inferred]** "Skeletal animation is expensive because it runs on the CPU" is
half true, and the half that is wrong is the half people optimise. There are two
separate costs and they live in different places:

| | What it is | Scales with | Runs on |
|---|---|---|---|
| **Skinning** | transform each vertex by its bone matrices | **vertices** × influences | **GPU**, since ~2002 |
| **Pose evaluation** | sample curves, blend clips, apply IK, produce the bone matrices | **bones** × active clips | **CPU**, usually |

**[COMMUNITY]** Skinning on the CPU is indeed hopeless — 100 characters at 50
bones and 5000 vertices is 25M vertex transforms per frame, about 60 ms, roughly
3.6x over a 60Hz budget. Moving it to a vertex shader takes it under 1 ms.

But that has been standard for two decades. **[inferred] Matrix-palette skinning
in the vertex shader is not an optimisation any more, it is just how skinning
works** — so it is not what limits a crowd. What limits a crowd is the *other*
cost: evaluating poses. And that scales with **bone count**, not vertex count.

**[COMMUNITY]** Which is also why "move animation to the GPU" is not automatic
advice — bone interpolation, blending, layering and IK on worker threads works
well, and offloading whole animation *sequences* to the GPU pays off mainly in
the massive-instancing case a crowd actually is.

### 10.2 So how did Unity get 10,000?

**[COMMUNITY]** From Francois Cournoyer's GDC 2015 talk, *Massive Crowd on
Assassin's Creed Unity: AI Recycling* — and the answer is that almost none of
those ten thousand were what the number implies:

| | Count |
|---|---|
| Real AI brains | **40** |
| High-resolution models | **120** |
| Crowd NPCs on screen | **10,000** |

The rest are puppets driven by a deliberately simplistic brain, and a pooling
system swaps a low-res NPC for a high-res one as the player approaches, timed so
the swap is not noticed. That is the *AI recycling* of the title.

**The animation half is the same idea applied to the skeleton.** **[COMMUNITY]**
NPCs near the player carried around **300 bones**, acted independently and
reacted in full detail; distant ones ran on **11 bones** and moved in simplified
groups following shared rules.

**[inferred] Eleven bones is the whole answer to the question.** Given §10.1 —
that the CPU cost scales with bones and not vertices — cutting 300 to 11 is
roughly a 27x reduction in exactly the thing that costs, and eleven is about
spine, head, two arms and two legs: plenty to read as a walking person from
forty metres. They did not make skeletal animation faster. They made 9,880 of
the characters barely have a skeleton.

**[inferred] The transferable lesson, and it is the same one as §11.3:** the way
to ten thousand is not a faster animation system, it is being ruthless about
what the other nine thousand eight hundred are permitted to be. Both AC Unity
and the two-population split arrive at the same structure from different
directions — a small, expensive, interactive set near the player, and a large,
cheap, non-interactive one everywhere else, with promotion between them.

### What this means here

cromwell already has compute (`gpu/compute/ComputeShader.hpp`, and a self-test
proving the dispatch path). So the GPU route is available rather than
theoretical. But **[inferred]** the order is not in doubt:

1. **CPU steering over the spatial hash first.** §8 measures 5000 agents'
   neighbour queries at 4 ms. A horde of hundreds — WWZ's actual target — is
   comfortably inside a frame with no GPU work at all.
2. **Move to compute only when a measurement demands it**, and expect the
   awkward part to be getting results *back* — a readback stalls, so the design
   that works keeps the agent state resident on the GPU and never round-trips.
3. **Assume rendering becomes the limit before simulation does.** The 100k
   figure is avoidance maths on a GPU with nothing to draw.

---

## 11. The full stack, if the horde is actually built

**[inferred]** The obvious architecture is: simulate on the GPU with compute,
animate with vertex animation textures, drop to billboards at distance, draw it
all instanced. That is broadly right, and it is roughly what shipped crowd
renderers do. Three things about it are not obvious, and each one decides
whether the result is a game or a screensaver.

### 11.1 A GPU sim does NOT use `SpatialHash`

Worth being blunt, because it is the natural assumption. `SpatialHash` is a
chained hash walked by CPU pointer-chasing. That structure is actively bad on a
GPU: divergent chain lengths per thread, random access, no coalescing.

**[COMMUNITY] [PAPER]** The GPU equivalent is a **sorted uniform grid**, and the
shape is standard across fluid sim, particles and crowds:

| Pass | What it does |
|---|---|
| 1 | one thread per agent — compute its cell id |
| 2 | **sort the agent buffer by cell id** (bitonic sort, or counting sort with a parallel prefix scan) |
| 3 | one pass to find each cell's **start and end index** in the now-sorted buffer |
| 4 | neighbour query = read the contiguous span for your cell and its 8/26 neighbours |

The point is that after the sort, an agent's neighbours are *adjacent in memory*,
so the neighbour read is coalesced. That is the entire trick, and it is why the
technique reports **O(n) behaviour scaling to a million particles** at
interactive rates.

**[COMMUNITY]** A worked reference is `wayne-wu/webgpu-crowd-simulation` —
position-based dynamics with a Jacobi solver, five compute passes per frame
(velocity planning → neighbour search → stability solve → constraint solve →
velocity finalise), with the hash grid built by GPU bitonic sort exactly as
above.

**[inferred] So the two structures coexist rather than compete**, and §11.3 is
where the split falls out.

### 11.2 The question that decides the architecture: how does the player shoot one?

**[inferred]** This is the crux, and it is not a rendering problem.

If agents live in a GPU buffer, the CPU does not know where any of them are.
Every gameplay interaction — a bullet hitting a specific zombie, a grenade
killing forty, one climbing onto a specific ledge, aggro, damage numbers — needs
agent state on the CPU. Reading it back stalls the pipeline, and doing that
every frame throws away everything the GPU bought.

The three ways out, in increasing order of how well they work:

1. **Read back everything each frame.** Simple, and it caps you around where the
   CPU sim would have anyway. Pointless.
2. **Read back late and asynchronously**, a frame or two behind. Workable for
   soft things — aggro, counts, density — useless for a hitscan bullet that must
   resolve this frame.
3. **Split the population.** The one that actually ships.

### 11.3 Two populations, not one

**[inferred]** The design that makes all of this work:

| | Gameplay agents | Ambient horde |
|---|---|---|
| Count | tens | thousands |
| Lives on | CPU | GPU, resident, never read back |
| Sim | full steering, pathing, hit reactions | position-based dynamics, flow field, avoidance |
| Can be shot? | yes, individually | no — or only via a coarse proxy |
| Neighbour search | `SpatialHash` | sorted uniform grid, §11.1 |
| Animation | real skinned mesh, ragdoll, IK | VAT or imposter |

Agents **promote** to the gameplay population as they approach the player and
**demote** when they leave. The player only ever interacts with a few dozen at
once — everything else is a moving backdrop that reads as a horde.

**[inferred] This is what makes the CPU hash relevant to a WWZ-style game after
all**: not for the horde, for the promoted set. And a few dozen agents is *below*
the crossover measured in §8, so even there it is the wrong tool — a flat array
would do. `SpatialHash` earns its place in this game shape at the *boundary*
work: which ambient agents are close enough to promote, what is near an
explosion, what a sound reaches.

### 11.4 The LOD ladder — and one real conflict in it

**[COMMUNITY]** VAT bakes animation into a texture read in the vertex shader, so
the mesh becomes a plain static mesh with no skeleton. Cheap and effective. Its
documented limits: texture memory, **no animated collision**, little real-time
interactivity, and quality needs 16-bit or float textures — 8-bit shows
artefacts on slow animations.

**The conflict worth knowing before committing:** **[COMMUNITY]** VAT is indexed
by vertex id, so **mesh LODs and VAT do not compose** — changing the vertex count
invalidates the texture. The workaround is a **separate VAT bake per LOD level**,
which works and costs memory. It is not a blocker, but it is a surprise if it is
discovered late.

**[inferred] Also worth flipping the usual ordering: VAT is a MID-range
technique, not a close-up one.** A zombie near the player has to react to being
shot, ragdoll when it dies, and blend between animations. VAT can do none of
those — it plays baked loops. So:

| Range | Representation | Why |
|---|---|---|
| Near | real skinned mesh, bones, ragdoll, hit reactions | it must interact |
| Mid | **VAT**, instanced | baked loops read fine; nothing touches it |
| Far | **imposter**, instanced | 8 triangles |
| Very far | cull, or one crowd-density effect | |

**[COMMUNITY]** On the far end, the distinction is worth getting right:
*billboards* are fixed-perspective sprite planes (reported as 8 cards, 72
triangles); *imposters* blend a single sprite from the viewer's angle out of a
baked octahedral atlas — 8 triangles, 9 vertices, and a smoother result. Total
War uses untextured generic models at distance for the same purpose. **[COMMUNITY]**
Creative Assembly's own account of Warhammer II's scale is that LOD alone only
*partially* solved it, and the rest came from moving work across CPU threads.

### 11.5 What the evidence says the bottleneck will be

**[COMMUNITY]** The WebGPU crowd project's own conclusion is that with complex
meshes **rendering, not compute, becomes the bottleneck** — and its throughput
charts peak in the *low hundreds* of agents once real meshes are drawn.

That is the same conclusion §10 reaches from the other direction: the 100k
figure is avoidance maths with nothing to draw. **[inferred] Plan the budget
around the renderer, and treat the simulation as the cheap half.** A horde that
simulates beautifully and cannot be drawn is the common failure here, not the
reverse.

---

## 12. So: do these scale to big worlds?

Separating the two things that "scale" can mean:

| What grows | Solved? | By what |
|---|---|---|
| **World size** | **Yes** | tiled navmesh + streaming (DetourTileCache, World Partition chunks). Mature, if fiddly. |
| **Path length** | **Yes** | hierarchical pathfinding — cluster the mesh, search clusters, refine inside. **[PAPER]** HPA* for grids; HNA* extends it to navmeshes via multilevel k-way partitioning with cached sub-paths. |
| **Geometry changing at runtime** | **Partly** | tile cache rebuilds are quick but not free; Unreal still hitches, and its large-world generation is experimental. |
| **Agent count** | **No, not with navmesh** | this is where everyone changes system — lanes (ZoneGraph) or flow fields (RTS). |

**[inferred] The honest summary: world size is a solved problem, agent count is
not — and they are usually confused for each other.** "Does it scale to big
worlds" almost always turns out to mean "will it survive a thousand agents",
and the answer there is that no navmesh-based system does, which is why Epic and
every RTS built something else for that case.

---

## 13. What this means for cromwell

**[inferred]** Ordered by what earns its place soonest.

### Built — the spatial index

`cromwell/spatial/SpatialHash.hpp`. A hash grid over positions with
`clear` / `insert` / `queryRadius` / `queryBox`, rebuilt per frame rather than
updated incrementally (the header argues that trade). Checked against a
brute-force scan in `tests/SpatialHashTests.cpp`, and timed in `xcom_perf`.

It pays for itself the first time anything asks "what is near me" — perception,
targeting, triggers, audio emitters, LOD selection — and it is the structure
§8 identifies as the one that makes a crowd affordable at all.

`game/` does not use it, because the lattice already answers the same question
exactly. That is the correct outcome, not a wasted build: the engine cannot
assume a discrete world, and the game should not pay for generality it does not
need.

### Design now, build later — the navigation interface

Do not implement navigation until a project needs it. Do settle the *shape*,
because it is the thing that is expensive to change later, and `MoveGraph`
already shows what it looks like: a representation that enumerates legal moves,
and a planner that knows nothing else.

Four layers, swappable independently:

1. **Representation** — an interface. `Lattice` implements it here; a navmesh
   implements it in an FPS; a lane graph implements it for crowds.
2. **Planner** — A* over that interface, hierarchical when paths get long.
3. **Steering** — only when there is continuous movement. This tile game moves
   cell to cell and needs none.
4. **Group movement** — only for the RTS. Flow fields, layered *over* 1.

### Do not build

- **A navmesh generator.** Recast is the industry standard, it is zlib-licensed,
  and writing a voxelising mesh generator is a project, not a feature. If
  cromwell ever needs a navmesh, it should wrap Recast/Detour rather than
  reinvent it.
- **One navigation system for all three genres.** §4 is the whole point. The
  attempt is what produces a system that serves none of them.
- **Anything for agent counts nobody has yet.** Flow fields are the right answer
  to a problem this project does not have. Build them with the RTS.

---

## Sources

**Epic / Unreal**
- [World Partitioned Navigation Mesh](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partitioned-navigation-mesh) — chunk actors, base navmesh, generation modes, the two documented dynamic limits
- [World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine) — the streaming grid navmesh chunks follow
- [Navmesh Generation for Large Worlds (Experimental)](https://portal.productboard.com/epicgames/1-unreal-engine-public-roadmap/c/1482-navmesh-generation-for-large-worlds-experimental) — public roadmap status
- [Dynamic NavMesh Performance](https://forums.unrealengine.com/t/dynamic-navmesh-performance/327496) and [World partitioned level and navmesh](https://forums.unrealengine.com/t/world-partitioned-level-and-navmesh/2675004) — the practitioner reports behind §5's "where it hurts"

**Recast / Detour**
- [Recast Navigation](https://recastnav.com/) — solo vs tiled, DetourTileCache, DetourCrowd
- [recastnavigation on GitHub](https://github.com/recastnavigation/recastnavigation)

**Valve / Source 2**
- [Dota 2 Navigation Mesh](https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Dota/Navigation_Mesh) — 64×64 grid, material-driven generation
- [Dota 2 Pathfinding](https://liquipedia.net/dota2/Pathfinding) — the long/short pather split

**Mass / crowds**
- [Zone Graphs in Unreal Engine](https://christiansantori.com/blog/zone-graphs-unreal-engine-crowd-navigation) and [Mass Entity: Purposeful Crowds](https://christiansantori.com/blog/mass-entity-unreal-engine-purposeful-crowds)
- [Mass AI and 10,000 NPCs at 60fps](https://www.strayspark.studio/blog/crowd-traffic-simulation-ue5-mass-ai)

**Hierarchical pathfinding**
- [Hierarchical path-finding for Navigation Meshes (HNA*)](https://www.sciencedirect.com/science/article/abs/pii/S0097849316300668) — navmesh clustering via MLkP
- [Improvements to Hierarchical Pathfinding for Navigation Meshes](https://www.cs.upc.edu/~npelechano/MIG2017_Rahmani.pdf) (PDF)
- [Near Optimal Hierarchical Path-Finding (HPA*)](https://citeseerx.ist.psu.edu/document?doi=b0f0432ba69e4d730b93a75e3d19c8e9d811efac) — Botea, Müller

**Flow fields**
- [RTS Pathfinding: Flowfields](https://www.jdxdev.com/blog/2020/05/03/flowfields/)
- [Hybrid Vector Field Pathfinding](http://www2.cs.uregina.ca/~anima/408/Notes/Crowds/HybridVectorFieldPathfinding.htm) — the Continuum Crowds lineage

**Swarms and crowds**
- [L4D Nav Meshes](https://developer.valvesoftware.com/wiki/L4D_Level_Design/Nav_Meshes) — axis-aligned quads, the Director's use of them
- [AI navigation in Left 4 Dead](https://gamingme.wordpress.com/2010/02/25/ai-navigation-in-left-4-dead-part-i/) — rectangle growing, reactive path following, the movement/pathfinding split
- [There's a Hole in Your NavMesh, Dear Zombie](http://aigamedev.com/open/articles/hole-navmesh-dear-zombie/)
- [Swarm Engine](https://www.moddb.com/engines/swarm-engine) and [World War Z: six things to know](https://gameinformer.com/preview/2019/03/29/six-things-to-know-about-world-war-z) — the 500-agent target and the school-of-fish description
- [Fast Simulation of Crowd Collision Avoidance](https://eprints.whiterose.ac.uk/id/eprint/150111/1/_John_Charlton____ORCA_GPU_Paper.pdf) (PDF) — ORCA with spatial partitioning at scale
- [A Neighborhood Grid Data Structure for Massive 3D Crowd Simulation on GPU](https://www.researchgate.net/publication/228731597_A_Neighborhood_Grid_Data_Structure_for_Massive_3D_Crowd_Simulation_on_GPU)
- [Uniform spatial subdivision to improve the Boids Algorithm](https://www.ijarnd.com/manuscripts/v3i10/V3I10-1144.pdf) (PDF)

**GPU crowds**
- [Fast Simulation of Crowd Collision Avoidance](https://arxiv.org/abs/1908.10107) — Charlton et al., CGI 2019. ORCA on GPU, 100k agents at 60fps, up to 30x multi-core CPU. **Simulation only — no rendering.**
- [webgpu-crowd-simulation](https://github.com/wayne-wu/webgpu-crowd-simulation) — position-based dynamics, five compute passes per frame, hash grid built by GPU bitonic sort. Concludes rendering is the bottleneck with complex meshes.
- [A Toolkit for Computation on GPUs (GPU Gems ch.37)](https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-37-toolkit-computation-gpus) — uniform grid neighbour search, the n² to constant argument

**Crowd rendering**
- [Massive Crowd on Assassin's Creed Unity: AI Recycling](https://gdcvault.com/play/1022411/Massive-Crowd-on-Assassin-s) — Cournoyer, GDC 2015. The 40 / 120 / 10,000 split and the 300-vs-11 bone LOD in §10.2. [Free recording](https://archive.org/details/GDC2015Cournoyer), [Game Developer write-up](https://www.gamedeveloper.com/programming/video-behind-the-massive-crowds-of-i-assassin-s-creed-unity-i-)
- [Animated Crowd Rendering (GPU Gems 3 ch.2)](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-2-animated-crowd-rendering) — skinned instancing, bone matrices in a texture, ~10,000 characters
- [Techniques for Skeletal-Based Animation in Massive Crowd Simulations](https://www.mdpi.com/2073-431X/11/2/21)
- [Real-Time Large Crowd Rendering with Efficient Character and Instance Management on GPU](https://onlinelibrary.wiley.com/doi/10.1155/2019/1792304)
- [GPU skinning notes](https://github.com/raduacg/game-mechanics-optimizations/blob/main/53_gpu_skinning.md) — the shipped-title figures in §10
- [Designing Total War: Warhammer II to handle tons of units](https://www.gamedeveloper.com/design/designing-i-total-war-warhammer-ii-i-to-handle-tons-of-units-and-massive-battles)

**Vertex animation textures and imposters**
- [Labs Vertex Animation Textures 3.0](https://www.sidefx.com/docs/houdini/nodes/out/labs--vertex_animation_textures-3.0.html) and [OpenVAT](https://openvat.org/)
- [Vertex Animation and LODs](https://forums.unrealengine.com/t/vertex-animation-and-lods/233479) — the vertex-count conflict in §11.4
- [Impostor Baker Plugin](https://dev.epicgames.com/documentation/en-us/unreal-engine/impostor-baker-plugin-in-unreal-engine)
- [Imposters versus billboards](https://www.linkedin.com/advice/0/what-pros-cons-using-impostors-billboards-versus-other) — the triangle counts in §11.4

**Spatial hashing**
- Teschner, Heidelberger, Müller, Pomeranets & Gross, *Optimized Spatial Hashing for Collision Detection of Deformable Objects* (VMV 2003) — the three-prime hash `SpatialHash.cpp` uses

**Spatial partitioning**
- [Spatial Partition — Game Programming Patterns](https://gameprogrammingpatterns.com/spatial-partition.html)
