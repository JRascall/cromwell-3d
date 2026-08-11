# Source 2 particles — and the buy-versus-build answer

Two questions in one note, because the second is unanswerable without the first.
**What is Source 2's particle system actually made of**, and **is there a free
library that gives us it, or something good enough, without writing one.**

The short version of the second, up front, so the long version can be read
against it: **there is exactly one credible off-the-shelf answer — Effekseer,
MIT, actively released through 2026 — and it is a different architecture solving
a narrower problem.** It is an *effect player*: you author a tree of nodes in its
editor, ship a `.efk`, and it draws. Source 2's system is a *simulation
framework*: a flat stack of operators over a SIMD array, wired to gameplay by
64 control points, able to trace against the world mid-simulation. Effekseer will
cover muzzle flashes, explosions, impacts and smoke. It will not cover a particle
that asks the tile grid a question. §11 is the honest ledger.

**[`particle_architecture.md`](particle_architecture.md) is the decision this note
feeds** — the numbered requirements for what `cromwell` actually builds. This
note is what Source 2 does; that one is what we do about it. Read this for the
reasoning and that one to implement.

**Appendix A is the replication spec** — the ~142 named functions Source 2 ships,
read from a third-party reader's re-implementation, with the confirmed execution
order, the **parameter provider system** (the highest-value idea in the whole
note, and not an operator at all), what the shape of the catalogue reveals, and a
tiered minimum subset. §1–9 are the machine; Appendix A is the content.

**§13 answers the question this note is most often interrupted by** — *shouldn't
this all be on the GPU?* Short version: GPU simulation and fast particle
rendering are separate claims, everyone who built a GPU path kept the CPU one
alongside it rather than replacing it, and the thing that actually kills particle
frame time is overdraw, which compute does nothing about.

## 0. Sourcing

Source 2 is closed. This note is graded, like the rest of the directory:

| Tag | Means |
|---|---|
| `[VALVE-SDK]` | Read from `Tools/source-sdk-2013-master/src/public/particles/particles.h` on this machine — Source **1**'s particle system, 2612 lines, the direct ancestor. Every line number below is real. |
| `[S2V]` | Source 2 Viewer's published API for the `.vpcf` resource type — a third party's reader, but one that reads Valve's shipped files, so the *category names* are Valve's. |
| `[VDC]` | Valve Developer Community / source2.wiki — Valve's own docs and the Dota/CS2 workshop tooling. |
| `[inferred]` | Reasoning. Marked every time. |

**The central methodological point:** Source 1's particle system was rewritten
*in place* for Source 2, not replaced. §7 shows the function taxonomy
`[VALVE-SDK]` enumerates in 2013 is, term for term, the taxonomy Source 2 Viewer
reads out of a 2024 `.vpcf` — with exactly one addition. So the public header is
not a historical curiosity; it is the only legible description of the shipping
architecture, and the parts of it that survived are identifiable by name.

What this note does **not** have: the operator *implementations*. SDK 2013 ships
`public/particles/particles.h` and nothing else — the `particles` library itself
is closed, so every `Operate()` body is unread. The architecture is
fully readable; the physics inside any given operator is not.

---

## 1. The thesis

A Source particle system is **a stack of small pure-ish functions over a
struct-of-arrays, where every function declares which columns it reads and which
it writes.**

That one sentence generates almost everything else: the SIMD layout, the ability
to not allocate unused columns, the debug assertions, the editor, and the reason
it scales. It is a data-oriented design that predates the term being fashionable
and — this is the part worth internalising — **the data-orientation is not the
interesting bit. The declared dataflow is.**

---

## 2. Particles are columns, not structs

`[VALVE-SDK]` `particles.h:56–128`. There is no `Particle` type. There is a fixed
menu of **32 attribute slots**, of which 22 are defined:

```c
#define MAX_PARTICLE_ATTRIBUTES 32

DEFPARTICLE_ATTRIBUTE( XYZ, 0 );              DEFPARTICLE_ATTRIBUTE( ALPHA2, 16 );
DEFPARTICLE_ATTRIBUTE( LIFE_DURATION, 1 );    DEFPARTICLE_ATTRIBUTE( TRACE_P0, 17 );
DEFPARTICLE_ATTRIBUTE( PREV_XYZ, 2 );         DEFPARTICLE_ATTRIBUTE( TRACE_P1, 18 );
DEFPARTICLE_ATTRIBUTE( RADIUS, 3 );           DEFPARTICLE_ATTRIBUTE( TRACE_HIT_T, 19 );
DEFPARTICLE_ATTRIBUTE( ROTATION, 4 );         DEFPARTICLE_ATTRIBUTE( TRACE_HIT_NORMAL, 20 );
DEFPARTICLE_ATTRIBUTE( ROTATION_SPEED, 5 );   DEFPARTICLE_ATTRIBUTE( CONTROL_POINT_INDEX, 21 );
DEFPARTICLE_ATTRIBUTE( TINT_RGB, 6 );
DEFPARTICLE_ATTRIBUTE( ALPHA, 7 );
DEFPARTICLE_ATTRIBUTE( CREATION_TIME, 8 );
DEFPARTICLE_ATTRIBUTE( SEQUENCE_NUMBER, 9 );
DEFPARTICLE_ATTRIBUTE( TRAIL_LENGTH, 10 );
DEFPARTICLE_ATTRIBUTE( PARTICLE_ID, 11 );
DEFPARTICLE_ATTRIBUTE( YAW, 12 );
DEFPARTICLE_ATTRIBUTE( SEQUENCE_NUMBER1, 13 );
DEFPARTICLE_ATTRIBUTE( HITBOX_INDEX, 14 );
DEFPARTICLE_ATTRIBUTE( HITBOX_RELATIVE_XYZ, 15 );
```

Three of these are worth stopping on, because they are the ones that say what
kind of system this is:

- **`PREV_XYZ`** — there is no velocity attribute. Motion is **Verlet**: position
  and previous position, and `SwapPosAndPrevPos()` (`:1543–1547`) is a *pointer
  swap* of the two column base pointers, asserting first that their strides
  match. Velocity is `XYZ − PREV_XYZ`. This is why forces accumulate into an
  acceleration array (§5) and why position constraints (§6) can just move a
  particle and have the velocity change fall out for free.
- **`TRACE_P0` / `TRACE_P1` / `TRACE_HIT_T` / `TRACE_HIT_NORMAL`** — four columns
  reserved for **world raycast results**. A particle system can trace against
  the map, cache the hit, and have later operators read it. This is the
  capability no off-the-shelf library in §10 has, and it is the whole reason
  Source's particles can spark off a wall or slide down it.
- **`HITBOX_INDEX` / `HITBOX_RELATIVE_XYZ`** — particles bound to a *skeleton*,
  stored in hitbox-local space so they follow an animating character. Blood,
  burning, ice. Again: a gameplay coupling, not a VFX feature.

### 2.1 Four particles at a time, permanently

`[VALVE-SDK]` The array is not `float[n]` per attribute. It is **blocks of four**,
laid out for SSE:

```c
int m_nActiveParticles;
int m_nPaddedActiveParticles;   // # of groups of 4 particles      :1318–1321
...
m_nPaddedActiveParticles = ( nCount + 3 ) / 4;                    // :1539
```

Accessors come in three flavours, and the comment on the scalar one is the
project's own performance discipline stated by Valve (`:1085–1088`):

> ```
> // get the pointer to an attribute for a given particle.
> // !!speed!! if you find yourself calling this anywhere that matters,
> // you're not handling the simd-ness of the particle system well
> // and will have bad perf.
> ```

The vector accessors return strided SIMD pointers — `fltx4*` with the stride
divided by 4, `FourVectors*` with it divided by 12 (`:1919–1952`). `FourVectors`
is four vec3s transposed into three `fltx4`s: xxxx / yyyy / zzzz. So a force
operator does not loop over particles adding a vector; it loops over *blocks*
adding three registers.

Operators are `operator new`-overridden purely so the operator objects
themselves land SIMD-aligned (`:575–579`).

**Budgets** `[VALVE-SDK]` `:121–134`: 64 control points, 32 attributes,
`MAX_PARTICLES_IN_A_SYSTEM` 5000 (2000 under the other arm of a `#if`, almost
certainly the console build `[inferred]`).

---

## 3. The declared dataflow — the actual crown jewel

Every operator must answer, statically, what it touches `[VALVE-SDK]`
`:595–606`:

```c
virtual uint32 GetWrittenAttributes( void ) const = 0;      // pure virtual — not optional
virtual uint32 GetReadAttributes( void ) const = 0;         // pure virtual
virtual uint64 GetReadControlPointMask() const;             // 64 bits, one per control point
virtual uint32 GetReadInitialAttributes( void ) const;      // "...at spawn time"
```

Both attribute masks are **pure virtual**. You cannot write an operator that
declines to declare itself. The definition then ORs them together across the
whole stack (`:2204–2206`, `:2290–2292`):

```c
int m_nPerParticleUpdatedAttributeMask;
int m_nPerParticleInitializedAttributeMask;
int m_nInitialAttributeReadMask;
int m_nAttributeReadMask;
uint64 m_nControlPointReadMask;
```

Four things fall out of that, and each is worth more than the SIMD:

**(a) Unused columns are never allocated.** A system whose particles never rotate
allocates no `ROTATION` array. Sitting right beside the masks are the fallbacks
(`:2222–2226` region):

```c
float m_flConstantRadius;
float m_flConstantRotation;
float m_flConstantRotationSpeed;
int   m_nConstantSequenceNumber;
int   m_nConstantSequenceNumber1;
```

**If no initializer writes an attribute, it stops being an array and becomes a
scalar in the definition.** That is not a micro-optimisation — it changes the
memory traffic of the whole simulation by removing streams, which is CLAUDE.md's
"do less work" rule applied to *data* rather than to code.

**(b) The declaration is enforced, in debug, at the point of use.**
`:1965–1966`:

```c
Assert( !m_bIsRunningInitializers || ( m_nPerParticleInitializedAttributeMask & (1 << nAttribute) ) );
Assert( !m_bIsRunningOperators    || ( m_nPerParticleUpdatedAttributeMask     & (1 << nAttribute) ) );
```

An operator that lies about its writes trips an assert the first time it writes.
This is the difference between a declared dataflow and a documented one.

**(c) The control-point read mask is per-*point*, 64 bits wide.** So the engine
knows which of the 64 gameplay inputs a system actually consumes, and
`MarkReadsControlPoint` / `ReadsControlPoint` (`:2183–2185` region) let the game
skip updating the rest.

**(d) Initial values are a separate buffer, allocated on demand.** `[VALVE-SDK]`
`:1379`, `m_pParticleInitialAttributes[]` with its own strides, plus
`CopyInitialAttributeValues()` (`:1265`) and `CM128InitialAttributeIterator`
(`:1440`). "Fade to 30% of the radius you *spawned* with" needs the spawn radius
to still exist at t=3s. Rather than making every operator that wants this keep
its own copy, the framework snapshots — but **only the columns somebody declared
via `GetReadInitialAttributes()`**. Same pattern, one level up.

> Taking just this — masks, allocation-from-masks, assert-from-masks — and none
> of the rest of Source's design, would be a good day's work in any particle
> system, ours included.

---

## 4. Seven kinds of function

`[VALVE-SDK]` `:146–156`:

```c
enum ParticleFunctionType_t
{
    FUNCTION_RENDERER = 0,
    FUNCTION_OPERATOR,
    FUNCTION_INITIALIZER,
    FUNCTION_EMITTER,
    FUNCTION_CHILDREN,      // "a fake function type, only here to help eliminate
                            //  a ton of duplicated code in the editor"
    FUNCTION_FORCEGENERATOR,
    FUNCTION_CONSTRAINT,
    PARTICLE_FUNCTION_COUNT
};
```

They are all the same C++ class, `CParticleOperatorInstance`, distinguished by
which virtual they override — a single vtable with seven disjoint entry points
rather than seven base classes:

| Type | Entry point `[VALVE-SDK]` | Runs |
|---|---|---|
| Emitter | `Emit(coll, strength, ctx) -> uint32` `:638` | per frame; **returns a mask of the fields it initialised** |
| Initializer | `InitNewParticlesScalar(coll, first, n, writeMask, ctx)` `:707` | once, per particle, at birth |
| | `InitNewParticlesBlock(coll, startBlock, nBlocks, ...)` `:712` | the 4-wide version; **default forwards to the scalar one 4× times** |
| Operator | `Operate(coll, strength, ctx)` `:609` | per frame, over all particles |
| Force generator | `AddForces(FourVectors *accum, coll, nBlocks, strength, ctx)` `:660` | per frame; **accumulates, never writes position** |
| Constraint | `EnforceConstraint(startBlock, nBlocks, coll, ctx, nValidInLastChunk) -> bool` `:678` | per frame, **possibly many times** |
| Renderer | `Render(ctx, coll, ctx)` `:614` / `RenderUnsorted(...)` `:624` | per frame, per view |
| Children | — | not a function; a nested system |

Three details in that table are load-bearing:

**Emitters return a mask.** `Emit()` returns which attributes it initialised
itself, so the initializer pass can skip those — an emitter that places particles
along a path has already written `XYZ`, and no position initializer should
overwrite it. Dataflow again, but dynamic this time.

**Forces accumulate; they do not integrate.** `AddForces` gets a `FourVectors*`
accumulator and the block count. Every force in the stack adds into the same
array, and one integration step consumes it. This is why forces compose and why
adding a fourth force costs one more pass rather than one more integrator.

**Constraints are separated from everything else, and iterate.** They get their
own per-frame setup hook (`SetupConstraintPerFrameData`, `:671` — "It can set up
data like nearby world traces"), a block range so they can be run over subsets,
and `IsFinalConstraint()` (`:688`) for one that must run last after all the
others have relaxed. The header carries the design note that never got done
(`:917–918`):

> ```
> // need to think about particle constraints in terms of segregating affected
> // particles so as to run multi-pass constraints on only a subset
> ```

This is a **position-based dynamics** solver, and it is the reason `PREV_XYZ`
exists rather than a velocity column: a constraint moves the particle, and the
velocity correction is implicit.

### 4.1 The rest of the operator interface

Small virtuals, each of which exists because something broke `[VALVE-SDK]`:

| | |
|---|---|
| `GetRequiredContextBytes()` `:586` + `InitializeContextData()` `:591` | operators are **shared, stateless instances**; per-collection state lives in a byte block the framework allocates and hands back as `void *pContext`. Same shape as Unreal's behaviour-tree node memory in [`ai_state_machines.md`](ai_state_machines.md) §5.6, reached independently. |
| `ShouldRunBeforeEmitters()` `:735` | an operator that must see the world *before* this frame's particles exist. Source 2 promotes this to a category — see §7. |
| `RequiresOrderInvariance()` `:741` | "does this operator require that particles remain in the order they were emitted?" Killing a particle is normally a swap-with-last; a ribbon renderer forbids it. |
| `MayCreateMoreParticles()` `:724` | how a system knows it is *finished* — no particles and nobody intends to make more. |
| `IsScrubSafe()` `:701` / `SkipToTime()` `:747` | the Source Filmmaker tax: an operator using random numbers cannot be scrubbed backwards, and must say so. |
| `IsBatchable()` `:619` | whether this renderer's draws can merge with a neighbour's. |
| `RandomInt`/`RandomFloat` that `Assert(0)` `:757–776` | **a deliberate trap.** The globals are shadowed by poisoned members so that calling them inside an operator fails loudly; you must use `CParticleCollection::RandomFloat`, which is seeded per collection and therefore reproducible. |

### 4.2 Every operator has a fade envelope

`[VALVE-SDK]` `:778–782`, on the base class, so **every** function of every type
has these:

```c
float m_flOpStartFadeInTime;
float m_flOpEndFadeInTime;
float m_flOpStartFadeOutTime;
float m_flOpEndFadeOutTime;
float m_flOpFadeOscillatePeriod;
```

which is what the `flOpStrength` argument to `Operate()` and `Emit()` carries.
So gravity can fade in over the first half-second and oscillate; drag can switch
off at t=2. **The envelope is framework-level, not per-operator**, so no operator
implements its own "when am I active" logic and the editor gets one uniform UI
for it. Cheap, and it removes a whole category of duplicated parameter.

---

## 5. Control points — the gameplay interface

`[VALVE-SDK]` `:1063–1083`. 64 slots, each far more than a position:

```c
void SetControlPoint( int n, const Vector &v );
void SetControlPointObject( int n, void *pObject );
void SetControlPointOrientation( int n, const Vector &fwd, const Vector &right, const Vector &up );
void SetControlPointOrientation( int n, const Quaternion &q );
void SetControlPointForwardVector( int n, const Vector &v );   // and Up, Right
void SetControlPointParent( int n, int nParent );              // CPs form a hierarchy
void SetControlPointVelocity( int n, Vector vVel );            // "if unset, vel is [0 0 0]"
void SetControlPointRadius( int n, float flRadius );           // "if unset, radius is 0"
void SetControlPointDensity( int n, float flDensity );
void SetControlPointDuration( int n, float flDuration );
```

**This is the entire coupling surface between gameplay and VFX**, and its design
is the reason a Source 2 effect can be reconfigured without a code change: the
game writes numbered slots, the effect decides what slot 3 means. `[VDC]` Source 2
generalises the slots further, to "general-purpose variables of various types"
rather than strictly vectors.

`SetControlPointObject` is the one to note — an opaque pointer, compared for
identity against the camera at render time (`:1113`, *"the camera object may be
compared for equality against control point objects"*). That is how "don't draw
this if the camera is attached to it" works without the particle library knowing
what a camera is.

---

## 6. Rendering

**Sorting has its own SoA, and the header shouts about it** `[VALVE-SDK]`
`:934–950`:

```c
// **do not casually change this structure**. The sorting code treats it
// interchangably as an SOA and accesses it using sse.
struct ParticleRenderData_t
{
    float m_flSortKey;    // what we sort by
    int   m_nIndex;       // index or fudged index (for child particles)
    float m_flRadius;     // effective radius, using visibility
    uint8 m_nAlpha;       // effective alpha, combining alpha and alpha2 and vis
    uint8 m_nAlphaPad[3];
};
```

16 bytes, endian-swapped field order so the alpha byte lands correctly either
way, and the sort runs over it as columns. Two builders exist —
`GenerateSortedIndexList` and `GenerateCulledSortedIndexList` (`:1259–1260`) —
the second taking a forward vector, so **frustum rejection happens during list
generation rather than as a separate pass**.

Note what is *not* in the struct: position. `ExtendedParticleRenderData_t`
(`:952`) adds x/y/z for the renderers that need it. The common path sorts 16-byte
records.

**Flipbooks are first-class.** `CSheet` (`:1244`, `:372–376`) with a manager-level
cache keyed by material — animated sprite sheets, with `SEQUENCE_NUMBER` and
`SEQUENCE_NUMBER1` as particle attributes so two sequences can blend.
[`helldivers2_vfx.md`](helldivers2_vfx.md) documents the same idea from the other
end (`rows_and_columns`, `use_flipbook_blending` in the shader parameters), which
is a useful cross-check that this is industry-standard rather than Valve-specific.

**Renderer types** `[VDC]`: `render_sprites` (the ordinary billboard),
`render_sprite_trail` (stretched along velocity, length driven by the
`TRAIL_LENGTH` attribute), `render_rope` (all particles chained into one
continuous strip, subdivided by `texel_size`), `render_models` (a mesh per
particle). Source 2 adds projected and cable renderers that reference `.vmat`
rather than `.vtex`, and — per Valve's own CS2 docs — particle systems can render
lights and text and run physics, i.e. **the particle system is used as a general
"spawn a bunch of transient things" mechanism**, not just as a sprite drawer.

### 6.1 Visibility, and paying for fill rate

`[VALVE-SDK]` `:895–908`. Every renderer carries a `CParticleVisibilityInputs`
block, unpacked from the file as named fields:

```
"Visibility Proxy Input Control Point Number"   "Visibility Proxy Radius"
"Visibility input minimum" / "maximum"          "Visibility Camera Depth Bias"
"Visibility Alpha Scale minimum" / "maximum"    "Visibility Radius Scale minimum" / "maximum"
```

A control point acts as an occlusion *proxy*; the resulting visibility fraction
remaps into both an alpha scale and a radius scale. So an effect fades **and
shrinks** when its proxy is occluded, rather than popping.

Alongside it, on the definition (`:2215`, `:2245–2247`, and `:2170–2186` region):

```c
float m_flCullRadius;         float m_flCullFillCost;    int m_nCullControlPoint;
int   m_nRetireCheckFrame;    bool  HasRetirementBeenChecked( int nFrame ) const;
float m_flMaxDrawDistance;    float m_flNoDrawTimeToGoToSleep;
int   m_nMaxParticles;        int   m_nSkipRenderControlPoint;
float m_flMaximumTimeStep;    float m_flMaximumSimTime;  float m_flMinimumSimTime;
int   m_nMinimumFrames;
```

**`m_flCullFillCost` is the interesting one.** Systems are retired against a
*screen-fill* budget, not a count budget — the thing that actually costs money on
a translucent effect is overdraw, so the culling metric is overdraw, and
`HasRetirementBeenChecked(frame)` exists so the check runs once per frame across
however many collections share a definition. `[inferred]` The pairing of
`m_flCullRadius` with a nominated `m_nCullControlPoint` means the estimate is
"projected area of a sphere of this radius at this point", which is cheap enough
to run over every live system.

`m_flMaximumSimTime` / `m_flMinimumSimTime` / `m_nMinimumFrames` are the
"pre-roll" controls — how far a system is simulated *before its first frame is
drawn*, so a smoke column doesn't visibly grow from nothing when you walk into
the room, with a minimum so that **every instance doesn't pre-roll by the same
amount and look identical** (the comment at `:2231` is cut off mid-sentence in
the header — *"prevents all"* — but the intent is clear `[inferred]`).

---

## 7. What Source 2 changed

`[S2V]` Source 2 Viewer reads a `.vpcf` (KV3, NTRO-typed) and exposes exactly
these accessors:

```
GetEmitters()        GetInitializers()     GetOperators()      GetRenderers()
GetForceGenerators() GetConstraints()      GetChildren()       GetPreEmissionOperators()
```

Line that up against `ParticleFunctionType_t` from 2013 (§4) and the answer is:
**identical, plus one.**

- **Pre-emission operators became their own category.** In Source 1 this was
  `ShouldRunBeforeEmitters()`, a boolean on an operator (§4.1). In Source 2 it is
  a separate list in the file. `[inferred]` This is the right change — the flag
  version means the executor must scan the operator list twice and sort by a
  predicate, and the artist cannot see the ordering. `[VDC]` The editor uses them
  for "manipulating control points or adjusting simulation timescale" — i.e.
  operators that write the *inputs*, before anything reads them.
- **Children carry metadata**: `[S2V]` "delay, endcap, and detail level". Endcap
  is the sub-effect that plays when the parent stops — the thing every rocket
  trail needs and every hand-rolled system bolts on badly. Detail level is
  particle LOD, per child.
- **Control points became typed general-purpose variables** `[VDC]`, not just
  vec3s.
- **Function priority is explicit and order-based** `[VDC]`: where two identical
  functions coexist, the lowest in the stack wins. `[inferred]` Worth flagging as
  a trap of exactly the kind [`source2_animation.md`](source2_animation.md) §11
  documents for AnimGraph transitions and [`ai_state_machines.md`](ai_state_machines.md)
  §8 counts three separate instances of across Valve and Epic: **implicit
  ordering that only bites in the rare case.** It is a bug generator every time.

Everything else — the attributes, the SIMD blocks, the masks, the control points,
the Verlet integration, the constraint solver, the sheets — is the 2013 design
still running. `[inferred]` but strongly: the categories, the file's field names
and the editor's vocabulary all match, and no Valve statement suggests a rewrite.

---

## 8. Per-frame order

**Confirmed** — see Appendix A.1. A Dota 2 particle-editor guide states the
order outright: *pre-emission operators execute before all other functions,
followed by emitters, initializers, operators, force generators, constraints, and
renderers.* That is the order below, arrived at independently from the vtable
before the source was found. The remaining `[inferred]` parts are only the
placement of the initial-attribute snapshot and of `RecomputeBounds`.

```
1  pre-emission operators        (Source 2 category; Source 1 flag)
2  emitters          Emit()      -> returns mask of attributes it initialised
3  initializers      InitNewParticlesBlock()/Scalar() on the new range only,
                                   masked by what the emitter already wrote
4  copy initial attribute values for the declared columns
5  operators         Operate( strength from the fade envelope )
6  force generators  AddForces() -> one shared FourVectors accumulator
7  integrate         XYZ / PREV_XYZ Verlet step, then swap the two base pointers
8  constraints       SetupConstraintPerFrameData(), then EnforceConstraint()
                     over block ranges, iterated; IsFinalConstraint() ones last
9  bounds            RecomputeBounds()
10 children          recurse
-- render pass --
11 visibility        proxy -> alpha scale, radius scale
12 sort              ParticleRenderData_t, culled or not
13 renderers         batched where IsBatchable()
```

The step-4 placement is a genuine guess: it could equally sit inside step 3.
`[inferred]`

---

## 9. What is worth stealing, ranked

Independent of whether we write one or buy one — these are ideas, and three of
them apply to systems this codebase already has:

1. **Declared read/write masks, used to allocate.** §3. The largest idea here,
   and the one with the widest application: an attribute nobody writes is a
   scalar, not an array.
2. **Assert the declaration at the point of use.** §3(b). Two lines. Turns a
   convention into a checked invariant.
3. **Constants for unused per-particle columns.** §3(a).
4. **Forces accumulate into a shared array; one integrator consumes it.** §4.
   Composition for free.
5. **A framework-level fade envelope on every function.** §4.2. Removes a
   duplicated parameter from every operator anyone will ever write.
6. **Cull on estimated fill cost, not on count.** §6.1. Overdraw is the bill;
   measure the bill.
7. **Poison the global RNG inside operators.** §4.1. Reproducibility enforced by
   a compile-time trap rather than a code-review rule.
8. **Emitters return what they initialised.** §4. Lets the initializer pass skip
   work rather than overwrite it.

And one anti-pattern to *not* copy: implicit priority by stack order (§7).

---

## 10. The library survey

The brief was: don't roll our own. Here is everything real, with licence and
verdict. `[COMMUNITY]` throughout except where linked to a licence page.

| Library | Licence | What it actually is | Verdict |
|---|---|---|---|
| **Effekseer** | **MIT** | Standalone **editor + C++ runtime**. Backends: OpenGL, DX9/11/12, Vulkan, Metal, WebGL. v1.80.6, releases through 2026 (1.80.3 May 2026, 1.80.2 Apr 2026). Node-tree model, not operator-stack. | **The only serious candidate.** §11. |
| **PopcornFX v2** | **Commercial** | The best editor in this space. Runtime SDK is a C++ framework for any engine — but *"under commercial proprietary license"* and *"cannot be modified by licensees"*. Free tier (PLE) is **non-commercial only**; indie tier under $200k revenue; perpetual licence after a year of subscription. Only the *engine integration* code is open (Community Licence). | Out, on the brief. Reconsider only if VFX becomes a headline feature and the budget exists. |
| **Defold `dmParticle`** | Defold Licence (Apache 2.0 **minus** the right to commercialise the engine/editor) | Genuinely a *standalone subsystem* by design — `engine/particle/src/particle.cpp`, emitters with spawn-rate curves, acceleration/drag/radial/vortex modifiers, colour/scale/rotation curves over particle age. Clean, small, readable. | **Code donor, not a solution.** Authoring lives in the Defold editor, which you cannot lift. Shipping a *game* with it is fine under the licence; shipping `cromwell` as an engine product is the part to read the licence about. |
| **Particle Universe** | **MIT** (`OGRECave/particleuniverse`) | The classic Ogre ParticleFX replacement — emitters/affectors/observers/behaviours, a genuinely rich feature set, and it had a commercial editor. | Ogre-coupled: `Ogre::` types through the whole API. Extracting it is a rewrite wearing a donor's clothes. Read the **affector/observer** taxonomy, take nothing. |
| **SPARK 2** | zlib `[COMMUNITY]` | Portable C++ particle engine, emitters/modifiers/zones/renderers with Irrlicht/Ogre/GL backends. Architecture is close to Source's. | **Effectively dead** — last real activity ~2015. Taking it means adopting an unmaintained dependency and writing a modern renderer for it anyway. |
| **Godot 4 particles** | MIT | GPUParticles3D: the whole simulation is a *process material* shader, which is a legitimately good design and the closest thing to a modern reference implementation you may legally read and copy. | Not liftable (RenderingServer-coupled) but **the best free reading** on GPU-side particle simulation. Same role Godot plays for IK in [`rigging_ik.md`](rigging_ik.md) §6. |
| **The `.vpcf` route** | — | CS2 Workshop Tools include Valve's actual particle editor, and `.vpcf` is KV3 text. Source 2 Viewer proves it is parseable. | **Don't.** You would be reimplementing every operator (§0 — the implementations are closed), and shipping assets authored in Valve's tools inside a non-Source commercial game is a licence question with an unattractive answer. Fine for study, which is what §1–9 are. |
| **Sparkle** (`tcoppex/sparkle`) | **MIT** | The best-realised free GPU particle engine: **entirely compute-shader**, C++14 / GL 4.4, bitonic sort for alpha blending, curl noise and 3D vector fields, and both SoA and AoS layouts to compare. Inspired by Square Enix's *Agni's Philosophy*. | **Reference, not a dependency** — it is a *demo application*, not a library (GLFW/GLM/imgui baked in), ~196 stars, and the author notes it is optimised for Linux with Windows performance suboptimal. If §13.6's GPU path ever happens, **this is the thing to read first**; our GL 4.3 is one version below its 4.4. |
| **libpartikel**, **RayParticle3D** | MIT/zlib | raylib-adjacent particle helpers. `libpartikel` is header-only **C99, 2D, self-described alpha** with an unstable API. | No. Relevant only because the tree currently links raylib; neither is close to what a 3D effect needs. |
| GitHub compute-shader particle demos (`diharaw`, `Crisspl`, `Nelarius`, GParticles) | various | 2M-particle compute-shader demos. | Demos. Useful to read for the indirect-draw/compaction pattern; none is a library, and Sparkle is the better-realised version of all of them. |
| Unreal Niagara / Unity VFX Graph | EULA | — | Not available outside their engines. Named only to close the list. |

---

## 11. Effekseer, honestly

**It is MIT, it is alive, it ships an editor, and the editor is the thing you
cannot write.** That last point decides the recommendation: the code in §1–9 is a
few weeks of work; a tool an artist will accept is a year, and this project has
no artist to complain, which means an in-house system's parameters would be tuned
by editing a header and rebuilding. That is the real cost of rolling our own, and
it is not the simulation.

**What it is architecturally.** An effect is a **tree of nodes**. Each node has
Generation (count/rate), Position, Rotation, Scale, Draw and Sound parameter
blocks; positions can be fixed, PVA, eased, F-curve driven or NURBS-path driven;
children inherit from parents. Draw types are **Sprite, Ribbon, Ring, Track,
Model** — Track being a ribbon with independent outer/inner/centre colour and
size. Forces are a per-node **Force Field** block: directional/gravity,
attraction with distance attenuation, damping, and **turbulence** with a flow
scale and strength (its docs warn plainly that strong turbulence "becomes
heavy"). Soft particles and background distortion are supported, the latter via a
red/green offset texture with an explicit draw-priority rule for what distorts
what.

**Read that against §4 and the difference is clear**: Effekseer's expressiveness
lives in *curves per node in a hierarchy*; Source 2's lives in *a composable
stack of operators over declared columns*. Effekseer is the better authoring
model for a hand-made effect. Source 2 is the better model for an effect that
must interact with a world.

### 11.1 Is it on par with Source 2?

**No — but the gap is narrower than the architecture difference suggests, and it
falls almost entirely on one side: the coupling to gameplay and the world.** The
*authoring* side is close to parity and in two places ahead.

Checked against Effekseer's own tool reference `[COMMUNITY]` rather than assumed:

| Capability | Source 2 | Effekseer | |
|---|---|---|---|
| Sprite / billboard | `render_sprites` | Sprite node | **par** |
| Trails | `render_sprite_trail` (velocity-stretched, `TRAIL_LENGTH`) | Ribbon and **Track** (independent outer/inner/centre colour and size) | **par** |
| Ropes / chains | `render_rope`, subdivided | Ribbon | **par-ish** |
| Mesh particles | `render_models` | Model node, plus **procedural model generation** | **Effekseer ahead** |
| Flipbook sheets | `CSheet`, two sequences blendable | UV animation with blending | **par** |
| Soft particles | yes | yes | **par** |
| Screen distortion | yes | yes, R/G offset texture with an explicit draw-priority rule | **par** |
| Custom shading | material system | **node-graph material editor** generating per-backend shaders | **Effekseer ahead** |
| Forces | force generators, accumulated | Force Field: directional/gravity, attraction with distance attenuation, damping, **turbulence** | **par** |
| Parameter animation | operators over lifetime, envelopes | **F-curves**, easing, PVA, **NURBS paths** | **Effekseer ahead on authoring curves** |
| Culling / LOD | `m_flCullFillCost`, `m_flMaxDrawDistance`, sleep | Culling and Levels-of-Detail pages exist | **unclear — see below** |
| Sub-effects on death | children with **endcap** | Triggers 0–3, **On Parent Removed**, **On Parent Collision** | **par** |
| Collision | full world traces, 4 attributes, results readable by later operators | **1.8 added collision — ground/plane, "not external collision"** | **Source 2 well ahead** |
| Gameplay inputs | **64 control points**, parented, each carrying orientation, velocity, radius, density, duration, object identity | **4 dynamic inputs** (`@In0..@In3`) with an expression language (`sin`, `cos`, `rand`, `step`, `@GTime`, `@PTime`) | **Source 2 well ahead** |
| Particles bound to a skeleton | `HITBOX_INDEX` + hitbox-relative position | — | **Source 2 only** |
| Constraint solver | position-based, iterated, per-block | — | **Source 2 only** |
| Renders lights / text / physics | yes `[VDC]` | — | **Source 2 only** |
| Extensibility | write a C++ operator, declare its masks, it appears in the editor | fixed node/parameter set | **Source 2 only** |

`[inferred]` Read the right-hand column and the shape is clear. **Effekseer is a
mature authoring tool whose weak axis is exactly the axis Source 2 was built
along: knowing about the game.** Its dynamic parameters are four floats and an
expression evaluator; Source's control points are 64 parented transforms with
velocity, radius, density and duration, and an operator can read any of them.
Its collision is a ground plane; Source's is a real trace whose hit position and
normal become particle attributes the next operator reads in the same frame.

That is not a criticism of Effekseer — it is a different product. It is an
**effect player** that a game triggers, and the four-input interface is
proportionate to that. Source 2's system is a **simulation framework the game
drives continuously**, which is why it needs 64 slots and why it can be
extended in C++ at all.

**The two things to weigh honestly:**

- The Effekseer-ahead rows — node-graph materials, F-curves, NURBS paths,
  procedural models — are **authoring** wins, and authoring is the thing we
  cannot build (§11's opening argument). They are worth more to this project than
  they look on a feature table.
- The Source-2-ahead rows are almost all **gameplay coupling**, and this game's
  actual VFX list — muzzle flash, tracer, impact, grenade, smoke, death — needs
  very little of it. A tracer that ends at the right place needs a control point,
  not a collision system, because the gameplay layer already raycast (§12.3).

`[inferred]` So: **not on par, but plausibly on par with what we would need for
several years**, and the parts it lacks are the parts we would be writing
ourselves in any scenario, including the roll-our-own one.

**What it will not do for us:**

- **World queries are a plane, not a world.** Collision arrived in 1.8 and is
  documented as ground-plane only, *"not external collision"*. Nothing
  corresponds to `TRACE_P0`/`TRACE_HIT_NORMAL` (§2), so a particle cannot ask our
  tile grid whether it just hit a wall. Matters little for an XCOM-like; matters
  for the FPS and RTS futures `cromwell` is judged against
  ([`nav_architecture.md`](nav_architecture.md)'s framing).
- **Four dynamic inputs against sixty-four control points.** See the table. This
  is the gap that would bite first, and it bites as *"we can't drive that from
  gameplay"* rather than as a visual limitation.
- **It owns its own rendering.** Its own shaders, its own state, its own sorting,
  its own draw calls. Integrating means handing it the GL context and the
  view/projection matrices, and feeding it a depth texture if we want soft
  particles. It will not participate in our material system, our clustered
  lighting, or [`source2_rendering.md`](source2_rendering.md)'s passes. It draws
  in its own pass, and that pass gets **one profiler zone** per CLAUDE.md.
- **Frustum culling is admittedly imperfect** `[COMMUNITY]` — each effect has a
  different shape and size, so its bounds are conservative.
- **It simulates on the CPU.** No mention of GPU particles or compute appears
  anywhere in its release notes. That is not the disqualification it sounds like
  — see §13 — but it caps the count.
- **Its aesthetic centre of gravity is stylised/anime VFX.** That is where its
  users are and where its features point. Not disqualifying; worth knowing.

**Integration cost, concretely** `[inferred]`: create `EffekseerRendererGL` against
our existing context, a `Manager` per world, `Update(deltaFrames)` inside the
frame, `Draw()` in a dedicated pass after opaque with our depth bound, plus a
loader that routes its file I/O and texture loading through ours. A day or two to
a triangle on screen; a week to something that doesn't fight the renderer.

---

## 12. Recommendation

**Superseded — see §12A.** The original recommendation was *take Effekseer, write
nothing*, and it was correct for the brief as posed ("I'd rather not roll our
own"). It stops being correct the moment three specific requirements are stated,
because those three are precisely the axis §11.1 identifies as Effekseer's weak
one. The original is left in outline because it remains the right answer if the
requirements are ever relaxed:

> Effekseer for the effects that are pictures; nothing else, until an effect
> needs to read the world. One pass, one profiler zone, no wrapper.

## 12A. Recommendation, given the stated requirements

The requirements: **many gameplay inputs, real collision, particles bound to a
skeleton.** Against §11.1's table those are three of the four rows where Source 2
is "well ahead" or "only", so:

**No free library provides them, and the closest commercial one is
licence-blocked for modification.** PopcornFX's runtime SDK is the only
off-the-shelf thing designed for continuous engine coupling, and its own terms
say licensees cannot modify it (§10). Effekseer is MIT, so forking it to add
inputs and a collision callback is *legal* — but it means grafting a
continuous-simulation interface onto a player architecture and then maintaining
that delta against a ~4,400-commit upstream forever. That is not obviously
cheaper than writing the thing you actually want.

**So: build it, in `cromwell`, on Source 2's architecture.** What follows is why
that is a much smaller sentence here than it usually is.

### 12A.1 The three requirements are the cheap parts

This is the counter-intuitive bit and it is worth stating plainly. The expensive
parts of a particle system are the editor and the operator catalogue. **None of
the three requirements is either.**

| Requirement | Real cost here |
|---|---|
| **Many gameplay inputs** | An array of structs and some accessors. §5 lists the entire interface Valve arrived at — position, orientation, velocity, radius, density, duration, parent index, object identity — and the *design* is the valuable part, which is already published and already read. **Days.** Start at 16 slots, not 64; the number is a `constexpr`. |
| **Collision** | The query layer is the hard part of collision and this project *is* a spatial query engine — [`OcclusionGrid`](../src/game/world/OcclusionGrid.hpp), [`RayCaster`](../src/game/los/RayCaster.hpp), the occupancy grids. Two caveats below. **Days, once the query exists.** |
| **Skeleton binding** | **Blocked, and not by the particle system.** `Skeleton`, `Bone` and `ozz` return *zero* hits across `src/`. There is no animation system, so there are no bones to bind to. Once there are, the particle side is an attribute pair — bone index plus bone-local offset, transformed by that bone's matrix each frame (§2's `HITBOX_INDEX` / `HITBOX_RELATIVE_XYZ`). **~50 lines, after ozz lands.** |

The two collision caveats, both real and both shaping the design:

1. **`RayCaster` is not the caster this needs.** Its signature is
   `cast(ax, ay, aHeight, …)` with a `Hit`, a `RayRules` enum and a bound
   `UnitRoster` — a 2.5D line-of-sight caster answering *"can A see B"*, not
   *"where does this spark land"*. Particle collision wants a different query
   against the same grids.
2. **`cromwell` may not call into `game/`** — CLAUDE.md's one architectural rule,
   and `RayCaster` lives in `game/los/`. So the engine-side particle system takes
   a **collision callback interface** the game implements. That is the correct
   shape anyway: it keeps the engine liftable, and it is the same move
   [`console_porting.md`](console_porting.md) §1.1 argues for elsewhere.

### 12A.2 The editor is not a year — that estimate was for the wrong editor

§11 argued the decisive cost was authoring, "a few weeks for the simulation and a
year for the editor." That is true of an **artist-grade standalone tool** like
Effekseer's. It is not true of what this project needs, and the tree already
contains most of the answer:

- **`src/cromwell/ui/` is a 48-file draw-list widget kit** — sliders, steppers,
  buttons, labels, panels, gauges, spinners — architected so widgets emit
  vertices and know nothing about GL. **It is compiled** (CMakeLists.txt
  lines 115–124). *(Note: [`console_porting.md`](console_porting.md) §5 states
  these files are absent from the build. That was true when written and is now
  stale.)*
- **There is already a tabbed dev panel** — `game/render/dev/DevView`, F1, with
  the profiler tab as the working precedent for "a tab that shows live numbers
  and takes input."

A particle editor as **one more dev-panel tab** — live sliders over the active
system's parameters, save/load to a text format, hot reload — is *weeks*. For a
solo project with no artist to satisfy, that covers nearly everything
Effekseer's editor would have given, and it does it **inside the game, against
the real lighting and the real camera**, which a standalone tool cannot.

`[inferred]` This is the single biggest change to the calculus, and it was not
visible until the requirements forced the question.

### 12A.3 Build order

1. **Columns and masks first.** §9's items 1–3. Attributes as SoA, declared
   read/write masks, unused columns collapsing to constants, the debug assert.
   **This is the decision that is free now and expensive later** — retrofitting
   SoA onto a `std::vector<Particle>` is the change nobody makes.
2. **Control points.** §5's interface at 16 slots. Cheap, and it is the thing
   that makes every later effect drivable from gameplay rather than baked.
3. **The minimum function set**: one emitter, a handful of initializers, a
   handful of operators, accumulated forces, one billboard renderer. Plus the
   framework-level fade envelope (§4.2) and the poisoned RNG (§4.1) — both are
   cheaper to put in at the start than to retrofit into twenty operators.
4. **The dev-panel editor tab**, as soon as there are more than about five
   parameters worth tuning. Earlier than feels justified; the whole argument for
   building this instead of buying it rests on the loop being fast.
5. **Collision**, via a game-implemented callback, when there is a query worth
   calling.
6. **Skeleton binding**, after ozz-animation exists. Leave the two attribute
   slots defined and unused until then; do not design around bones that do not
   exist.
7. **Constraints and children/endcaps** only if something asks for them.

### 12A.4 What not to build, still

- **The operator catalogue.** §9 is the transferable part; Valve's catalogue is
  fifteen years of artist requests we do not have. Add operators one at a time,
  each because an effect needed it.
- **A node-graph material editor.** §11.1 marks this as a place Effekseer is
  ahead, and it stays ahead. Particle materials can be ordinary shaders in
  `assets/shaders/` for a very long time.
- **A GPU compute path.** §13. Not needed at this scale, and it is the one part
  that would fight every requirement in §12A — readback, events and world
  queries are exactly what GPU simulation gives up.
- **Anything for effects that are purely decorative and complicated to author.**
  If a set-piece explosion ever wants NURBS paths and procedural meshes,
  Effekseer is still MIT and can still be added *alongside* — which is what every
  engine in §13.2 did with its own two systems, for the same reason.

---

## 13. CPU or GPU — the question §1–12 dodged

Everything above describes a system simulated on the **CPU**, in SIMD, and it is
fair to ask why, when Unreal has run Niagara emitters as compute dispatches for
years. Tags here follow the directory's convention: `[EPIC]`, `[PAPER]`,
`[COMMUNITY]`, `[inferred]`.

### 13.1 Separate the two things first

**GPU simulation and fast particle rendering are not the same claim, and the
second does not depend on the first.** Every system in this note already renders
on the GPU — one instanced quad per particle, one draw call for the batch. A
CPU-simulated particle is not a CPU-*drawn* particle.

What moving the *simulation* to compute buys is **count, and CPU relief**. What
it does not buy is fill rate, and fill rate is what actually goes wrong. `[PAPER]`
GPU Gems 3 ch.23 puts it plainly: when particle effects fill the screen, overdraw
is "almost unbounded and frame rate problems are common, even in technically
accomplished triple-A titles." A million GPU-simulated particles that each cover
200 pixels will destroy a frame exactly as thoroughly as ten thousand CPU ones
would. **The count is the wrong axis.**

Which is why Source 2's budget knob is `m_flCullFillCost` (§6.1) — retirement
priced on projected screen area rather than on particle count. Valve measured the
bill they were actually being sent.

### 13.2 What everyone actually shipped

| Engine | CPU sim | GPU sim | Note |
|---|---|---|---|
| **Unreal / Niagara** | yes | yes | per-emitter switch `[EPIC]` |
| **Unity** | yes (Shuriken) | yes (VFX Graph) | **two separate products**, both maintained `[COMMUNITY]` |
| **Godot 4** | yes (`CPUParticles3D`) | yes (`GPUParticles3D`) | GPU path is a shader "process material" |
| **Source 2** | yes | — | no evidence of a GPU path; CS2's headline effect went elsewhere entirely (§13.4) |
| **Effekseer** | yes | — | **no mention of GPU particles or compute anywhere in its release notes** — this resolves the item §11 previously left unverified |

**The pattern is the answer to the question.** Nobody replaced the CPU system
with a GPU one. Everyone who built a GPU path **added it as a second system and
kept the first**, and the two are exposed to artists as a choice, not as an
upgrade. Unity did it so bluntly that they are two different editors.

### 13.3 Why the CPU one survives

The reasons are the same five each time, and four are structural rather than
performance:

1. **Readback.** GPU particle state lives on the GPU. Anything gameplay must
   *read* — did this spark reach the target, where do I put the decal, has the
   effect finished — costs a round-trip, which is a stall or a frame of latency.
   CLAUDE.md already states the general form of this: *the readback is usually
   the problem, not the dispatch.*
2. **Events die at the boundary.** `[EPIC]` Niagara's own docs: `SceneDepthCollision`
   "will not generate an event — only the CPU collision module does this," and
   community reports are consistent that adding collision events and gameplay
   logic to a GPU sim greys modules out. Particle-A-dies-spawns-system-B is three
   lines on a CPU and an indirect-dispatch-plus-append-buffer design on a GPU.
3. **The GPU has no world, only proxies.** `[EPIC]` Niagara's three collision
   options are the whole trade laid out: *depth buffer* — cheap, low accuracy,
   shapes not accurately portrayed, and **a particle that leaves the screen
   disappears immediately**; *global signed distance field*; or *hardware ray
   tracing*, which is experimental, asynchronous, and **one frame behind**. None
   of these is `TRACE_P0`/`TRACE_HIT_NORMAL` (§2) — a real trace against real
   collision geometry, available to the next operator in the same frame.
4. **Sorting.** Back-to-front alpha ordering is `GenerateSortedIndexList` over a
   16-byte SoA record on the CPU (§6); on the GPU it is a bitonic sort per frame.
   Perfectly doable, not free, and one more thing to write.
5. **Most effects are small.** A muzzle flash is 20 particles and a bullet impact
   is 60. `[COMMUNITY]` — the Unity practitioner framing is the honest one: *a
   compute shader isn't free; for chimney smoke the Particle System is far
   cheaper*, and if you are already GPU-bound the CPU path is the faster choice.
   A dispatch plus barrier costs more than the loop it replaced.

`[inferred]` So the GPU path is not "the modern way" — it is **the answer to one
specific question, which is "I want 100k+ of them and they need nothing from
gameplay":** rain, dust, sparks, snow, debris fields, crowd-scale ambient. That
is a real and worthwhile category, and it is also a *minority* of the effects in
any game that shoots at things.

### 13.4 Valve's own answer to the biggest particle problem they had

Worth noting because it cuts against the framing entirely: CS2's headline effect
is volumetric smoke, and Valve did **not** solve it by scaling particles up.
`[COMMUNITY]` The smoke grenade is a dynamic volumetric object that lights,
displaces around geometry, and is carved by gunfire and explosions — a voxel
volume, not a particle cloud.

`[inferred]` The general lesson, and it applies to us: **when a particle system
is being asked for something a particle system is bad at — a dense participating
medium — the fix is often a different representation rather than more
particles.** [`rdr2_atmospherics.md`](rdr2_atmospherics.md) is the same move for
fog and clouds, and [`voxel_terrain.md`](voxel_terrain.md) the same for volume
generally.

### 13.5 If the fill rate is the problem, fix the fill rate

All three standard fixes are **rendering-side and independent of where you
simulate**, which is the practical form of §13.1:

- **Render the expensive particles at half or quarter resolution** into an
  off-screen target and depth-aware-upsample. `[PAPER]` GPU Gems 3 ch.23 — "huge
  savings in overdraw at the expense of some image processing overhead."
- **Shrink the quads.** Trimmed/fitted billboards instead of full quads with
  mostly-empty corners, and steeper alpha falloff so the same look needs fewer
  covered pixels.
- **Bin the particles in compute and shade per tile**, which turns unbounded
  overdraw into bounded per-tile work. `[COMMUNITY]` AMD's *"Holy Smoke! Faster
  Particle Rendering using Direct Compute"* (Gareth Thomas, GDC 2014) is the
  reference.

Note what these have in common: they are all **do less work** in CLAUDE.md's
sense — fewer pixels touched — while moving the sim to compute is *do the same
work somewhere else*. The order of attack applies here as everywhere.

### 13.6 What this means for `cromwell`

`[inferred]`, and it does not change §12:

- The engine has GL 4.3 and compute (`cromwell/gpu/compute/`), so the GPU path is
  *available*. It is not *needed*: a tile tactics game has no million-particle
  problem, and the FPS/RTS futures the engine is judged against would want it for
  ambient weather and debris — which is precisely the "needs nothing from
  gameplay" category, i.e. the clean case.
- If it ever happens, it happens the way every engine above did it: **a second,
  narrower system alongside the first**, not a replacement, and not an
  abstraction over both. An abstraction whose two implementations differ on
  whether gameplay can read the result is an abstraction that lies.
- §9's ideas survive the move intact. Declared read/write masks become *which
  SSBO columns you allocate*; unused-attribute-becomes-a-constant becomes a
  shader define. The dataflow declaration is the portable part — which is more
  evidence it, and not the SIMD, was the real design.

---

## 14. What this note does not establish

- **No Source 2 operator implementation was read.** SDK 2013 ships the header
  only. Every `Operate()` body, in both engines, is unread.
- **No `.vpcf` was opened on this machine.** §7's category list is Source 2
  Viewer's published API, not a file this note parsed.
- **The per-frame order in §8 is inferred** from the interface. The two places it
  is most likely wrong are where the initial-attribute snapshot sits, and whether
  bounds are recomputed before or after children.
- **Effekseer has not been built or linked here.** §11's integration cost is an
  estimate from its API surface and its published integrations, not from a
  compile.
- **Effekseer's lack of GPU simulation is argued from absence** — its published
  release notes mention no GPU particle or compute feature. That is good evidence
  and not proof; nobody built it here to check.
- **No GDC deck in §13 was read in full.** The AMD tiled-particle talk and GPU
  Gems 3 ch.23 are cited from their abstracts and summaries, not worked through.
- **§11.1's comparison table is built from Effekseer's documentation, not from
  use.** Two rows are genuinely unresolved: its Culling and Levels-of-Detail
  pages exist but their configuration was not read, so "unclear" there means
  unclear. Feature *presence* is well sourced; feature *depth* is not, and a
  documented feature can still be shallow in practice.
- **Source 2 having no GPU particle path is an absence of evidence**, not
  evidence of absence — the engine is closed, and a compute path could exist
  without surfacing in the editor's vocabulary.
- **No licence in §10 was reviewed by anyone qualified to review licences.** The
  Defold and PopcornFX rows in particular state terms as published; if either
  becomes load-bearing, read the actual text.
- **Appendix A is a floor, not the catalogue.** See A.0 — it enumerates what a
  third-party reader implemented, and no parameter list, default or formula for
  any individual function was read.

---

# Appendix A — the feature catalogue, for replication

§1–9 describe the *machine*. This appendix enumerates the *content* — the named
functions Source 2 ships — because that is what a replication plan needs and what
a "we'll work it out later" plan silently omits. It is the answer to *how big is
this thing, really.*

## A.0 Sourcing, and the size of the caveat

New tag: `[VRF]` — **ValveResourceFormat / Source 2 Viewer**, an open-source
reader that parses `.vpcf` and *re-implements Source 2's particle functions* in
order to preview real Dota 2 and CS2 effects. The catalogue below is its
`Renderer/ParticleRenderer/` tree, read directly from the repository.

Two caveats, and the first is large:

1. **This is a floor, not a ceiling.** VRF implemented what it needed to make
   shipped effects look right. Valve's real catalogue is larger — VRF even keeps
   a `ParticleSupportInfo.cs` to track coverage. Treat every count below as *"at
   least this many."*
2. **File names are VRF's C# classes**, which mirror Valve's `.vpcf` class
   strings — `BasicMovement.cs` ↔ `C_OP_BasicMovement`, `RandomLifeTime.cs` ↔
   `C_INIT_RandomLifeTime`. `[inferred]`, but the naming is systematic and the
   editor-guide names line up (`Lifespan decay` ↔ `Decay`, `Movement basic` ↔
   `BasicMovement`).

Also used: `[VDC-GUIDE]` — a Dota 2 particle-editor guide mirrored on GitHub,
which is where the confirmed execution order and the artist-facing display names
come from.

**No parameter list, default value or formula for any individual function was
read.** This appendix says *what exists*, not *what it does in detail*.

## A.1 Execution order — confirmed

`[VDC-GUIDE]`, verbatim in substance:

```
pre-emission operators  ->  emitters  ->  initializers  ->  operators
                        ->  force generators  ->  constraints  ->  renderers
```

plus the ordering rule: **all functions are priority-based, and lower functions
in the stack override higher ones.** (§7 flags this as the trap it is.)

This confirms §8, which was reconstructed from the vtable before the source was
found.

## A.2 The provider system — copy this first

**The single most important thing in this appendix, and it is not a function.**

In Source 2, a numeric parameter is **not a float**. It is an `INumberProvider`,
and there are at least **14 kinds** `[VRF]`:

| Group | Providers |
|---|---|
| Literal | `LiteralNumberProvider` |
| Random | `RandomNumberProvider` — with a range, a `ParticleFloatRandomMode`, an optional `ParticleFloatBiasType`, and sign flipping |
| System state | `CollectionAgeNumberProvider`, `EndCapAgeNumberProvider`, `DetailLevelNumberProvider` (LOD0–LOD3) |
| Per particle | `ParticleAgeNumberProvider`, `ParticleAgeNormalizedNumberProvider` (0–1), `PerParticleNumberProvider` (any scalar field), `PerParticleVectorComponentNumberProvider`, `PerParticleSpeedNumberProvider`, `PerParticleCountNumberProvider` (unique ID), `PerParticleCountNormalizedNumberProvider` |
| Control points | `ControlPointComponentNumberProvider`, `ControlPointSpeedNumberProvider` |

And **every one of them then passes through an `AttributeMapping`** `[VRF]`, a
remap stage with seven modes:

| Mode | Does |
|---|---|
| `Direct` | pass through |
| `Mult` | scale by a constant |
| `Remap` | input range → output range |
| `RemapBiased` | remap with a bias curve |
| `Curve` | evaluate a piecewise curve at the input |
| `Notched` | one output inside a range, another outside |
| `Round` | nearest / floor / ceiling |

with the input handled as **clamped** or **looped**.

There are parallel `IVectorProvider` and `ITransformProvider` interfaces for
vector and transform parameters.

**Why this matters more than any operator.** It means "radius" can be *normalised
particle age → curve → output* with **no operator involved at all**. Fade-over-
life, scale-by-speed, colour-by-distance-from-a-control-point, vary-by-particle-
ID — none of these needs code; they are a parameter's *source* plus a remap.

`[inferred]` This is why Source 2's editor feels deep without a node graph, and
it is almost certainly **the highest value-per-line feature in the entire
system.** A dozen providers and seven remap modes replace scores of operators
that would otherwise each exist to do one wiring job. If only one idea from this
appendix gets built, build this one.

## A.3 The catalogue

Counts exclude each category's abstract base class.

### Emitters — 3

`ContinuousEmitter`, `InstantaneousEmitter`, `NoiseEmitter`

*Emission is the simplest part of the system. Three.*

### Initializers — 49

**Position:** `CreateWithinSphere`, `CreateWithinSphereTransform`,
`CreateWithinBox`, `CreateOnGrid`, `CreateAlongPath`, `CreateSequentialPath`,
`CreateSequentialPathV2`, `RingWave`, `PointList`, `PositionOffset`,
`PositionWarp`, `NormalOffset`, `NormalAlignToCP`

**Inheritance / relationships:** `CreateFromParentParticles`,
`InheritFromParentParticles`, `InheritVelocity`, `CreateFromCPs`,
`InitFromCPSnapshot`, `DistanceToCPInit`

**Velocity:** `VelocityRandom`, `VelocityRadialRandom`, `VelocityFromCP`,
`InitialVelocityNoise`

**Randomised scalars:** `RandomLifeTime`, `RandomRadius`, `RandomRotation`,
`RandomRotationSpeed`, `RandomAlpha`, `RandomColor`, `RandomScalar`,
`RandomVector`, `RandomVectorComponent`, `RandomTrailLength`, `RandomYawFlip`,
`RandomSequence`, `RandomSecondSequence`, `SequenceLifeTime`

**Generic set / arithmetic:** `InitFloat`, `InitVec`, `AddVectorToVector`,
`OffsetVectorToVector`, `GlobalScale`

**Remapping:** `RemapScalar`, `RemapScalarToVector`, `RemapSpeedToScalar`,
`RemapParticleCountToScalar`, `RemapTransformOrientationToRotationsInit`

**Noise:** `CreationNoise`, `AgeNoise`

### Operators — 62

**Lifetime:** `Decay`, `AlphaDecay`, `VelocityDecay`, `FadeAndKill`,
`RestartAfterDuration`

**Fades:** `FadeInSimple`, `FadeInRandom`, `FadeOutSimple`, `FadeOutRandom`,
`CGeneralRandomFade`

**Interpolation and ramps:** `LerpScalar`, `LerpVector`, `LerpToOtherAttribute`,
`InterpolateRadius`, `ColorInterpolate`, `ColorInterpolateRandom`,
`RampScalarLinear`, `RampScalarLinearSimple`, `OscillateScalar`,
`OscillateVector`

**Movement:** `BasicMovement`, `MaxVelocity`, `Spin`, `SpinUpdate`,
`MovementRotateParticleAroundAxis`, `PositionLock`, `NormalLock`, `DampenToCP`,
`LockToSavedSequentialPath`, `LockToSavedSequentialPathV2`,
`MaintainSequentialPath`

**Remapping — the largest family:** `RemapCPtoVector`,
`RemapControlPointDirectionToVector`, `RemapCrossProductOfTwoVectorsToVector`,
`RemapVelocityToVector`, `RemapSpeed`, `RemapScalarEndCap`,
`RemapParticleCountToScalar`, `RemapParticleCountOnScalarEndCap`,
`RemapTransformOrientationToRotations`

**Set / write:** `SetFloat`, `SetVec`, `SetToCP`, `SetCPtoVector`,
`SetFromCPSnapshot`, `SetAttributeToScalarExpression`

**Vector maths:** `RotateVector`, `NormalizeVector`, `ClampScalar`,
`QuantizeFloat`, `DistanceToCP`

**Culling:** `Cull`, `DistanceCull`, `PlaneCull`

**End cap:** `EndCapDecay`, `EndCapTimedDecay`, `EndCapTimedFreeze`,
`LerpEndCapScalar`, `LerpEndCapVector`, `ReinitializeScalarEndCap`

**Noise:** `Noise`, `VectorNoise`

### Force generators — 6

`AttractToControlPoint`, `TwistAroundAxis`, `RandomForce`, `PerParticleForce`,
`TurbulenceForce`, `CurlNoiseForce`

### Constraints — 2

`ConstrainDistance`, `RopeSpringConstraint`

### Pre-emission operators — 14

`SetControlPointPositions`, `SetSingleControlPointPosition`,
`SetRandomControlPointPosition`, `SetControlPointOrientation`,
`SetControlPointRotation`, `SetControlPointToVectorExpression`,
`SetParentControlPointsToChildCP`, `DistanceBetweenCPsToCP`, `RemapSpeedtoCP`,
`HSVShiftToCP`, `RampCPLinearRandom`, `ChooseRandomChildrenInGroup`,
`PlayEndCapWhenFinished`, `StopAfterDuration`

### Renderers — 6

`RenderSprites`, `RenderTrails`, `RenderCables`, `RenderStandardLight`,
`RenderOmni2Light`, `RenderSound`

**Total: ~142 functions**, and remember A.0 — that is the floor.

## A.4 What the shape of the catalogue tells you

Five readings, and they are worth more than the list:

1. **The physics is thin; the mapping is thick.** 62 operators and 49
   initializers against **6 forces and 2 constraints.** Overwhelmingly, this
   system is not simulating — it is *wiring values to other values*. Anyone
   planning a particle system by thinking about forces and collisions has
   mis-estimated where the work is by an order of magnitude.
2. **`Remap*` is the largest single family** — CP, direction, cross product,
   velocity, speed, particle count, transform orientation, all → scalar or
   vector. Combined with A.2's providers, this is Source 2's answer to "connect
   anything to anything" **without shipping a node graph.**
3. **Control points are read *and written*.** Every pre-emission operator writes
   CPs — set them, ramp them, put the distance between two of them into a third,
   HSV-shift into one. So control points are not just the gameplay input surface
   (§5); they are **the system's scratch variables and its intermediate value
   bus.** That reframes §5 substantially, and it is the second-most-valuable
   idea in this appendix.
4. **"End cap" is a first-class second lifetime phase**, not a flourish — six
   operators, a pre-emission operator, an enum, an age provider. When a system
   is told to stop, particles enter a *distinct* mode with its own decay, freeze,
   lerp and re-initialise behaviour. Every hand-rolled particle system bolts this
   on badly later; Source 2 has it in the type system.
5. **Renderers spawn non-particles** — `RenderStandardLight`, `RenderOmni2Light`,
   `RenderSound`, `RenderCables`. This confirms §6's reading: the particle system
   is Valve's general **"spawn a bunch of transient things"** mechanism. A muzzle
   flash's light and its sound are *particles*.

Plus one structural note: **snapshots**. `ParticleRenderer.Snapshots.cs`,
`InitFromCPSnapshot`, `SetFromCPSnapshot`, `CreateFromCPs` — a captured array of
points that particles can be spawned from or driven by. `[inferred]` This is how
effects follow arbitrary geometry (a model's bones, another system's particles)
without the particle system knowing what a model is.

## A.5 The supporting vocabulary

Enums that would have to exist `[VRF]`: `ParticleAttachment` (how a CP binds to
the world), `ParticleOrientation` (billboard modes), `ParticleBlendMode`,
`ParticleColorBlendType`, `ParticleTextureLayerBlendType`, `ParticleDetailLevel`
(LOD0–3), `ParticleEndCap`, `ParticleSetMethod`, `ParticleFloatRandomMode`,
`ParticleFloatBiasType`, `ParticleAnimationType`, `ParticleLightUnitChoiceList`,
`ParticleOmni2LightTypeChoiceList`, `SpriteCardTextureType`,
`SpriteCardTextureChannel`, `TextureRepetition`, `VectorExpression`.

`ParticleDetailLevel` is worth a second look — LOD is a **parameter provider**
(A.2's `DetailLevelNumberProvider`), so quality scaling is expressed as *any
parameter can read the current detail tier*, rather than as a separate LOD
system. `[inferred]` That is a notably cheap way to ship particle LOD.

## A.6 Minimum viable subset, ranked

For §12A.3's build order. **Tier 0 is roughly 15 functions and is a genuinely
useful particle system.**

**Tier 0 — the skeleton (build all of it):**

- Columns + declared masks (§3), control points (§5), the confirmed execution
  order (A.1)
- **The provider system + AttributeMapping** (A.2) — before any operator, because
  it deletes the need for many of them
- Emitters: `Continuous`, `Instantaneous`
- Initializers: `CreateWithinSphere`, `RandomLifeTime`, `RandomRadius`,
  `RandomRotation`, `RandomColor`, `RandomAlpha`, `VelocityRandom`
- Operators: `Decay`, `BasicMovement`, `FadeOutSimple`, `InterpolateRadius`,
  `Spin`
- Forces: one directional/gravity accumulator
- Renderers: `RenderSprites`

**Tier 1 — when effects start asking:** `CreateWithinBox`, `RingWave`,
`InheritVelocity`, `CreateFromParentParticles`; `ColorInterpolate`,
`FadeInSimple`, `MaxVelocity`, `PositionLock`, `Cull`/`DistanceCull`;
`AttractToControlPoint`, `TurbulenceForce`; `RenderTrails`; **end-cap decay**;
children with delay.

**Tier 2 — genre-driven, probably FPS/RTS era:** the `Remap*` family as needed
one at a time; `Noise`/`VectorNoise`/`CurlNoiseForce`; the sequential-path
operators; `RenderStandardLight`; `ConstrainDistance`; snapshots; detail-level
providers.

**Not planned:** `RopeSpringConstraint`, `RenderCables`, `RenderSound` (we have
an audio system for that), `RenderOmni2Light`, the full remap catalogue,
`SetAttributeToScalarExpression` (an embedded expression language).

## A.7 The functions a previewer cannot see — Valve's own registered catalogue

**A.3 has a hole, and it is exactly where the interesting requirements are.**
Read back through it: there is no world-trace operator, no collision constraint,
nothing that binds a particle to a bone or a model. Yet §2 shows Source 1 had
four `TRACE_*` attributes and two `HITBOX_*` attributes, so those functions must
exist.

`[inferred]` **The gap is structural, not accidental.** VRF is a *previewer* — it
opens a `.vpcf` and draws it with no world to trace against, no collision
geometry, no animating model and no game. The functions it cannot implement are
precisely the ones that talk to those things. **A catalogue derived from a viewer
is systematically blind to every gameplay-coupled feature**, which is worth
remembering the next time a tool's coverage is mistaken for an engine's.

New tag: `[VALVE-PET]` — read from **Valve's own particle editor binary**,
`GarrysMod/bin/tools/pet.dll` (Particle Editor Tool), cross-checked against
`garrysmod/bin/client.dll`. Both register the **same 166 distinct
`C_OP_*` / `C_INIT_*` names** — 57 initializers and 109 operators. This is
**Source 1**, so treat it as the ancestor catalogue rather than as Source 2's;
but the names that appear in both sources match exactly (`C_OP_BasicMovement`,
`C_INIT_RandomLifeTime`, `C_OP_RemapCPtoVector`…), which cross-validates A.3 and
makes it reasonable to expect Source 2 equivalents of what follows.

It also **confirms §4 from a second direction**: emitters and renderers are
registered as `C_OP_*` — `C_OP_ContinuousEmitter`, `C_OP_InstantaneousEmitter`,
`C_OP_NoiseEmitter`, `C_OP_RenderSprites`, `C_OP_RenderRope`,
`C_OP_RenderModels`, `C_OP_RenderPoints`, `C_OP_RenderSpritesTrail`,
`C_OP_RenderScreenVelocityRotate`. One class, seven entry points, exactly as the
header said.

### A.7.1 Collision and the world — 13 functions

| Function | What it is |
|---|---|
| **`C_OP_WorldTraceConstraint`** | **The one.** The real world trace, and the operator that fills §2's `TRACE_P0` / `TRACE_P1` / `TRACE_HIT_T` / `TRACE_HIT_NORMAL` columns for later operators to read. |
| **`C_OP_WorldCollideConstraint`** | collision as a position constraint — solved in the constraint phase (§4), so the velocity correction falls out of Verlet |
| `C_OP_MovementPlaceOnGround` | continuously keep particles on the ground |
| `C_INIT_PositionPlaceOnGround` | the spawn-time version |
| `C_INIT_CreateFromPlaneCache` | spawn from a cached set of planes rather than tracing per particle — **a derived cache, the same pattern as `OcclusionGrid`** |
| `C_OP_PlanarConstraint` | keep particles on one side of a plane |
| `C_OP_BoxConstraint` | confine to a box |
| `C_OP_ConstrainDistance` | the distance constraint (survives into Source 2) |
| `C_OP_ConstrainDistanceToPath` | distance constraint against a path |
| `C_OP_ForceBasedOnDistanceToPlane` | soft plane repulsion as a force rather than a constraint |
| `C_INIT_InitialRepulsionVelocity` | spawn moving *away* from nearby geometry |
| `C_OP_PlaneCull` | kill on crossing a plane |
| `C_INIT_RtEnvCull` | cull against a raytrace environment |

`[inferred]` Note the **three tiers of increasing cost** — cull (kill it), plane
or box constraint (cheap analytic shape), full world trace — and that Valve
shipped all three rather than only the accurate one. That is CLAUDE.md's *cull
cheaply before testing expensively* as a content-authoring choice: **the artist
picks the collision fidelity per effect**, which is the design worth copying,
more than any individual operator.

### A.7.2 Models and skeletons — 8 functions

| Function | What it is |
|---|---|
| **`C_OP_LockToBone`** | **the binding.** Particles follow a bone as it animates — §2's `HITBOX_RELATIVE_XYZ` made useful |
| **`C_INIT_CreateOnModel`** | spawn distributed over a model's surface |
| **`C_INIT_CreateInHierarchy`** | spawn within the bone hierarchy |
| `C_INIT_SetHitboxToModel` | assign each particle a hitbox — fills `HITBOX_INDEX` |
| `C_INIT_SetHitboxToClosest` | assign the nearest hitbox instead |
| `C_INIT_ModelCull` / `C_OP_ModelCull` | kill particles inside/outside a model, at spawn and continuously |
| `C_OP_RenderModels` | a mesh per particle |

`[inferred]` The decomposition is the useful part: **"particles on a character"
is not one feature, it is spawn-placement (`CreateOnModel`) + assignment
(`SetHitboxTo*`) + per-frame following (`LockToBone`)**, and they are separable.
Blood spray needs the first two; a burning effect needs all three; a
frost-covering effect needs the third with no velocity at all.

### A.7.3 The gameplay-coupling family, also invisible to a previewer

The one that reframes §5 hardest:

- **`C_OP_SetControlPointToImpactPoint`** — set a control point to a trace impact.
  §12A.3 argued a tracer ending at the right place needs a control point rather
  than particle collision; **Valve shipped an operator whose entire job is that**.
- `C_OP_SetControlPointToPlayer`, `C_OP_SetControlPointToCenter`,
  `C_OP_SetControlPointsToParticle` — CPs driven from the game, the effect's own
  bounds, or from particles themselves
- `C_OP_SetChildControlPoints`, `C_OP_SetPerChildControlPoint` — parents feeding
  children
- **`C_OP_LagCompensation`** — **particles are lag-compensated.** A networked
  effect is rewound the way hitboxes are in
  [`valve_networking.md`](valve_networking.md) §7.3. Nothing in a previewer could
  ever reveal this, and it is a real design signal: at Valve, particles are
  close enough to gameplay to need the netcode's help.
- `C_OP_ControlpointLight` — lighting driven from a control point

### A.7.4 Other functions only this source reveals

`C_OP_VelocityMatchingForce` (flocking), `C_OP_ParentVortices`,
`C_OP_DifferencePreviousParticle`, `C_OP_PerParticleEmitter` (every particle is
an emitter), `C_OP_MaintainEmitter` and `C_OP_DecayMaintainCount` (hold a
population rather than a rate), `C_OP_RampScalarSpline`,
`C_OP_OscillateScalarSimple`, `C_OP_Orient2DRelToCP` /
`C_OP_OrientTo2dDirection` / `C_OP_SpinYaw` (orientation control),
`C_INIT_LifespanFromVelocity`, `C_INIT_ColorLitPerParticle`,
`C_INIT_MoveBetweenPoints`, `C_INIT_RandomYaw`, `C_INIT_SequenceFromCP`,
`C_INIT_ChaoticAttractor` and `C_INIT_CreateInEpitrochoid` (a strange attractor
and a spirograph, both shipped).

### A.7.5 What this does to the plan

The three requirements in §12A decompose into **named, separable pieces**, which
is the point of having done this:

| Requirement | Functions to replicate | Depends on |
|---|---|---|
| Many gameplay inputs | the `SetControlPoint*` family, especially `SetControlPointToImpactPoint` | nothing — build now |
| Collision | `PlaneCull` → `PlanarConstraint`/`BoxConstraint` → `WorldCollideConstraint` → `WorldTraceConstraint`, **in that order of cost**, plus `CreateFromPlaneCache` as the derived-cache escape hatch | a game-supplied trace callback (§12A.1) |
| Skeleton binding | `CreateOnModel` + `SetHitboxToModel` + `LockToBone` | **ozz-animation existing first** |

`[inferred]` And the sequencing note that falls out: **cheap collision is
available immediately.** `PlaneCull` and `PlanarConstraint` need no trace at all
— a floor plane and a kill plane cover a surprising share of what a tile game's
effects want, and neither waits on the callback or the query design.

## A.8 What Appendix A does not establish

- **Every count is a floor** (A.0). VRF implements a subset.
- **No function's parameters, defaults or maths were read** — only that it
  exists and what its name implies. This applies to A.7 as much as A.3: the
  annotations there are read from the *names*, and a name is a strong hint, not a
  specification.
- **The `C_OP_*` ↔ VRF class-name mapping is inferred**, though systematically —
  and A.7 partially confirms it, since the two independently-obtained lists agree
  wherever they overlap.
- **A.7 is Source 1, not Source 2.** 166 names from Valve's Source 1 particle
  editor. Source 2 demonstrably has functions Source 1 lacks
  (`CreateWithinSphereTransform`, `CreateOnGrid`, `PointList`, `CurlNoiseForce`,
  `RenderOmni2Light`, the whole end-cap family), so **the honest picture is the
  union of A.3 and A.7, and even that is a floor.** Whether Source 2 kept
  `WorldTraceConstraint` and `LockToBone` under those names is **not
  established** — only that Source 1 had them and that the taxonomy around them
  survived (§7).
- **No `.vpcf` was parsed here**, and no Source 2 particle binary was found on
  this machine — s&box ships the Source 2 engine DLLs but **not** the particle
  library (checked: no `C_OP_` strings in any binary in its `bin/win64`), and
  the local CS2 and Dota 2 installs are Steam stubs of ~100–500 KB with no
  binaries at all. A.7's binaries are Garry's Mod's, which is a **modified**
  Source 1 — Facepunch could in principle have added or removed operators,
  though nothing in the list looks non-Valve.
- **Tier assignments in A.6 are judgement**, not Valve's, and reflect this
  project's genres rather than any general truth.
