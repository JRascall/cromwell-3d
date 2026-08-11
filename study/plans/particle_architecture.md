# Particle system — requirements

**A design note, not research.** The third in this directory alongside
[`nav_architecture.md`](nav_architecture.md) and
[`console_porting.md`](console_porting.md). Everything factual about Source 2 is
in [`source2_particles.md`](../games/valve/source2_particles.md); this note decides *what
`cromwell` should build*, and is written to be implementable later by someone who
has not read the research.

Requirements are numbered `R1…` so they can be argued with individually. Each is
**MUST**, **SHOULD** or **WON'T**, in the RFC sense. Where a requirement exists
because Valve solved something a particular way, the Source function is named so
the reference can be found; where it deliberately departs, it says so.

---

## 1. The rule that generates the rest

> **Build the machine, not the catalogue.**

Source 2 ships ~166 named functions and it took fifteen years of artist requests
to get there ([`source2_particles.md`](../games/valve/source2_particles.md) A.7). Almost none of
that is architecture. The architecture is four ideas — declared columns,
parameter providers, control points, a fixed execution order — and **a system
with those four and fifteen functions is a good particle system, while a system
with a hundred functions and none of them is a mess that cannot be extended.**

Two corollaries that decide arguments later:

- **Every function we do not write is free, and every one we write is forever.**
  Add functions one at a time, each because a specific effect needed it. Never
  because the list looked incomplete.
- **The parameter provider system (§4) removes the *need* for whole families of
  functions.** Build it before any operator, or dozens of operators will be
  written that a provider and a remap curve would have covered.

## 2. Scope

**In scope.** A CPU-simulated, data-driven particle system in `cromwell`,
authored in-game, driven by gameplay through control points, able to collide with
the world and to follow a skeleton.

**Out of scope, permanently** — see §13 for the full list with reasons: GPU
compute simulation, a node-graph material editor, a standalone tool, and Source's
operator catalogue.

**Out of scope, for now.** Anything not needed by an effect that exists. This is
a specification of the machine and the first functions, not of the content.

## 3. Data model

**R1 (MUST) — particles are columns, not structs.** Attributes are separate
arrays indexed by particle, never an array of a `Particle` struct.
*Why:* this is the one decision that is free now and effectively impossible
later; every operator written against a struct has to be rewritten. It is also
the only DOD claim in this note that is structural rather than a constant factor
(CLAUDE.md, "structural, not micro").

**R2 (MUST) — a fixed attribute enum with a bitmask.** One `uint32` mask,
therefore at most 32 attributes. Valve's cap of 32 is not a coincidence; it is
the mask width, and the mask is worth more than the 33rd attribute.
Initial set, trimmed from Source's 22:

| | |
|---|---|
| `Position`, `PrevPosition` | Verlet pair — **there is no velocity attribute** (R3) |
| `Radius`, `Rotation`, `RotationSpeed` | |
| `TintRgb`, `Alpha` | |
| `CreationTime`, `LifeDuration` | |
| `SequenceNumber` | flipbook frame |
| `ParticleId` | stable per-particle identity, for `PerParticleCount` providers |
| `TrailLength` | reserve; needed by the trail renderer |
| *reserved:* `TraceP0`, `TraceP1`, `TraceHitT`, `TraceHitNormal` | R24 |
| *reserved:* `BoneIndex`, `BoneRelativePosition` | R29 |

**R3 (MUST) — motion is Verlet.** Velocity is `Position − PrevPosition`.
*Why:* it makes every position constraint (R25–R27) correct for free — move the
particle and the velocity correction follows — which is the entire reason
Source's collision is a *constraint* rather than a special case. The swap at the
end of a step is a pointer swap of two column bases, not a copy.

**R4 (MUST) — every function declares the attributes it reads and writes**, as
two pure-virtual mask accessors. Pure virtual, not defaulted: a function must not
be able to decline to declare itself.

**R5 (MUST) — allocate columns from the union of the declared masks.** A system
whose particles never rotate allocates no rotation array.

**R6 (MUST) — an attribute nobody initialises collapses to a constant** on the
definition, and reads resolve to that scalar.
*Why:* R5 and R6 together are the payoff for R4 and are worth more than the
layout. They remove memory streams rather than making them faster — CLAUDE.md's
"do less work" ahead of "do it closer".

**R7 (MUST) — assert the declaration at the point of use, in debug builds.** A
write to an undeclared column trips an assert on the first write.
*Why:* the difference between a declared dataflow and a documented one.

**R8 (SHOULD) — a separate initial-value buffer**, allocated only for columns
some function declared it would read initial values of.
*Why:* "fade to 30% of spawn radius" needs the spawn radius at t=3s. Without
this, every function that wants it keeps a private copy.

**R9 (WON'T, for now) — hand-written SIMD.** Write the columns SoA and plain
scalar loops over them, and let the compiler vectorise.
*Why:* this is a deliberate departure from Source and it follows the repo's own
rule. Valve needed explicit `fltx4` at 5,000 particles per system on 2007
hardware. SoA layout is *structural* and required by R1; four-wide intrinsics are
a *constant factor*, which CLAUDE.md says not to spend up front and to justify
with a measurement. R1 is what makes adding SIMD later cheap — the layout is
already right — so this defers the cost without paying the retrofit.
**Revisit when `xcom_perf` says simulation is a measurable share of the frame.**

## 4. Parameter providers — build this first

**The highest-value feature in the design, and it is not a function.** In Source 2
a numeric parameter is not a float; it is a source plus a remap.

**R10 (MUST) — a numeric parameter is an `INumberProvider`, not a float**, with
vector and transform equivalents.
Initial provider set:

| Provider | Returns |
|---|---|
| `Literal` | a constant |
| `Random` | range, with mode and optional bias |
| `ParticleAge` / `ParticleAgeNormalized` | seconds / 0–1 over lifetime |
| `ParticleField` | any scalar attribute of this particle |
| `ParticleSpeed` | velocity magnitude |
| `ParticleId` / `ParticleIdNormalized` | identity, and 0–1 across the emission |
| `CollectionAge` | system age |
| `ControlPointComponent` | one component of a control point |
| `ControlPointSpeed` | a control point's velocity magnitude |
| `DetailLevel` | the current quality tier — see R47 |

**R11 (MUST) — every provider's output passes through a remap stage** with at
least: `Direct`, `Mult`, `Remap` (input range → output range), `Curve`
(piecewise), `Clamp`. `RemapBiased`, `Notched` and `Round` are SHOULD.
Input handling: clamped or looped.

**R12 (MUST) — providers are resolved without allocation and without virtual
dispatch per particle.** A provider is evaluated inside the per-particle loop; it
must not be a `std::function`, must not allocate, and must not do a map lookup
(CLAUDE.md hot-loop rules). A tagged union or a small resolved struct, decided at
load time.

> **Why R10–R12 come before any operator.** "Fade out over life", "grow with
> speed", "tint by distance from a control point", "vary by particle index" are
> then *not operators*. They are a parameter's source and a curve. Skipping this
> means writing each of them as an operator, then writing them again for the next
> attribute, which is exactly how a catalogue reaches 166 entries.

## 5. Functions

**R13 (MUST) — one base type, distinguished by which entry point it overrides.**
Emitters, initializers, operators, forces, constraints, renderers and
pre-emission operators are all the same class with different virtuals — as Source
does, confirmed twice ([`source2_particles.md`](../games/valve/source2_particles.md) A.7:
emitters and renderers are registered as `C_OP_*`).

**R14 (MUST) — the execution order is fixed and is this:**

```
pre-emission operators -> emitters -> initializers -> operators
                       -> forces -> integrate -> constraints -> bounds -> children
```

then, at render time: visibility → sort → renderers. Confirmed from Valve's own
documentation, not inferred.

**R15 (MUST) — forces accumulate into one shared array; a single integration step
consumes it.** A force never writes position.
*Why:* forces compose for free and adding a fourth costs one pass, not one
integrator.

**R16 (MUST) — constraints run after integration and may iterate.** They may be
run over sub-ranges, and one may declare itself final (runs last, once).

**R17 (SHOULD) — emitters return a mask of attributes they already initialised**,
so initializers skip rather than overwrite.

**R18 (MUST) — per-function state lives in a context block the framework
allocates**, not in members. Function instances are shared and effectively
immutable once loaded.
*Why:* the same conclusion Unreal reached for behaviour-tree nodes
([`ai_state_machines.md`](../topics/agents/ai_state_machines.md) §5.6). It is also what makes R21
(determinism) achievable.

**R19 (SHOULD) — every function has a framework-level fade envelope**
(start/end fade in, start/end fade out) producing a strength value passed to it.
*Why:* one uniform implementation and one uniform editor control, instead of
every operator growing its own "when am I active" parameters.

**R20 (MUST) — the initial function set is small.** Phase 0 in §12. Adding a
function requires an effect that needs it.

**R21 (MUST) — simulation is deterministic given a seed.** Random numbers come
from a per-collection generator, never from a global.
*Why:* reproducible bugs, and it keeps the door open for replays and for
networked effects. Source enforces this by **shadowing the global RNG with a
member that asserts** — cheap, and worth copying exactly: it converts a code
review rule into a compile-time trap.

## 6. Control points

**R22 (MUST) — control points are the only coupling between gameplay and
effects.** Game code never touches particles directly.

**R23 (MUST) — a control point carries more than a position**: position,
orientation, velocity, radius, and a parent index. Duration and density are
SHOULD.
*Why:* Valve's set, arrived at over two engines. Velocity in particular is what
lets an effect inherit motion without the game computing it per particle.

**R24 (MUST) — 16 slots initially**, capped by a `uint32` read-mask (so ≤32).
The count is a `constexpr`. Valve use 64 with a `uint64` mask; 16 is enough for
years and the mask width is the real design decision.

**R25 (MUST) — control points are writable by the system itself**, via
pre-emission operators, and are the system's intermediate value bus — not only
its input.
*Why:* the finding that most reframed the design
([`source2_particles.md`](../games/valve/source2_particles.md) A.4). Every pre-emission operator
in Source writes a control point. This is what lets effects compute
"distance between these two things" once, rather than per particle.

**R26 (MUST) — a `SetControlPointToImpactPoint` equivalent exists in Phase 1.**
*Why:* it is the cheap answer to a whole class of "the effect must know where the
shot landed" requirements, it needs no particle-side collision at all, and the
gameplay layer has already done the trace. Valve shipped exactly this operator.

## 7. Collision

**R27 (MUST) — collision is tiered, and the tier is an authoring choice.** Four
levels, cheapest first, matching Source's:

| Tier | Function | Needs |
|---|---|---|
| 0 | `PlaneCull` — kill on crossing a plane | nothing |
| 1 | `PlanarConstraint`, `BoxConstraint` — analytic shapes | nothing |
| 2 | `WorldCollideConstraint` — resolve against world geometry | the callback (R28) |
| 3 | `WorldTraceConstraint` — a real trace, results left in the `Trace*` columns for later functions to read | the callback (R28) |

*Why tiered:* this is CLAUDE.md's *cull cheaply before testing expensively* moved
into the content layer — the artist picks the cost per effect rather than the
engine picking one fidelity for everything. **Tiers 0 and 1 depend on nothing and
belong in Phase 1.**

**R28 (MUST) — the world is reached through a callback interface the game
implements.** `cromwell` may not include anything under `game/` (CLAUDE.md's one
architectural rule), and the existing caster lives in
[`game/los/RayCaster.hpp`](../../src/game/los/RayCaster.hpp).
*Note:* that caster is a 2.5D line-of-sight query answering *"can A see B"*, with
a bound `UnitRoster` and a rules enum. It is **not** the query this needs. A
particle needs *"where does a segment first hit solid geometry, and what is the
normal"*. Tier 2/3 requires that query to be written; tiers 0–1 do not.

**R29 (SHOULD) — a plane-cache escape hatch.** Where an effect collides against a
small fixed set of surfaces, cache the planes at spawn and use tier 1 against
them rather than tracing per particle. This is the derived-cache pattern the
codebase already uses for `OcclusionGrid`, and the same three rules apply: the
fast path may only skip work that provably does nothing, the slow path is
unmodified, and it is tested against its source rather than against a second
implementation.

**R30 (MUST) — collision resolves as a constraint** (R16), not as an ad-hoc
position edit inside an operator.

## 8. Skeleton binding

**R31 (BLOCKED, MUST eventually) — particles can be bound to a bone.** `Skeleton`,
`Bone` and `ozz` currently return zero hits across `src/`; there is no animation
system, so there is nothing to bind to. **This cannot be built first and must not
shape the design beyond reserving R2's two attribute slots.**

**R32 (MUST, when unblocked) — treat it as three separable features**, as Source
does, because effects need different subsets:

| Feature | Source equivalent | Used by |
|---|---|---|
| spawn distributed over a model's surface | `C_INIT_CreateOnModel` | blood, impacts |
| assign each particle a bone/hitbox | `C_INIT_SetHitboxToModel` / `ToClosest` | anything persistent |
| follow that bone per frame | `C_OP_LockToBone` | burning, frost, status effects |

**R33 (MUST, when unblocked) — position is stored bone-relative** and transformed
by the bone matrix each frame, not re-projected from world space.

## 9. Rendering

**R34 (MUST) — one sprite renderer first**, instanced quads, one draw call per
batch.

**R35 (MUST) — a compact sort record, sorted as columns.** Sort key, index,
effective radius, effective alpha. Position is *not* in the common record.

**R36 (SHOULD) — cull during sort-list generation**, not as a separate pass.

**R37 (MUST) — flipbook support** via the `SequenceNumber` attribute and sheet
metadata (rows/columns), with blending between frames as a SHOULD.
*Why:* [`helldivers2_vfx.md`](../games/shooters/helldivers2/helldivers2_vfx.md) shows this is universal, not a
Valve quirk.

**R38 (SHOULD) — soft particles**, i.e. depth-aware fade against the scene depth
buffer. Cheap, and its absence is the single most obvious tell of a naive
particle renderer.

**R39 (WON'T, Phase 0–2) — trails, ropes, model particles, light particles,
sound particles.** All exist in Source; none is needed to ship a first effect.
Trails are the most likely first addition.

**R40 (MUST) — fill rate is the budgeted resource, not particle count.**
Retirement and LOD decisions are made on estimated screen area, following
Source's `m_flCullFillCost`.
*Why:* overdraw is what actually costs; a count budget measures the wrong thing
([`source2_particles.md`](../games/valve/source2_particles.md) §13.1).

## 10. Lifecycle

**R41 (SHOULD) — "end cap" is a first-class second lifetime phase**, not a flag.
When a system is told to stop, particles enter a distinct mode with its own
decay/freeze/lerp behaviour.
*Why:* every hand-rolled particle system bolts this on badly two years in, and
Source has six operators plus an enum plus an age provider devoted to it. Getting
the *phase* into the model early is cheap; retrofitting it is not. The operators
themselves can wait.

**R42 (MUST) — a system knows when it is finished** — no particles, and no
function intends to create more — and is reclaimed.

**R43 (SHOULD) — children**, with a spawn delay, so an effect is composable from
sub-effects.

## 11. Authoring

**This section is the reason the system is worth building rather than buying.
If it is skipped, buy Effekseer instead** — the whole build-versus-buy argument
in [`source2_particles.md`](../games/valve/source2_particles.md) §12A turns on the iteration
loop being fast.

**R44 (MUST) — effects are data, in a text format, not code.** JSON via the
already-chosen `nlohmann/json`.

**R45 (MUST) — hot reload.** Editing the file updates the running effect without
a rebuild.

**R46 (MUST) — an in-game editor as a dev-panel tab**, built on the existing
[`cromwell/ui/`](../../src/cromwell/ui) draw-list widget kit, with the profiler tab
as the precedent. Live sliders over the active system's parameters, save back to
R44's format.
*Why:* this is the requirement that replaces "a year of tool work" with "weeks",
and it does something a standalone tool cannot — tune the effect **in the real
scene, under the real lighting, at the real camera angle**.

**R47 (SHOULD) — quality tiers are a provider, not a system.** `DetailLevel` as
an input any parameter can read (R10), rather than a separate LOD mechanism.
*Why:* notably cheap, and it makes "half the particles on low" an authoring
decision per effect instead of an engine policy.

**R48 (SHOULD) — the editor shows live counts and the fill-cost estimate** for
the system being edited, so R40's budget is visible where it is spent.

## 12. Engine boundary, layout and instrumentation

**R49 (MUST) — the system lives in `cromwell` and names nothing under `game/`.**
World access is R28's callback; skeleton access will be an equivalent interface.
Judge every feature against RTS, FPS and third-person, not against this game.

**R50 (MUST) — one profiler zone, `particles`**, added in the same commit as the
system. Sub-zones only once a measurement points at one. GPU work gets a paired
`CW_GPU_ZONE`, and GPU zones do not nest.

**R51 (MUST) — no allocation, no map lookups and no linear scans inside
per-particle loops.** Queries fill caller-supplied buffers. Running indices
rather than recomputed ones.

**R52 (MUST) — strict encapsulation is preserved.** The SoA columns are *private*
members of the collection, reached through typed accessors returning strided
spans/iterators — the pattern Valve use with their attribute iterators. Public
data members are not an acceptable price for a data-oriented layout, and they are
not necessary to get one.

**R53 (SHOULD) — proposed module layout**, respecting the 6–7-unit folder rule:

```
cromwell/particles/
    Attributes.hpp        the enum, masks, and the constants that cap them
    Provider.hpp/.cpp     §4 — providers and the remap stage
    ControlPoints.hpp     §6
    ParticleDef.hpp/.cpp  a loaded effect: functions, masks, budgets
    Collection.hpp/.cpp   the live instance — columns, simulate, render list
    World.hpp/.cpp        the manager; update/render entry points; the callbacks
    fn/                   the functions, grouped by category
    render/               sprite renderer, sort, batching
```

**R54 (MUST) — tests.** In `xcom_tests`: that declared masks match actual
allocation (R4–R6), that an undeclared write asserts (R7), and that a fixed seed
reproduces a fixed result (R21). In `xcom_perf`: a row for simulation cost at a
few particle counts, which is also the evidence R9 asks for before any SIMD work.

## 13. Non-requirements

Written down so they are not re-argued:

| | Why not |
|---|---|
| **GPU compute simulation** | It gives up readback, events and world queries — which is this document's §6, §7 and §8. It buys count, and count is not the constraint. If ambient weather ever needs 100k particles, that is a *second, narrower* system, exactly as Unreal, Unity and Godot each did ([`source2_particles.md`](../games/valve/source2_particles.md) §13.2). |
| **Hand-written SIMD** | R9. Structural layout yes, constant factors on measurement. |
| **A node-graph material editor** | Effekseer and Source are both ahead here and will stay ahead. Particle materials are ordinary shaders in `assets/shaders/` for a long time. |
| **A standalone editor application** | R46. The dev panel is the editor, and it is better positioned than a separate tool. |
| **Source's operator catalogue** | §1. ~166 functions is fifteen years of requests we do not have. |
| **An embedded expression language** | Source has `SetAttributeToScalarExpression`. Providers plus remap curves (§4) cover the same ground declaratively. |
| **Rope/cable/sound/light particles** | R39. Sound and light have their own systems here. |
| **An abstraction over "our system or Effekseer"** | An abstraction whose implementations disagree about whether gameplay can read the result is an abstraction that lies. If Effekseer is ever added for set-piece work, it is added *alongside*, with its own pass. |

## 14. Phases

Each phase ends at something demonstrable.

**Phase 0 — the machine.** R1–R8 (columns, masks, constants, asserts), R10–R12
(providers and remap), R13–R14 (function base, execution order), R18, R21–R24
(control points), R49–R52.
*Ends when:* one hard-coded effect — a puff of smoke — runs, with a profiler zone
and the R54 tests passing. No file format yet.

**Phase 1 — usable.** R44–R46 (data format, hot reload, editor tab), the first
function set (continuous + instantaneous emitters; sphere/box position, random
lifetime/radius/rotation/colour/alpha/velocity initializers; decay, basic
movement, fade, interpolate radius, spin operators; one directional force),
R34–R37 (sprite renderer, sort, flipbooks), R26 (impact-point control point),
R27 tiers 0–1 (plane cull, planar/box constraints), R42.
*Ends when:* a muzzle flash, an impact and a grenade explosion are authored
entirely in the editor by someone who does not rebuild.

**Phase 2 — depth.** R19 (fade envelopes), R38 (soft particles), R40–R41 (fill
budget, end cap), R43 (children), R47 (detail level), R17.
*Ends when:* effects survive contact with a real scene — they fade, they respect
a budget, they end properly when the thing that spawned them dies.

**Phase 3 — the world.** R28 (the trace query and callback), R27 tiers 2–3,
R29 (plane cache), R30.
*Ends when:* sparks bounce off cover.

**Phase 4 — characters.** R31–R33, **after ozz-animation exists.**
*Ends when:* a unit can be on fire.

## 15. Open questions

1. **Does the trace query in R28 belong to the particle system at all**, or is it
   a general engine service that navigation, LOS and particles all consume? The
   second is likely right and is a bigger decision than this note should make.
2. **Where do decals sit?** `cromwell/decal/` already exists. An impact effect
   wants a particle burst *and* a decal, and it is not obvious whether the
   particle system should be able to spawn one, or whether both are spawned by a
   third thing.
3. **R41's end-cap phase — how much of it in Phase 0?** The *phase* is cheap to
   model early and expensive to retrofit; the operators are not. The split
   proposed here (model early, operators in Phase 2) is a guess.
4. **Does `ParticleId` need to be stable across a save/load?** Only matters if
   effects persist across saves, which is a game-design question nobody has
   asked yet.
5. **R9's revisit trigger is vague.** "A measurable share of the frame" should
   become a number once `xcom_perf` has a particle row.
