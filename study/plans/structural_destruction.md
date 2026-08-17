# Structural destruction — a build plan

**A design note, not research.** The research is
[`red_faction_guerrilla_destruction.md`](../games/shooters/red_faction_guerrilla_destruction.md),
which reads GeoMod 2.0 out of the retail install; this note is the decision
about what *this* engine should do with it, in what order, and what was
deliberately not done. Written after a scoping conversation, 2026-08-17.

**The question it answers:** can our walls crumble and whole buildings come
down, the way Red Faction's do?

**The answer:** yes, and cheaper for us than for Volition — but the visible half
needs a physics engine we do not have, and the valuable half does not.

---

## 1. What Red Faction actually does, in one paragraph

A building is a graph: a few thousand rigid subpieces joined by links that each
carry a cross-sectional area. Breaking something deletes edges; a background
pass re-runs connectivity from a set of anchor pieces at the base; anything no
longer reaching an anchor becomes loose. Strength is not baked — the file ships
joint *area* and piece *volume*, and a material table supplies strength and
density at load. Pieces the engine cannot afford are **deleted with a dust puff
played over them**. Full detail, with the shipped numbers, in the research note.

## 2. Why it is cheaper for us

Volition pre-fractured every building by hand in 3ds Max, and paid **83% of a
building's CPU memory** for the result. Two things make our position better:

- **Our geometry is generated, not authored.**
  [`StoreyGeometryEmitter`](../../src/game/render/scene/StoreyGeometryEmitter.cpp)
  builds the world out of `BoxEmitter` calls from tile data. A box is convex and
  axis-aligned, so cutting one is arithmetic — none of the hard parts of fracture
  (concave meshes, watertightness, UV continuity) apply. RFG pre-fractures
  *because* it cannot cut its authored meshes at runtime; that reason does not
  exist here.
- **One fracture pattern serves the whole game.** Cut a unit box once, offline,
  and instance the pattern per surface kind. RFG needed 460 unique chunk types
  for a city. Likewise the exposed interior — RFG needs an `_EDGE` material per
  fracture material because its wall interiors vary; a box wall has one interior,
  so one texture covers everything.

## 3. What the engine already has

| Red Faction | ours, today |
|---|---|
| subpiece | cell `(x, y, z)` in [`World`](../../src/game/world/World.hpp) |
| link | [`Edge`](../../src/game/world/Edge.hpp) horizontally, `Tile::hasFloor` vertically |
| link area | implicit — every face is the same size |
| anchor flag (`0x20`) | `z == 0` with floor. No flag needed |
| "edit the data, everything re-derives" | [`DestructionSystem`](../../src/game/rules/DestructionSystem.hpp) already states exactly this as its contract |
| rubble as cover | `stampWreck` — a killed vehicle's hull already becomes half cover |
| derived-cache precedent | `OcclusionGrid`, and CLAUDE.md's three rules for one |

**Not present:** any physics engine. `cromwell/collision/Shape.hpp` names Jolt in
a comment; nothing is integrated.

## 4. The stages, cheapest first

Rough sizes, not estimates.

### Stage 1 — material on tiles and edges
`Tile::artTag` is explicitly *"invisible to the simulation"*, so nothing today
records what a wall is made of. Add a material reference plus a data table of
strength and density. **~1–2 days.** Valuable standing alone: concrete survives
what wood does not, with no destruction work at all. Materials-as-data is
already the direction for the material system, so this is not speculative.

### Stage 2 — the support check, and whole buildings coming down
On every break, determine which cells still have a path down to the ground;
anything that does not, collapses. Bookkeeping over the grid we already have.
**~1 week**, most of it spent on doing it incrementally and spreading it across
frames rather than flood-filling per hit — Red Faction runs 400 objects a frame
with a 100–5000 ms gap between passes, and that shape is the right one.

This is the highest payoff per unit of effort in the whole plan: it is the
"blow out the ground floor and the building comes down" behaviour, with **zero
new art**.

Constraints: it is a derived cache, so CLAUDE.md's three rules apply, and it
must be invalidated at the boundary that owns the data (`World::at()` non-const,
as `OcclusionGrid` already is). It runs per frame, so it gets a profiler zone in
the same commit.

### Stage 3 — what a collapsing cell looks like
Cheapest honest version: the cell clears and stamps rubble that counts as half
cover, reusing the `stampWreck` idiom. **~2–3 days.** It pops rather than
crumbles.

**This is not a placeholder.** Red Faction deletes pieces it cannot afford and
covers the deletion with a dust puff — a permanent cheap path is something you
want regardless, for weak hardware and for distance from the camera. Treat it as
the bottom of a quality dial, per the standing preference for scalable settings
over adaptive tricks.

### Stage 4 — real chunks that break apart and tumble
Needs a rigid-body engine in first. **Jolt is its own project**, and this stage
cannot start until it lands. After that, cutting the boxes is comparatively
easy for the reasons in §2.

## 5. Decisions taken

- **Do not wait for Jolt.** Stages 1–3 do not need it, and — more to the point —
  the risky part of this feature is not the physics, it is the *gameplay*.
  Buildings coming down changes cover, line of sight, pathing and mission
  layout. Stages 1–3 answer "is a collapsing building good to play against" for
  a fraction of the cost. Doing physics first means building the expensive half
  against requirements not yet discovered.
- **Stage 3 is the low quality setting, not throwaway work.** See above.
- **Skip Red Faction's stress *load* accumulation, at least initially.** At ~3
  storeys, plain connectivity is most of the effect. Load accumulation buys "the
  wall sags before it goes", which needs a camera close enough to watch it.
  Revisit if the camera ever gets closer.

## 6. The ceiling on realism

Our world is a lattice, so pieces break along tile lines however finely we cut
them. Red Faction's fracture ignores any grid, and its shards follow window
frames, beams and panel lines because a human cut them; ours would be generic
and could only be biased per `artTag`. **Up close ours will read blocky.** At
this camera height that is very likely fine, and it is the reason stage 4 is
last rather than first.

## 7. Ideas from the research note worth taking whatever we build

Ranked, and several are independent of destruction entirely.

1. **Bake the measurement, look up the coefficient.** RFG ships joint area and
   piece volume and resolves strength and mass from a table at load, so one
   17 KB file re-tunes the whole game with no asset re-bake. Applies to any
   baked or derived data in cromwell.
2. **Amortised background solve with a fixed per-frame quota.** Bound the work,
   let the latency float.
3. **When you cannot afford an object, delete it and play the effect that makes
   deletion legible.** Generalises far past debris.
4. **Size each event by the quantity that event is about** — impact by energy,
   detachment by dimension, a slow topple by mass, a collapse by footprint area.
   One "importance" scalar for all of them is what makes destruction sound
   procedural.
5. **Two damage radii, one with impulse and one without.** The near field is
   thrown; the ring around it just lets go. One float, and most of why an RFG
   explosion reads as a demolition.
6. **A damage LOD** — one merged mesh per group until something in it breaks.
   Monotone, so no hysteresis. This is also the answer to the vertex-count
   problem stage 4 would otherwise hit.
7. **Clamp the speed used in debris damage**, and let the player push small light
   pieces aside without their own motion changing. The two ways a debris field
   ruins a game.
8. **Strength and damage-propagation are different material properties.** RFG's
   `Brittleness` decides how far a hit travels, not what a joint survives;
   conflating them is why materials feel samey.

## 8. Open questions

- What does a collapsing cell do to a unit standing in or under it? RFG has a
  complete tweak-table model of debris damage; we have none.
- Does a collapse invalidate a unit's already-issued move order, and how is that
  presented? Not considered at all yet.
- Multiplayer is out of scope here, but note that RFG replicates destruction as
  an ordered stream of numbered events and never replicates the physics — three
  shipped games in `study/` reach that same conclusion independently.
- Whether the support check should be its own grid or fold into an existing
  derived structure (`BlockedMass`, `RoomPartition`) was not investigated.
