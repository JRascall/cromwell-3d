# CLAUDE.md

## Build and test

cmake ships with VS2022, not on PATH:

```
CM="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CM" --build builds/_cmake-win --target <target> --config Release
```

Build tree is `builds/_cmake-win`; the runnable product stages to `builds/win`.
`ctest -C Release` from the build tree runs all suites. `game_core` and the test
binaries build without raylib — prefer them for a fast loop.

| | |
|---|---|
| `xcom_perf` | simulation benchmark: occupancy, pathfinding, visibility at 4/16/64/256 bodies, plus crowd neighbour queries and a cell-size sweep |
| `xcom_perf --bake` | also runs the sun bake benchmark |
| `xcom_tests` | tile core, including derived-cache equivalence checks |

## The one architectural rule

`cromwell` (engine) may not include, link against, or name anything under
`game/`. The arrow never reverses — that is what makes the engine liftable into
the next project, and `cmake --build . --target cromwell` checks it rather than
trusting it. See the header comment in CMakeLists.txt.

cromwell is intended for future RTS, FPS and third-person projects, so judge
engine-side features against three genres, not just this tile game.

## Performance discipline for new code

Consider this on every non-trivial addition. Not as a final polish pass — the
layout and the access pattern are the parts that are expensive to change later.

### Order of attack, and it is not negotiable

1. **Do less work** — algorithmic. Measured in this codebase: **146x** (replacing
   a per-cell roll call over every unit with one array read).
2. **Do it closer** — cache and layout. Measured here: **~2x** (68-byte `Tile`
   fetches replaced by a 2-byte-per-cell summary).

Algorithmic wins are one to two orders of magnitude larger than cache wins and
they compose. A spiral search touching 25 cells beats a perfectly cache-friendly
scan of 5,000, *even though its access pattern is worse*. Fix what you are doing
before fixing how it is laid out.

### Measure before you commit to an optimisation

Concrete example, kept as a warning: the full-map scratch wipe in `ReachField`
looked like an obvious win. Measured, it is **0.7 µs against a 15.8 µs search —
4%**, and generation stamps would have added a load to every `cost()` call in the
search's hot loop. It was **not implemented**, deliberately. `xcom_perf` still
prints the split so the decision can be revisited when maps get bigger.

Similarly `Footprint::cellsAt` allocates a `std::vector` per call and is used per
unit in `OccupancyMaskBuilder`. Mask build measures 0.005 ms at 256 bodies.
Left alone on purpose.

### Structural, not micro

This is about **shape**, not constant factors. Do not micro-optimise.

| Worth doing up front | Not wanted |
|---|---|
| Data layout and access patterns | instruction counting |
| Complexity class — what work happens at all | manual inlining, loop unrolling, branch hints |
| Where a call sits relative to a loop | rewriting readable code to shave 2% |
| Choosing the right structure for the data | anything in cold code, ever |

The test: **does it change what the code is doing, or only how fast it does the
same thing?** The first is worth getting right when the code is written, because
it is expensive to change later. The second is almost never worth it, and when it
is, a measurement will say so first.

**Cold code gets none of this.** Most of the codebase runs once, or on a click,
and its performance is irrelevant — write it for clarity and comment it properly.
The question "how many times does this actually run?" is usually answered in two
seconds with "not many", and that is the end of it. Only the small fraction that
sits inside per-cell, per-ray, per-agent or per-frame nesting deserves the rules
above.

### Where DOD belongs, and where it does not

**Data-oriented in the spatial query layer.** Flat arrays, one to four bytes per
cell, indexed by arithmetic: `OccupancyGrid`, `OcclusionGrid`, `ReachField`,
`BlockedMask`, `SpatialHash`. This is where layout pays.

**Ordinary OOP in the entity layer.** `Unit`, components, virtuals, `unique_ptr`.
There will never be enough units for their layout to matter, and the readability
is worth more. **Do not turn the entity/component system into an archetype ECS** —
the cost is in the queries each entity makes, not in iterating them.

### Hot-loop rules

Inside anything running per cell, per ray step, per agent or per frame:

- **No hash or map lookups.** `Unit` caches component pointers precisely because
  `findComponent<T>()` was landing inside the ray caster's per-step loop. Cache
  the pointer at bind time; `Entity::onComponentsChanged` exists to make that
  safe.
- **No linear scans** over a collection a grid could index.
- **No allocation.** Queries fill a caller-supplied vector — see
  `SpatialHash::queryRadius`.
- **Keep a running index** rather than recomputing `(z * height + y) * width + x`
  per access when stepping one cell at a time.
- **Cull cheaply before testing expensively.** Range-reject a pair before running
  LOS, penetration or pathfinding on it.

### Derived caches need the escape-hatch pattern

A summary of authoritative data (`OcclusionGrid` over `Tile`) is a maintenance
liability. Three rules keep it honest:

1. **The fast path may only skip work that provably does nothing.** It never
   decides anything the slow path would have decided differently.
2. **The slow path is the original code, unmodified.** Complicated cells set a
   `kNeedsTile`-style bit and fall through to it. This keeps it an optimisation
   rather than a second implementation to keep in step.
3. **Test derived data against its source, not against a second implementation.**
   Comparing two implementations only proves they agree. `testOcclusionGrid`
   checks every bit against the tiles; `testOccupancyIndex` compares a bound
   roster against the original scan and re-checks after moves, deaths and
   demolition.

Invalidate at the boundary that owns the data — `World::at()` non-const dirties
the occlusion grid, so there is no way to mutate geometry and forget.

### GPU

The engine targets GL 4.3 and has compute (`cromwell/gpu/compute/`). Before
moving work there: the readback is usually the problem, not the dispatch. Keep
state resident and avoid round-trips. Assume rendering becomes the limit before
simulation does — see `study/navigation.md` §10–11.

## Research notes

`study/` holds sourced deep dives, with explicit source tags (`[VALVE]`,
`[EPIC]`, `[PAPER]`, `[COMMUNITY]`, `[inferred]`). Look there before
re-researching. `navigation.md` covers spatial indexing and navigation;
`crowd_scale.md`, `battle_scale.md` and `map_scale.md` cover how shipped games
handle agent count, simulation depth and map extent respectively.

Preserve the commenting style in this codebase. Headers explain **why** a thing
exists and what was rejected, at length. That is the project's main asset — match
it rather than trimming it.
