# Map scale — Eugen Systems, and the problem that is extent rather than count

Deep dive on **R.U.S.E.**, **Wargame** and their successors, running on Eugen's
**IRISZOOM** engine. These are the odd ones out in this directory: their hard
problem is not how many things exist, but how far apart they are and how much of
the world is visible at once.

> **A warning about sources, up front.** Eugen have published **no technical
> talks, papers or engine documentation**. There is no GDC session, no GPUOpen
> analysis, no equivalent of Booth's L4D paper. Everything factual below is a
> *published specification* — map sizes, ranges, unit counts, documented
> gameplay mechanics — and everything architectural is **[inferred]**: reasoning
> from what the game demonstrably does to what must be true underneath.
>
> That is weaker than the other notes in this directory and is marked as such
> throughout. It is still worth writing down, because the *problem shape* is
> real and well-specified even where the solution is not.
>
> **Superseded for R.U.S.E. — see [`ruse.md`](ruse.md).** The warning above was
> right about Eugen's *publications* and wrong about the *evidence*: R.U.S.E.
> ships its whole content pipeline as readable data, and `ruse.md` is a
> first-party read of the retail build. It corrects this note in two places
> — §1.1's numbers are Wargame's and do not describe R.U.S.E. (largest shipped
> map **10.3 km²**, unit cap **200**), and §2's inferred streaming and LOD
> architecture is now measured rather than reasoned. Where the two disagree,
> `ruse.md` wins. What survives here is the *Wargame* material and §3–4, whose
> conclusions `ruse.md` confirms.

Tags: **[SPEC]** published specification or documented mechanic.
**[COMMUNITY]** player-derived, wikis and guides. **[inferred]** our reading.

---

## 1. The problem shape, and why it is different

### 1.1 The numbers

**[SPEC]**

| | |
|---|---|
| Map size | up to **150 km²** (Wargame: AirLand Battle, Red Dragon) |
| Objects | *"millions"* displayed simultaneously |
| Unit types | 1,200+ (Red Dragon) |
| Players | up to **20** simultaneously |
| Camera | seamless from whole-map strategic down to ~20 m above ground, **no loading, no transition** |
| Engine claim | maps *"a hundred times larger than in traditional RTS games"* |

**[COMMUNITY]** Weapon ranges are deliberately **compressed below reality**: tank
guns capped around **2,275 m**, most ATGMs **2,800 m**, with air defence and
artillery also shortened *because of map size*.

**[inferred] That last line is worth pausing on, because it inverts the usual
expectation.** The maps are enormous, and the ranges still had to be cut — a real
Cold War tank gun outranges 2,275 m comfortably. So even 150 km² is *small*
relative to the engagements being modelled. Eugen are not fighting to fill the
map; they are fighting to make a map big enough that realistic ranges do not
trivialise it, and losing.

### 1.2 Density is the whole difference

Set against the other notes here:

| | Extent | Combatants | Density |
|---|---|---|---|
| **Total War** | ~1 km² battlefield | ~6,400 | very high |
| **AC Unity** | a city district | 10,000 | high |
| **Wargame** | **150 km²** | ~1,000 | **~1000x lower** |
| **R.U.S.E.** ([`ruse.md`](ruse.md) §1.1) | **1.7–10.3 km²** | **≤200** | low, but nothing like Wargame's |

**[inferred] This is the key structural fact and it changes which data structures
are correct.** Total War's problem is *contention* — thousands of things in one
place, all interacting. Eugen's problem is *extent* — a thousand things scattered
across an area a thousand times larger, most of it empty, with sight lines
kilometres long.

Those want opposite structures. Contention wants a **dense flat grid**: every
cell occupied, arithmetic addressing, no wasted memory. Extent wants something
**sparse and unbounded**: a hash grid, or a hierarchy, because a dense grid over
150 km² at any useful resolution is mostly empty cells you paid for.

**[inferred] Which retroactively justifies the choice in `navigation.md` §3.** The
spatial *hash* — sparse-friendly, unbounded, memory only where things are — is
precisely the structure an extent-dominated world wants, and precisely the wrong
one for a dense tile battlefield (where this project's lattice is correct
instead). Eugen's games are the archetype for the case the engine-side structure
was built for.

---

## 2. What IRISZOOM must be doing

**[SPEC]** The engine *"streams all the data"*, which is what makes the seamless
zoom possible. Beyond that sentence, nothing is published.

### 2.1 The zoom is the defining constraint

**[inferred]** A conventional RTS has one camera distance and can tune a single
LOD set around it. A camera that travels continuously from 20 m to whole-map
altitude has no such luxury: **every asset needs a representation at every scale,
and transitions must be invisible in motion.**

That forces, at minimum:

- **Continuous or heavily-tiered LOD** with cross-fading, not popping — the zoom
  is a *player-driven, continuous* camera motion, so any pop is directly
  observable and repeatable.
- **Imposters or billboards at strategic altitude.** At whole-map zoom a vehicle
  is a few pixels; drawing geometry for it is waste. Per `navigation.md` §11.4,
  that is exactly the imposter case.
- **Terrain LOD with no cracks** across an enormous heightfield — clipmaps or
  chunked LOD with skirts, streamed by distance.
- **Aggressive draw-call batching**, because at strategic zoom nearly every unit
  on a 150 km² map is potentially in frustum simultaneously. This is the moment
  the engine cannot cull by distance, and it is the worst case that sets the
  budget.

**[inferred] The strategic view is the performance worst case, not the tactical
one** — which is the reverse of most games, where the close-up view is the
expensive one. That single inversion probably explains most of IRISZOOM's design.

### 2.2 What the "millions of objects" claim likely means

**[inferred]** Not millions of simulated entities — a thousand-ish units are
simulated. The figure almost certainly counts *scenery*: trees, buildings, rocks,
props over 150 km². Those are static, instanced, and never think.

Worth separating explicitly, because it is the same distinction as Men of War's
**10–15k entities versus 500–800 units** in [`battle_scale.md`](battle_scale.md)
§2.4. Marketing counts objects; the simulation counts agents; they differ by
orders of magnitude in both games.

---

## 3. Spotting — the interesting spatial query

This is the best-documented part, because players reverse-engineered it, and it
is the most transferable.

### 3.1 The mechanic

**[COMMUNITY]** Detection is a contest between two per-unit stats:

| | |
|---|---|
| **Optics** | how far and how reliably a unit can spot and identify others |
| **Stealth** | a *"stealth field"* around a unit — the better the stealth, the larger the field, and the closer an observer must come to see through it |

Modified by terrain: cover grants a **stealth bonus** (infantry additionally take
70% less damage in towns, 40% in forests). And critically —
**[COMMUNITY]** *vision cannot be granted beyond obstructions*: hills and
mountains genuinely block sight.

**[inferred]** So detection is roughly: for each (observer, target) pair, compare
optics against stealth at that range, then confirm an unobstructed line over the
terrain. That is a **visibility query with a range test and a terrain LOS test**,
and it is the thing Eugen's whole game design rests on — recon is the central
skill in Wargame.

### 3.2 Why it cannot be done naively

**[inferred]** With ~1,000 units and up to 20 players, the pair count is order
**10⁶**. Doing a terrain ray-march per pair per frame over kilometres is plainly
impossible. So three things are near-certain:

1. **Range culling first, via a spatial index.** Optics tops out around 2–3 km on
   a ~12×12 km map, so each observer's candidate set is a tiny fraction of the
   world. This is exactly a `queryRadius` and it must come before any LOS work.
2. **Spotting runs on a slow tick, not per frame.** Recon in Wargame resolves over
   seconds, not milliseconds, and detection visibly lags. That is the *think*
   pattern — see `cromwell/entities/Component.hpp`, which already has this split
   and staggers phases so a cohort does not think in lockstep.
3. **Terrain LOS is a 2D heightfield march, not a 3D DDA.** Occlusion comes from
   terrain (and cover bonuses), not from arbitrary geometry, so the test is
   "does the height profile between A and B ever rise above the sight line" —
   a walk along a line over a heightmap, with early-out. Vastly cheaper than this
   project's `RayCaster`, and only possible because the world is a heightfield
   rather than a voxel lattice.

**[inferred] Point 3 is the one worth internalising.** Eugen can afford
kilometre-long sight tests because their world representation makes LOS a 1D
problem along a 2D line. The cost of long-range visibility is set by the *world
representation*, not by the range.

### 3.3 R.U.S.E.'s twist: the visibility system lies

**[SPEC]** R.U.S.E.'s central mechanic is deception — the "ruses" of the title
let a player feed the opponent false information: fake units, disguised
divisions, hidden movements.

**[inferred]** Which means the game does not have *a* visibility state; it has a
**per-player believed state** that can deliberately diverge from truth. That is a
real architectural demand and not a cosmetic one: visibility results must be
per-observer, storable, and corruptible by design — you cannot bolt lying onto a
system that computes one global "who can see what".

Worth noting because a fog-of-war system built the simple way (one shared
visibility field) forecloses this entire genre of mechanic, and the cost of
keeping visibility per-observer is paid at the very beginning or not at all.

---

## 4. Read against the rest of this directory

### 4.1 Three different problems, three different answers

| | The hard part | The answer |
|---|---|---|
| **Crowds** (`crowd_scale.md`) | agent *count* | tier the agents; most are not real |
| **Battles** (`battle_scale.md`) | simulation *depth* × count | decouple sim from display; queue expensive work |
| **Map scale** (here) | *extent* and view distance | stream everything; sparse spatial index; range-cull before any expensive test |

**[inferred]** None of these three is a harder version of another. They are
different axes, and a technique from one is frequently useless in another — LOD
tiers do nothing for a 150 km² map with a thousand units, and streaming does
nothing for ten thousand pedestrians in one square.

### 4.2 What actually transfers from Eugen

**[inferred]**

1. **Range-cull before you test.** The expensive test (LOS, penetration,
   pathfinding) should never run on a pair the spatial index could have rejected.
   Obvious, and still the most commonly skipped step.
2. **Match the spatial structure to density, not to taste.** Sparse-and-vast
   wants a hash or a hierarchy; dense-and-small wants a flat grid. This project
   has both cases live — the lattice for the tile map, `SpatialHash` for the
   engine.
3. **Let the world representation set the cost of a query.** Terrain-only
   occlusion over a heightfield makes 3 km sight lines affordable. A voxel
   lattice makes them expensive. Choose the representation with the queries in
   mind, because it is not changeable later.
4. **Decide early whether visibility is per-observer.** R.U.S.E.'s whole design
   depends on it and it cannot be retrofitted.
5. **Slow ticks for perception.** Detection at 2 Hz is indistinguishable from
   detection at 60 Hz and costs a thirtieth as much.

### 4.3 The honest gap

**[inferred]** Everything in §2 is reasoning from the outside. If Eugen ever
publish, this section should be rewritten rather than patched — the streaming and
LOD architecture of a seamless-zoom engine is the part worth actually knowing,
and it is precisely the part that is missing.

**Update.** It got rewritten rather than patched, and not because Eugen
published — because the build turned out to be readable. See
[`ruse.md`](ruse.md) §3 and §4. The scorecard for §2's guesses, since it is worth
knowing which kinds of inference held:

| §2 said | The build says |
|---|---|
| continuous or heavily-tiered LOD, no popping | **two separately baked terrain meshes**, not a LOD chain — and `TransitionProportion 0.5` chains under each |
| imposters at strategic altitude | correct, and more so: a whole impostor system with packed atlases, its own sun-view matrix and its own depth shadow |
| terrain LOD with no cracks, streamed by distance | correct on streaming, wrong on *by distance* — priorities are in-view / nearly-in-view / near and they **cross over between levels** |
| aggressive draw-call batching | correct — offline texture grouping per scenery set, with the LOD meshes atlased separately |
| the strategic view is the performance worst case | **the strongest guess, and it holds**: it is why the coarse mesh is a separate resident asset and why coarse-but-near outranks fine-but-visible |

The pattern is that reasoning from the constraint got the *shape* right almost
every time and the *mechanism* wrong about half the time — which is roughly the
value an inferred note should be assumed to have.

---

## Sources

**Engine and specifications**
- [IRISZOOM (Wargame Wiki)](https://wargame.fandom.com/wiki/IRISZOOM) and [IRISZOOM (R.U.S.E. Wiki)](https://ruse.fandom.com/wiki/IRISZOOM)
- [IRISZOOM engine — ModDB](https://www.moddb.com/engines/iriszoom)
- [Wargame: Red Dragon](https://en.wikipedia.org/wiki/Wargame:_Red_Dragon) and [Wargame: AirLand Battle](https://en.wikipedia.org/wiki/Wargame:_AirLand_Battle) — map sizes, player counts, engine versions
- [R.U.S.E.](https://en.wikipedia.org/wiki/R.U.S.E.) — the deception mechanic
- [Eugen Systems](https://eugensystems.com/)

**Gameplay mechanics (player-derived)**
- [Razzmann's Extensive Wargame: Red Dragon Tutorial](https://steamcommunity.com/sharedfiles/filedetails/?id=758149660) — optics, stealth fields, cover bonuses
- [A Guide to Recon and Counter-Recon](https://steamcommunity.com/sharedfiles/filedetails/?id=244638431)
- [Recon and Counter-Recon — Wargame Red Dragon Strategies](https://troublmaker.wordpress.com/2014/04/01/recon-and-counter-recon-wargame-red-dragon-strategies-and-tactics/)
- [Sight Range and Cover — Eugen Systems Forums](https://forums.eugensystems.com/viewtopic.php?t=48795)

**Related notes**
- [`ruse.md`](ruse.md) — the first-party read of R.U.S.E.'s shipped build; supersedes §1.1 and §2 here
- [`crowd_scale.md`](crowd_scale.md) — when the problem is agent count
- [`battle_scale.md`](battle_scale.md) — when the problem is simulation depth
- [`navigation.md`](navigation.md) — the spatial index and navigation layers, and why the hash suits extent-dominated worlds
