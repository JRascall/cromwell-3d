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

`./tools/tidy.sh` runs clang-tidy over the tree (ships with VS2022, no compile
database needed). Currently clean; keep it that way. It does not substitute for
thinking about where a call sits — see the note below on what static analysis
cannot see.

## Profiling

**In-game, always available.** The dev panel (F1) has a **profiler** tab showing
per-zone CPU and GPU milliseconds, live.

**F9 starts and stops a capture.** It writes Chrome Trace JSON to
`builds/win/profiles/profile_NNN.json` — beside the executable, not the working
directory, and **numbered so a capture cannot overwrite the previous one**. Open
it at [ui.perfetto.dev](https://ui.perfetto.dev) for a zoomable flame graph;
CPU and GPU are separate lanes. The status line and the log give the exact path
and the frame count.

`xcom_profiler_tests` checks the trace is structurally loadable — the failure
that matters is a file Perfetto refuses to open, which is otherwise discovered
at the end of a profiling session.

GPU figures lag by a frame or two. That is inherent: a timestamp query issued
this frame is only readable once the device has caught up, and asking sooner
stalls the pipeline being measured.

### Give every new system a zone

**When you add anything that runs per frame, give it a profiler zone in the same
commit.** `CW_PROFILE_ZONE_N("name")` (cromwell/diag/Profile.hpp), plus a paired
`CW_GPU_ZONE("name")` (cromwell/gpu/GpuProfiler.hpp) if it issues GL work.

The reason is not tidiness. **An unzoned system does not show up as a zero row —
it shows up as nothing at all, and its time silently inflates whatever encloses
it.** The panel's `frame` row is the truth; everything under it is what has been
accounted for. The gap between the two is work nobody can see, and it grows
every time a system is added without a zone. Chasing a frame spike that lives in
that gap is exactly the debugging session the panel exists to prevent.

Name it after the system, at the altitude the existing rows sit at — `web`,
`steam`, `simulation`, `entity tick`, `effects`, `pointer pick`, `render`,
`shadow map`, `ssao`, `lit scene`, `recompute`, `reach`, `visibility`. If a new
row would nest under one of those, nest it; the panel indents by depth and a
capture keeps the tree.

### Granularity is earned by cost, not by code structure

**One zone per system is the default. Sub-zones are earned by being a large
share of the frame.**

The failure mode is mirroring the module tree in zone names — navigation gets a
zone for the representation, one for the planner, one for steering, one for the
flow field, and now there are twenty rows for something costing 0.3 ms. Zone
names are not a table of contents for the code. They answer "where did the frame
go", and a system that is not going anywhere needs exactly one row saying so.

The rule:

| Share of the frame | Granularity |
|---|---|
| under ~1% | one zone, or fold it into its parent |
| a few % | one zone, named for the system |
| **a big slice** | **split it — that is what sub-zones are for** |

`render` earns `shadow map` / `ssao` / `lit scene` because it is most of the
frame and the split says which pass. `steam` does not earn a breakdown, because
knowing which part of a 0.02 ms callback pump was slow changes nothing.

So: **add the system's zone when you add the system, and add sub-zones only
after a measurement points at it.** Granularity chases cost; it does not
anticipate it. If navigation ever becomes 8 ms, split it then — and the split
will be along whatever line the measurement actually revealed, which is rarely
the line the module structure would have suggested.

**Investigating is different.** Ten temporary zones inside a system to find a
spike is exactly right. Delete them afterwards and keep the one that explained
it.

Zones go on **passes and systems** — anything running once or a few dozen times
a frame. A zone costs ~40 ns, which is nothing around a render pass and ruinous
inside a ray step. For something running thousands of times you want a
benchmark, not a zone.

GPU zones **must not nest** — `GL_TIME_ELAPSED` allows one active query per
context. The render passes are siblings, so this costs nothing.

**Tracy**, for the deeper analysis the panel is not built for: configure with
`-DXC_TRACY=ON` and run the Tracy server to connect. The same zone macros feed
it, so no extra instrumentation. Off by default — the client is not free.

**Why not Visual Studio's profiler:** it profiles a whole process run with no
in-game trigger, and its GPU tool supports Direct3D and explicitly not OpenGL,
so it can say nothing about this renderer's GPU at all.

**For per-shader-line cost**, neither of these helps — a zone says the sky pass
took 3 ms, not which GLSL line did. That needs NVIDIA Nsight Graphics' Shader
Profiler (supports OpenGL, correlates to source lines and microcode) or AMD's
Radeon GPU Profiler. Reach for those once a zone has told you which pass.

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
- **When ranking candidates, sort before the expensive test — then run it from
  the best down and stop at the first pass.** Cheap filters, then score, then the
  raycast. Testing every survivor is the mistake: you only need the *winner* to
  pass, so the common case costs one expensive test rather than one per
  candidate. Applies to target selection, cover scoring, ability placement —
  anything that picks a best cell out of many. (`study/spatial_queries.md` §3.6,
  where this is CryEngine's stated rule and raycasts are "the dominant cost of
  the whole AI system".)
- **Enumerate candidates in the shape of the question, not the shape of the
  grid.** A radius scored over a square lattice puts its best cells on the
  cardinal and diagonal axes, so units converge on eight headings and approach at
  a visible slant — real shipped bug, not a theoretical one. Walk a ring for a
  radial query. Cheaper *and* unbiased. (`study/spatial_queries.md` §5.2.)
- **When a choice is re-made repeatedly, give the incumbent a small discount.**
  Scores are noisy and cost functions have plateaus, so two candidates at 0.71
  and 0.70 will swap on rounding and the unit oscillates — re-picking cover
  every turn, or re-targeting every frame. A bias of a percent or so on "what I
  chose last time" is enough to break ties and invisible when it is not needed.
  Costs one term; the bug it prevents reads as "the AI is indecisive" and gets
  misdiagnosed as pathfinding or scoring. **Incumbency is information, and a
  scorer that ignores it thrashes.** Applies wherever the previous rule does.
  (Unreal's motion matching does exactly this with a `-0.01` continuing-pose
  bias — `study/motion_matching.md` §3.2, §8.2.)

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
