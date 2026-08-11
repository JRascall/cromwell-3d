# Rainbow Six Siege — reference notes

How Ubisoft Montreal built a competitive 5v5 shooter in which **the level is not
level data** — every wall is cut at runtime, by arbitrary polygons, on all
platforms, deterministically, at 60 fps.

This is the best-documented real-time destruction in any shipped game, because
Ubisoft gave **two** GDC 2016 talks about it — one on the destruction system,
one on the renderer built around it — and both decks are public with their
numbers intact. That makes this note unusually well sourced for the study
folder: most of what follows is read from the slides themselves rather than from
interviews.

The short answer to the question that prompted this note, up front, because it
is the single most surprising fact in the deck:

> **The wall geometry is genuinely cut at runtime by a procedural algorithm.
> The flying debris is not.** Debris is pre-made, instanced, box-collided and
> aggressively recycled. Siege spends its destruction budget on the *hole* —
> which is gameplay — and spends almost nothing on the *fragments*, which are
> decoration. §2.7.

> **Read alongside:** [`re_engine_rendering.md`](../rendering/re_engine_rendering.md) — Siege's
> shadow system is the same cache-and-repair architecture, arrived at
> independently and under harder constraints.
> [`space_engineers.md`](../space/space_engineers.md) for the other destructible-world
> game here, and §7 reads them against each other.
> [`source2_rendering.md`](../valve/source2_rendering.md) §13 for clustered lighting.
> [`elite_dangerous.md`](../space/elite_dangerous.md) §2.4 for the other treatment of
> determinism as an architecture.

---

## Sourcing

| tag | source | strength |
|---|---|---|
| **[GDC-D]** | **"The Art of Destruction in Rainbow Six: Siege"** — Julien L'Heureux, Technical Lead / Physics Programmer, Ubisoft Montreal, GDC 2016. **The primary source for §2, §3.** | **Very strong.** A working engineer's deck with budgets, benchmarks and named algorithms. |
| **[GDC-R]** | **"Rendering 'Rainbow Six | Siege'"** — Jalal El Mansouri, Technical Architect, Ubisoft Montreal, GDC 2016. **The primary source for §4.** | **Very strong.** Frame budgets, draw-call counts, buffer formats, millisecond costs. |
| **[AUDIO]** | Ubisoft Montreal's audio team, *Game Design Deep Dive: Dynamic audio in destructible levels* | Strong for mechanism, no costs. |
| **[UBI]** | Ubisoft's own Siege dev blogs — limb penetration, explosions and shrapnel | Strong. Written for players but technically specific. |
| **[COMMUNITY]** | Player measurement, wiki documentation of weapon behaviour | Weak; used only for gameplay numbers. |
| **[inferred]** | Our reading. | — |

**How the decks were read.** Both GDC PDFs were downloaded and their text
extracted directly, so the bullet text and the figures below are the slides'
own. Two caveats: slide text is terse and occasionally runs words together, so
**phrasing is transcribed but sentence structure is sometimes reconstructed**;
and a few figures come from slide *tables*, where column alignment had to be
inferred — those are flagged where it matters. L'Heureux's own annotation on the
benchmark table, quoted verbatim, is **"to take with a heap of salt."**

---

## 1. The one thing that matters most

**In Siege, destruction is not an effect layered on the world — it *is* the
world's authoritative state, and every other system is a consumer of it that had
to be rewritten to cope.**

**[GDC-D]** The deck's own list of what destruction forced open is the fastest
way to see the scale of the commitment:

| System | What destruction did to it |
|---|---|
| **AI navigation** | navlink update on trapdoors and breachable walls |
| **AI visibility** | must see through *partially* broken walls |
| **Sound propagation** | "destruction changes the acoustic of the environment drastically" — §6 |
| **Rendering** | "static lighting and shadows are severely limited"; "less occluders, can see more objects" |
| **Collision** | surfaces become concave the moment they are cut — §2.8 |
| **Gameplay** | "need to know when an object is broken" — and the deck calls this "an ambiguous concept" |
| **Networking** | every cut must be deterministic and replicated — §3 |

**[inferred]** That table is the actual lesson, and it generalises to any project
whose world is mutable at runtime — including this one. **The cost of
destructible geometry is not in the destruction system. It is in every system
that had assumed geometry was static**, and the bill arrives in navigation,
visibility, audio, lighting and networking simultaneously. L'Heureux's closing
takeaway says as much directly: destruction "must be tackled early on" because
of "production & mentalities inertia", and it "needs a clear production buy-in"
since "a lot of teams need to contribute and adapt."

**[GDC-D]** Note also who owned it: RealBlast was a **small, mostly-programmer
team inside Ubisoft's central Technology Group, dedicated to destruction for
about five years**, independent of any production, whose stated domains were
**"destruction, navmesh"** — and which shipped destruction first on *Assassin's
Creed IV: Black Flag* before Siege. **[inferred]** Those two domains sitting in
one team is not a coincidence: they are the two systems that must both be
regenerated when geometry changes, and §1's table is why.

---

## 2. Destruction — what it actually is

### 2.1 Procedural, and the deck defines the word

**[GDC-D]** The definition, verbatim from the slides:

> **Procedural destruction:** "A change in the state of an object generated at
> runtime, where the **outcome is unique**."
>
> "In opposition to **pre-fragmented** destruction, which is **pre-determined
> and has a fixed outcome**."

**[inferred]** Worth being precise about, because the two get conflated
constantly. Pre-fragmented means an artist broke the mesh into pieces offline
and the runtime picks which pieces detach — the geometry is authored, the
selection is dynamic. Procedural means **the geometry itself did not exist
before the shot was fired.** Siege ships both, and §2.9 shows the cost gap
between them is the whole reason the design is shaped the way it is.

### 2.2 The destruction model — a graph of connections, not a pile of pieces

**[GDC-D]** Three slides define the representation, and it is more interesting
than "a mesh that breaks":

1. **"Objects are separated into different parts based on their physical
   material."** The deck's own note: this "should drive modeling for assets to
   be more readily destruction compatible" — i.e. **the material boundary is the
   fracture boundary**, and artists have to model to that.
2. **"Hierarchical decomposition, based on fragmentation"** — the deck flags it
   as "efficient for rendering & physics", **[inferred]** because a hierarchy
   lets an untouched subtree stay one draw call and one collision shape.
3. **"Connection-based leaf graph"** — "the game interacts with connections, the
   leaf graph manages state."

**[inferred] That third point is the design.** The authoritative structure is
not the geometry — it is a **graph whose edges are structural connections between
leaf fragments.** Damage removes connections; the graph decides what is now
detached. Geometry is downstream of that. It is the same separation this project
makes between authoritative `Tile` state and everything derived from it, and it
is why the system can answer "is this broken?" without inspecting meshes.

**[GDC-D]** And procedural cutting is layered *into* that model rather than
replacing it: "leaf fragments can be flagged as **procedural**, depending on
topology", after which "visual and collision can change" and the fragment "can
create new child fragments."

**[inferred]** So a wall is a pre-authored hierarchy of material layers whose
*leaves* are allowed to be cut arbitrarily. The pre-fragmented structure carries
the cheap, common case; procedural cutting is switched on only for the leaves
whose topology supports it. **That is a fast-path/slow-path split of exactly the
kind `CLAUDE.md` describes**, chosen by a static property (topology) rather than
by a runtime guess.

### 2.3 Surface procedural destruction — the algorithm

**[GDC-D]** This is the part built specifically for Siege, and the deck names
every step:

> "Developed **exclusively for Rainbow 6: Siege**. Use **arbitrary cutting
> polygons to cut a planar surface**. Great flexibility & simplicity to
> implement cutters. **General 2D polygonal technique. Robust, fast, simple.**"

The pipeline:

| Step | What happens |
|---|---|
| **1. Project 3D → 2D** | the surface is planar, so flatten it into its own plane and work in 2D |
| **2. Generate a cut pattern** | shape depends on "impact position in local space of object" plus "combination of inputs and material parameters" |
| **3. Polygon intersection** | surface polygons against cutter polygons — the deck cites **Weiler–Atherton polygon clipping** as the simple example |
| **4. Triangulation** | **ear clipping** — "robust, can handle multiple holes" |
| **5. Extrude back to 3D** | "extruded 3D mesh from 2D surface" |

**[inferred] The whole trick is step 1, and it is worth stating plainly: Siege
does not do 3D CSG.** General 3D boolean mesh operations are slow, numerically
fragile and produce degenerate geometry. By restricting destruction to **planar
surfaces** — which is what a wall, a floor, a barricade and a hatch all are —
the problem collapses to 2D polygon clipping, which is a solved, robust,
sixty-year-old body of work with known-good algorithms. The deck's own adjectives
are "robust, fast, simple", and they are earned by the restriction, not by the
implementation.

**[inferred]** That restriction is exactly the one this project already lives
under. Storey geometry here is planar surfaces on a lattice
(`SurfaceFacing`, `StoreyGeometryEmitter`, `SurfaceBuffers`). **If cutting holes
in walls ever becomes a feature, this is the algorithm** — and the reason to
record it now is that it changes what the surface representation should be
(polygons in surface-local 2D, not triangles in world space), which is the
expensive thing to change later.

### 2.4 Cutters — a small taxonomy that covers everything

**[GDC-D]** The cutter is the polygon set that gets clipped against the surface,
and the deck classifies them by what they define:

| Class | Defines | Examples |
|---|---|---|
| **Perimeter only** | the outer boundary of the hole | random ellipse, spline |
| **Inner fragments only** | how the region subdivides | **Voronoi** |
| **Both** | boundary *and* internal breakup | **glass**, **texture** |

**[GDC-D]** The **texture cutter** is the clever one: a "continuous and tileable
motif mapped in UV", where "the pattern is generated in UV space, then
transformed to 2D surface space", and "artists use a tool to generate vector
coordinates."

**[inferred]** Read that as: **the fracture pattern is authored content, drawn as
vectors in texture space, tiling.** So a brick wall breaks along mortar lines and
a plaster wall breaks in irregular plates, and neither needs a bespoke
algorithm — an artist drew the pattern and the cutter samples it. It is the same
conclusion [`elite_dangerous.md`](../space/elite_dangerous.md) §3.6 reaches from a
completely different direction: **a mature procedural system ends up selecting
and transforming authored content, not generating from pure noise.** Voronoi
gives you glass; only an artist gives you brickwork.

### 2.5 Decorations, not decals — geometry that survives being cut

**[GDC-D]** A subtle piece that solves a problem specific to cuttable surfaces:

> "Traditionally done as decals on the GPU side... **Decorations output actual
> geometry**: + more flexible especially for transparency, **+ preserved through
> destruction and child surfaces**, − more costly for CPU/rendering/memory,
> + can be applied offline by artists."

Two kinds:

- **Cut decorations** — "planar meshes that stick on the surface", which are
  **cut along with the surface**.
- **Feature-bound decorations** — "attached on geometric features, on edges and
  vertices", **not cut**; they "just disappear when the feature is gone", and
  they "can protrude from the surface".

**[inferred] The insight is that a decal is a *rendering* trick and a cuttable
wall needs a *geometry* answer.** A projected decal has no idea the wall now has
a hole through it; it will project across the void. Making the decoration real
geometry in the surface's own 2D space means it goes through the same clipper as
everything else, for free. And the feature-bound variant is the cheap
counterpart: skirting board attached to an edge does not need clipping at all,
it just needs to know when its edge stopped existing.

**[inferred]** The trade the deck states honestly — more CPU, more memory, more
artist work — is the price of correctness under cutting, and it is only worth
paying on surfaces that actually get cut.

### 2.6 Debris — the direct answer, and it is the opposite of what it looks like

**[GDC-D]** Verbatim, and this is the slide that answers the question:

> **"Debris in R6:S — Performance choice:**
> **No procedurally cut dynamic fragments**
> **Well-placed replacements**
> **Instanced**
> **Recycled aggressively"**

and the accompanying tricks:

> "**Vaporize fragments on explosion.** Simple collision primitives — **always
> boxes**. HavokFX."

**[inferred] So the split is:**

| | Approach | Why |
|---|---|---|
| **The hole in the wall** | **genuinely procedural**, cut at runtime, unique every time | it is *gameplay* — sight lines, bullet paths, movement |
| **The flying debris** | **pre-made instanced props**, boxes for collision, pooled and recycled, deleted en masse on explosions | it is *decoration* — nobody plays around a specific chunk of plaster |

This is a very clean instance of a general rule, and it is worth naming:
**spend simulation on what the player makes decisions about, and spend art on
everything else.** The debris looks like the expensive part and is the cheap
part. The hole looks like a hole and is the expensive part.

**[inferred]** Three supporting details make it work:

- **"Well-placed replacements"** — the pre-made pieces are chosen and positioned
  to *match* the cut that was actually made, so the fake reads as the
  consequence of the real. That placement logic is where the quality lives.
- **"Always boxes"** for collision. A debris chunk's collision does not need to
  match its silhouette; it needs to bounce plausibly and stop. A box does that at
  a fraction of the cost, and it removes the concave-shape problem entirely.
- **"Vaporize fragments on explosion"** is an admission worth respecting: the
  worst case for debris count is an explosion, which is also the moment the
  screen is full of effects and nobody can see the floor. So delete them all.

**[inferred]** Note what this means for the frequently-asked version of the
question — *"is it real-time destruction or pre-made destructibles?"* The answer
is **both, split along the gameplay/decoration line**, and that split is the
design rather than a compromise within it.

### 2.7 Collision after a cut — the concavity problem

**[GDC-D]** The problem, stated on the slide: after destruction the surface is
"very likely concave", which physics engines do not accept directly.

The solution:

> "**Collection of 2D convex shapes from a simplified version of the surface** —
> remove small holes, reduce tessellation of holes."
>
> Hint: "we use the **actual surface geometry for hi-resolution collision**, e.g.
> shooting."
>
> And on the physics layer: "feed **planes** instead of convex is a win."

**[inferred] Three separate ideas, all good:**

1. **Two collision representations at different fidelities**, chosen by
   consumer. Character movement and physics get a *simplified* convex
   decomposition — small holes removed, hole edges decimated, because a body
   cannot fit through a bullet hole anyway. Shooting gets the *exact* surface
   geometry, because whether a bullet passes through a specific hole is a
   gameplay-critical question.
2. **The simplification is semantically chosen, not geometrically.** "Remove
   small holes" is not mesh decimation — it is a statement about what a body can
   traverse. That is the right way to build a derived cache: the fast
   representation may only drop things that provably cannot matter to its
   consumer, which is `CLAUDE.md`'s escape-hatch rule 1 exactly.
3. **Planes beat convex hulls** where the surface is planar, because a plane is
   an analytic primitive and a hull is a search.

### 2.8 Budgets and benchmarks

**[GDC-D]** The requirements, as stated:

> "60 fps for smooth gameplay. **Destruction should not deteriorate framerate.**
> Determinism: every player should experience the game the same way."

The budgets:

| Resource | Budget |
|---|---|
| **CPU, pre-fragmented** | "not a risk — we had shipped AC:IV before" |
| **CPU, procedural** | **"high risk"** — "roughly **6 ms for a wall**" (2 procedural layers + pre-fragmented) |
| **GPU memory** | **25 MB** |
| **RAM** | **200 MB data + 150 MB engine** |

And the measured costs — the deck gives two figures per platform, **[inferred]**
read as typical and worst case:

| Operation | PC | PS4 | XB1 |
|---|---|---|---|
| **Single bullet hole** | 0.33 / 0.36 ms | 1.1 / 1.4 ms | 1.1 / 1.5 ms |
| **Single explosion** (drywall layer) | 1.4 / 1.9 ms | 2.8 / 4.0 ms | 3.6 / 4.9 ms |
| **Single explosion** (2 drywall + 2 wood layers) | **8.1 / 10.3 ms** | **19.5 / 23.5 ms** | **19 / 23 ms** |

**[GDC-D]** L'Heureux's own caveat on this table: **"to take with a heap of
salt."**

**[inferred] These numbers are the most valuable thing in the deck and they are
sobering.** A frame at 60 fps is 16.6 ms. **A single explosion through a
four-layer wall costs more than a frame on console** — 19–23 ms — and roughly
half a frame on the PC of the day. That is why §2.9 exists: the work cannot be
made to fit in a frame, so it is made not to *need* to fit in a frame.

Note also the platform spread: **PC is 3× faster than console on a bullet hole
and 2.4× on the heavy case.** Destruction is single-threaded-critical-path work
dominated by branchy geometry code, which is exactly where a Jaguar core suffered
most.

### 2.9 Making it fit — async, time slicing, and a debugging rule

**[GDC-D]** Four mechanisms, in the order the deck presents them:

**Multithreading.** "Multithreading at the object-basis is trivial — each
independent sub-state is MT in the simulation", including procedural
destruction. The warning: "watch out for race conditions when creating new
data."

**Asynchronicity.** "Made destruction a manageable risk. Little impact on
framerate and game feel." The costs, stated honestly: "introduces **delay
between game perception and actual destruction state**", and "creates the need
for an **event forwarding mechanism**." **[GDC-D]** With a specific caveat on the
scheduler: "you might not want to run along with physics."

**[GDC-D] And a trick worth stealing:** async enables **"pre-destruction —
perform destruction in advance, synchronize with end of animation."** **[inferred]**
When a breach charge has a two-second placement animation, you know two seconds
ahead exactly what hole is about to appear. So cut it during the animation and
reveal it on the last frame. **The animation is not hiding a load — it is
*budgeting* one**, and it is free because the fiction already required a wind-up.
This is the same idea as [`elite_dangerous.md`](../space/elite_dangerous.md) §6.2's
supercruise slowdown: put the expensive work where the game already wanted the
player to wait.

**Time slicing.** "Asynchronicity not sufficient to be engine-friendly — **what
about a 60 ms spike?**" So: "functions are split into steps; not finished?
Rescheduled." With the implementation note that "state variables can double up as
working data" and that there is "no easy solution in C++" — the deck shows the
`START_STEP_FUNCTION` / `STEP_FUNCTION` / `END_STEP_FUNCTION` macro trio that
compiles a function into a resumable `switch` over a state variable.

**[inferred]** That is a hand-rolled coroutine, five years before C++20 gave the
language one. The relevant point is not the macro — it is that **resumability is
a shape, and it has to be designed in.** [`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md)
§8.2 makes the same argument for sliced pathfinding, from Detour rather than from
Havok.

**And the rule that is worth more than any of it. [GDC-D]:**

> "Multi-threaded, time-sliced code is **very hard to follow and debug**. Make
> sure **you can disable it easily**. If possible, **make it single-threaded as
> well!** While this may hide some problems, it will make debugging **tractable
> 95% of the time**."

**[inferred]** A shipped team's considered position: keep a synchronous,
single-threaded path through the same code, permanently, as a debugging tool.
It costs a compile-time switch and it buys you the ability to ask "is this a
logic bug or a concurrency bug?" in one run.

**[GDC-D]** The optimisation section adds three standing rules — "measure
performance & optimize" (their telemetry came from "testers, game sessions,
automatic tests"), "limit degenerate cases" (police the data, disable features
no longer needed, "implement features to help bound complexity"), and "keep
runtime allocations low", listing hybrid heap/stack arrays, reusable working-data
structures, pools of short-lived objects, and in-place algorithms — with the
reason: allocation "often locks in multithreaded environments."

---

## 3. Determinism and networking — the hardest part

**[GDC-D]** The framing: destruction is a gameplay feature, therefore it "must be
deterministic **and** replicated", and the policy is to **"minimise bandwidth
usage over CPU usage"** — send **events (messages)**, not **states (meshes)**.
**[GDC-D]** With a bonus: "easy to do JIP [join-in-progress] with events" —
**[inferred]** a late joiner replays the event log rather than downloading the
current geometry.

### 3.1 The contract

**[GDC-D]** Stated as a contract between the game and the destruction library:

> "We expect to be provided: **the exact same inputs — in the same order**."

**Same order** turned out to be the easy half: "on an object basis, guaranteed by
the network layer", which the deck calls "definitely the easiest solution by
far."

**Same inputs** was not:

> "Not trivial: **race conditions between gameplay states**. **Network data
> compression even locally.** Need **symmetrical compression**."

**[inferred] That middle item is the trap and it is worth spelling out.** An
impact position sent over the network is quantised by the compressor. The remote
client therefore cuts the wall using the *quantised* position; the local client,
if it uses its own full-precision value, cuts using a *different* one — and two
slightly different cut polygons diverge into two different holes. The fix is
**symmetrical compression: compress and decompress the value locally too, so
both ends feed the identical quantised input to the generator.** This is a
completely general hazard for any deterministic system behind a lossy transport,
and it is invisible until two clients disagree.

### 3.2 Randomness

**[GDC-D]**

> "Seed a RNG based on some input value. On R6:S: **based on impact position.**
> Assumes perfect replication of inputs. Store the RNG on **TLS** for
> ease-of-use. **Caveat: time-slicing.**"

**[inferred]** Same technique as ED's noise-from-position
([`elite_dangerous.md`](../space/elite_dangerous.md) §3.1): **randomness derived from the
event rather than from a stream**, so it needs no synchronisation and no
sequencing. The time-slicing caveat is the sharp edge — a thread-local RNG plus a
function that yields mid-way and resumes on a different thread is a determinism
bug waiting to happen, and the deck flags it without claiming to have solved it
elegantly.

### 3.3 Instant feedback versus determinism

**[GDC-D]** The genuine conflict:

> "Instant feedback for shooting in R6:S is needed. Latency over the internet ≫
> latency on LAN. Breaks contract (ordering) → breaks determinism? Initially,
> compromised replication because of the **self-destruction** feature."

**[GDC-D]** And the honest verdict on their solution:

> "**Not a perfect solution.** Originator might not end up with exactly the same
> state. In practice, the difference is **minimal and unlikely to cause
> issues**."

**[inferred]** So the shooter's client cuts the wall immediately, out of order,
and accepts a small permanent divergence from the authoritative result. That is a
deliberate, measured breach of the determinism contract in exchange for
responsiveness, and it is the correct call for a shooter — but note that it is
*stated as a known defect*, not dressed up.

**[GDC-D] The alternative they considered and rejected — rollback:**

> "Each client keeps track of locally applied events; **reverts and re-applies**
> when receiving other events from the host. Pros/cons: **super robust and
> deterministic**; stack of events to revert is **not really bounded**
> (susceptible to latency); **each revert step is memory-intensive (full surface
> backup)**."

**[inferred]** Rollback is the textbook answer and the deck explains exactly why
it does not survive contact with this problem: the rollback unit is not a few
bytes of player state, it is **an entire cut surface's geometry**, so a snapshot
per event is enormous and the queue length is a function of the worst player's
ping. Rollback works for fighting games because their state is small. Geometry is
not small.

### 3.4 Debris is deliberately outside the contract

**[GDC-D]**

> "Physics replication is hard. Destruction is asynchronous. Characters impact
> dynamic objects. **All dynamic objects and debris are always small → ignored by
> gameplay → destruction on dynamic objects is not replicated.**"

**[inferred] This is §2.6's split enforced at the network layer, and it is the
cleanest expression of the whole design:** debris was made small *so that* it
could be declared gameplay-irrelevant, *so that* it need not be replicated, *so
that* it can be simulated locally, cheaply, differently on every machine. Each
player sees different rubble and it does not matter — by construction.

**[COMMUNITY]** The limit of this shows in practice: players have long reported
that client-side debris can produce **different sight lines for different
players**, and Ubisoft acknowledged the problem and listed "consistency of
barricade destruction for all players" as a fix objective. **[inferred]** Which
is the same argument as §2.6 arriving at its boundary — the moment a piece of
debris is big enough to hide behind or see past, it stops being decoration and
the "ignored by gameplay" premise fails.

---

## 4. Rendering — 60 fps with a world that will not hold still

**[GDC-R]** El Mansouri's deck. The framing that matters: Siege is "based on the
first iteration of a new current-generation-only rendering engine", and
"with massively and procedurally destructible levels, it was important to invest
in techniques that allow for better scaling on both CPU and GPU."

### 4.1 The budgets

**[GDC-R]**

| | Budget / measured |
|---|---|
| **GPU** | **14 ms average** on non-combat situations |
| **CPU** | **max 38 ms linear time** on consoles; **~10 ms average on the critical path** |
| — geometry | **~5 ms** |
| — lighting (incl. SSR) | **~5 ms** |
| — post / full-screen | **~4 ms** |
| — opaque pass, CPU | **max 4 ms linear** |

**[inferred]** Note "linear time" versus "critical path" — the deck distinguishes
total CPU work summed across cores from the dependency chain that actually bounds
the frame, and says "all passes and tasks are able to fork and join to minimise
critical path." 38 ms of work in a 16.6 ms frame is only possible if it
parallelises, and the 10 ms critical path is the number that had to fit.

### 4.2 Material-based draw calls — a renderer shaped by destruction

**[GDC-R]** The central architectural decision, and it exists *because* of
destruction:

> "Materials define destruction properties. **Debris share material.**"
>
> "In need of **granularity in culling** to keep up with destruction."

**[inferred]** Destruction generates unbounded *unique geometry* but almost no
new *materials* — a shattered drywall is a thousand new triangle sets that are
all still drywall. So the renderer is organised around the invariant rather than
the variable: **batch by material and state, and let geometry be arbitrary.**

**[GDC-R]** The implementation:

- **Unified buffers** — one vertex buffer, one index buffer, one constant
  buffer, giving "complete layout control and custom packing."
- A **draw call is defined by** shared shader, shared non-unified resources
  (textures), and shared render state; everything sharing those is batched.
- **Each submesh instance maps to three batches: Normal, Shadow, Visibility**,
  with the batch type used "to mask non-necessary data."
- Submesh instances get globally unique indices, and the shader walks a
  **multi-level indirection** — submesh instance index → mesh index → entity
  index → matrices and inverse scale.
- Per pass, submesh instance indices are gathered into a dynamic buffer in a
  **multithreaded job costing 1.5 ms linear**.
- Draws are issued as **`MultiDrawIndexedIndirect`**, with culling flags and
  buffer offsets carried per entry, and a culling compute shader writing surviving
  instance indices into a per-instance buffer.
- **`ReadFirstLane`** is used in the pixel shader when loading unified-constant-
  buffer values, so the compiler can treat them as scalar rather than vector.

### 4.3 Three levels of culling, and the numbers

**[GDC-R]**

| Level | Granularity | Tests |
|---|---|---|
| **1** | submesh **instance** | screen-space size, distance, frustum, occlusion |
| **2** | submesh **chunk** | screen-space size, frustum, **orientation**, occlusion |
| **3** | **triangle** | triangle normal culling |

**[GDC-R]** Hi-Z for occlusion is generated by rendering the **400 best
occluders** to a depth buffer.

**[GDC-R]** The results table (read from the slide; column alignment inferred):

> **10,537** unbatched draw calls total → **412** batched for visibility,
> **64** for shadows, at **73% culling efficiency.**

**[inferred]** A ~25× reduction in draw calls, in a game where the geometry
count is unbounded by design. And note that culling had to go all the way down to
*triangles* — which is unusual, and is a direct consequence of destruction: a
cut wall is one submesh whose triangles are scattered around a hole, so
instance-level culling alone tells you almost nothing.

### 4.4 Checkerboard rendering

**[GDC-R]** Siege is where this technique entered the industry's vocabulary, and
the deck is candid that it started somewhere else: "base idea came about to solve
**aliasing** issues. Experimented on a series of images to first test quality —
for most images **PSNR was better using a checkerboard pattern**, [and] visually
the results were more pleasing too."

The implementation:

- To target **1920×1080**, render geometry and lighting to a **960×1080** target
  **with MSAA 2×**, giving "half the samples of the full resolution image" laid
  out on the D3D MSAA 2× standard pattern — 2 colour and Z samples per pixel.
- Output a **3D velocity vector per rendered pixel**, with higher precision on X
  and Y for reprojection.
- **Offset the projection matrix each frame** so the pattern alternates between
  even and odd frames.
- **Gradient fixup**: divide the x-gradient by 2 so texture filtering behaves as
  it would at full width.
- **Reconstruct** the missing pixels from: current-frame neighbours weighted by
  linear Z; reprojected history colour and depth, using the velocity of the
  neighbour **closest to the camera** ("to preserve silhouette"); and confidence
  weights from colour coherency and velocity magnitude — with **YCoCg AABB
  clamping** against ghosting.

**[GDC-R]** The cost: **"Costs 1.4 ms — 8 to 10 ms net win."**

**[GDC-R]** And two bonuses: "particle effects can be easily evaluated per pixel
instead of per sample", and "**you can fit a lot more stuff in ESRAM**" —
**[inferred]** which on Xbox One was worth as much as the raw milliseconds, since
the 32 MB of ESRAM was the binding constraint on that machine's render targets.

### 4.5 Temporal AA on top, and the teeth filter

**[GDC-R]** TAA "integrates with the checkerboard rendering — can be run on the
same resolve shader", applying "**MSAA 4× style jitters on top of the
checkerboard pattern**" at sub-sample level.

**[GDC-R]** Plus a bespoke fixup: a **"teeth removal filter"** that identifies
sawtooth artefacts by detecting a **`01010` pattern across five adjacent pixels**
horizontally or vertically (with `1X100` given as an explicit *non*-teeth case)
and smooths them.

**[inferred]** That is the checkerboard's signature failure — a high-frequency
edge aligned with the sample lattice reconstructs as alternating filled and empty
pixels — and the fix is a five-tap pattern match. Ad hoc, cheap, and exactly the
sort of thing that only shows up once the technique is in a real frame.

### 4.6 Shadows — cached, and split static from dynamic

**[GDC-R]** "**All shadows are cache based**", using **cached Hi-Z for culling**.

- **Sun/moon**: full resolution. A shadow map containing **all static objects is
  built on load**. Cascade 1 is fully dynamic; **cascades 2 and 3 render dynamic
  objects only and blend with the static map**; **cascade 4 is substituted by the
  static map** outright. The deck notes this gives the "ability to scale shadow
  cost by mixing cascades with static map."
- **Local lights**: resolved at **quarter resolution** with **bilateral
  upscale**, results stored in a texture array, "lower VGPR usage on light
  accumulation." A **maximum of 8 visible shadowed local lights**; on a newly
  visible light the static map is rendered, then each frame only dynamic objects
  are rendered and composited.
- A **separate shadow pass** exists "to relieve lighting-resolve VGPR pressure",
  using a Hi-Z representation of the cached shadow map "to reduce the work per
  pixel."

**[inferred]** This is [`re_engine_rendering.md`](../rendering/re_engine_rendering.md) §1's
architecture — cache the static, repair with the dynamic — arrived at
independently, and Siege's version is under a harder constraint: **destruction
invalidates the static cache.** The deck's own note that "static lighting and
shadows are severely limited" is where that bites. **[inferred]** The workable
reading is that walls which can be destroyed simply do not participate in the
static shadow cache, which is a real quality cost the game pays quietly.

### 4.7 Clustered lighting and GI

**[GDC-R]** Lighting "uses a **clustered structure on the frustum**" — **32×32
pixel tiles with exponential Z distribution** — filled by "hierarchical culling
of light volumes". **"Local cubemaps [are] regarded as lights"** and injected into
the same structure. "Shadows, cubemaps and gobos reside in texture arrays";
deferred uses a pre-resolved shadow texture array, forward samples the shadow
depth buffer directly.

**[GDC-R]** Global illumination is **two voxel volumes**:

| Volume | Coverage | Contents | Resolution |
|---|---|---|---|
| **Low** | the whole map | sky visibility SH | **1–2 m per voxel** |
| **High** | the playable area only | sky visibility SH **+ bounce colour SH** | **25 cm per voxel** |

**[inferred]** Sky-visibility spherical harmonics is a compact, destruction-
friendly choice: it stores *how open to the sky each point is*, which is exactly
the quantity a hole in a wall changes, and it is cheap enough to update. Note the
coverage split — bounce colour, the expensive term, is stored only where players
can go.

**[GDC-R]** **Screen-space reflections** run at **quarter resolution** with
"temporal reprojection (ray-based accumulation, not depth-based)", gloss-dependent
linear marching, and **jittered ray start position and direction**; the SSR ray
trace is "done in async."

### 4.8 The GBuffer

**[GDC-R]** Four render targets — **RGB10A2 + 3× RGBA8** — plus **D32 depth /
S8 stencil**, with **inverted depth** so that "D32 float ensures uniform precision
distribution." Normals moved to **R10G10B10A2** (from best-fit normals) "to reduce
VGPR usage."

**[inferred]** The recurring theme across this deck is **VGPR pressure**, named
in four separate places as the reason for a design choice. On GCN, occupancy is
set by register count, and the deck is a good illustration that on that
architecture the register budget shapes the renderer as much as bandwidth does.

### 4.9 The line that matters most for this project

**[GDC-R]** One bullet, easy to miss:

> **"Poking holes degrades occlusion efficiency."**

**[inferred] That is the rendering tax of destructible geometry stated in five
words, and it applies directly here.** Occlusion culling works because walls
block things. A game in which walls stop blocking things is a game whose culling
progressively stops working *over the course of a round* — worst case arrives
late, when the most is destroyed and the fight is at its most intense. The
destruction deck says the same thing from the other side: "less occluders, can
see more objects."

The parallel to this project is exact: `OcclusionGrid` is a derived summary whose
*value* — not just its validity — depends on the geometry staying opaque. Any
feature that puts holes in walls does not merely dirty the cache, **it makes the
cache less effective, permanently, in the direction the round is heading.** That
is a budgeting fact rather than a correctness one, and it is the kind that gets
discovered in profiling rather than in design.

---

## 5. Ballistics

### 5.1 Surface penetration is not a special case — it is destruction

**[inferred]** The important structural point, which follows from §2: when a
bullet passes through a wall in Siege it is not consulting a "penetration
material" table and attenuating. It has **cut a hole**, the hole is real
geometry, and subsequent bullets and sight lines go through it because there is
now nothing there. **[GDC-D]** This is why the deck lists hi-resolution collision
against "the actual surface geometry, e.g. shooting" as a separate representation
from the simplified movement collision (§2.7) — shooting needs to know about
holes at their true size.

**[COMMUNITY]** Which is also why weapon calibre visibly changes how fast a
surface opens up: a larger round's cutter is bigger, so fewer shots clear a
sight line.

### 5.2 Limb penetration — through the body, not through the wall

**[UBI]** A separate system, added in Y4S4. Three classes:

| Class | Behaviour |
|---|---|
| **No penetration** | a bullet hits one body part and stops. The pre-Y4S4 behaviour for everything. |
| **Simple penetration** | may pass through one limb to hit **another body part on the same operator**; damage uses "the hit on the body part with the **highest modifier**". Cannot reach a third part, and cannot pass through one operator to another. |
| **Full penetration** | passes through the first target into a second person or surface; **subsequent targets take 70% of initial damage**. |

**[UBI]** The multipliers:

| Body part | Modifier |
|---|---|
| **Head / neck** | **50.0** |
| Torso (upper, lower, groin) | 1.0 |
| Arms / hands, legs | 0.75 at 1–2 armour, 0.65 at 3 armour |

with armour reducing incoming damage to 100% / 90% / 80% for 1 / 2 / 3 armour.

**[UBI]** And a targeted correctness fix worth recording: to avoid "issues where
hitting the lower back of an operator results in a headshot", the system **counts
only the first hit** when a bullet passes through the chest, neck or head region.

**[inferred]** That last rule is the interesting one. A naive "highest modifier
wins" rule combined with a 50× head multiplier means *any* shot whose ray happens
to exit through the head volume is a headshot — including one fired into the
spine from behind. The fix is not a physics change; it is a **special case
protecting a gameplay invariant from a general rule.** Worth remembering as a
category: when one multiplier dominates all others by 50×, every system that can
reach it needs auditing.

**[COMMUNITY]** Weapon classes map onto this: most weapons (ARs, LMGs, SMGs,
pistols, revolvers) are simple penetration; shotguns and machine pistols have
none; DMRs and sniper rifles have full.

**[COMMUNITY]** Damage falloff is **piecewise** — applied only between a start
and end range, linearly:

```
Damage = BaseDamage − RangeModifier × (Distance − FalloffStart) / (FalloffEnd − FalloffStart)
```

### 5.3 Explosions — raycasts to query points, not a radius test

**[UBI]** The model, which is more careful than the usual sphere overlap:

- Each explosion type has a **shape as well as a radius** — "a frag grenade is
  radial while a claymore is oblong."
- Physics finds candidate entities in the volume, then **raycasts from the
  epicentre toward multiple query points** on each operator's physics capsule and
  bounding volume — not one point per target.
- Final damage comes from **interpolating a damage curve against distance**.
- **Blockers carry metadata**: metal barricades and deployable shields stop the
  raycast and block damage; if nothing is hit, full damage applies.

**[inferred]** Multiple query points per target is the fix for the classic
explosion bug where a character is "in cover" or "not in cover" depending on
whether one privileged point — usually the pelvis — happened to be visible. It is
also `CLAUDE.md`'s rule about culling cheaply before testing expensively, in the
right order: bound the volume, collect candidates, *then* raycast.

**[UBI]** **Shrapnel (Y5S1)** changed one rule: previously a destructible object
in the way **capped** the explosion's radius outright, which made C4 oddly weak
indoors. Now "destructible objects will no longer limit the range of explosion
damage", but "damage applied to the player will be **reduced based on the number
of destructible objects the raycasts hit along the way**", with shrapnel impact
marks showing the direction damage came from.

**[inferred]** A binary blocker replaced by an accumulating attenuation — and the
visual feedback added at the same time, so the player can read the new rule off
the world rather than the patch notes.

---

## 6. Audio — the system destruction changed most

**[AUDIO]** Siege's sound propagation is worth a section because it is the
clearest case of a *non-rendering* system being rebuilt around mutable geometry,
and because its structure will look familiar.

**The representation:** "strategically placed points in the map, called
**Propagation Nodes**", forming a graph. A sound's route from source to listener
is the **lowest-cost path** through that graph, where cost depends on:

> "the path's **length**, its **cumulated angles**, and the **penalty assigned to
> the destruction level** of the specific Propagation Nodes impacted."

**Destruction drives it both ways.** Nodes inside an intact wall are
**unavailable**; "if a hole is created, the closest Nodes will be exposed to the
Propagation Path selection and will potentially let sound pass through depending
on the area impacted." And reinforcements run it in reverse — nodes go "from open
to closed." Different materials carry different penalties: "wooden barricades and
metal barricades both have their own obstruction settings."

**Diffraction is faked by moving the source.** The system "virtually
repositions the sound to reflect the direction of the paths, instead of the
actual position of the sound source, which ultimately simulates diffraction."

**Obstruction is baked or live depending on the source.** "Pre-rendered
simulation of the obstructed sound" for things like footsteps; "real-time
filtering" for performance-critical sources like gunfire. And an "Impulse Response
Reverb Processor" exists but "CPU constraints limited deployment", so reverb is
baked onto specific sources.

**[inferred] Three things transfer:**

1. **This is a navigation graph.** Nodes, edges, edge costs, a shortest-path
   search, and links that open and close as the world changes. It is the same
   structure as [`navigation.md`](../../topics/agents/navigation.md)'s coarse area graph, serving a
   completely different consumer — which is more evidence that the representation
   deserves an interface rather than a single owner. This project's
   `RoomPartition` is the same shape again.
2. **"Virtually reposition the sound"** is the cheapest good idea here. Rather
   than filtering to simulate a sound bending around a corner, *place the sound
   at the corner*. The listener's existing spatialisation does the rest, for
   free, and it is correct — the sound genuinely is arriving from that direction.
3. **Cost from length + cumulative angle + material penalty** is a nicely chosen
   metric: the angle term is what makes a long straight corridor read as closer
   than an equally long dog-leg, which is true and which pure distance cannot
   express.

---

## 7. What this means for cromwell

**[inferred]** Ordered by what is actionable.

**Take now — techniques, cheap, and this project has the surfaces for them:**

1. **If walls ever get holes, cut them in 2D.** §2.3. Project the planar surface
   into its own plane, clip the cutter polygon against it (Weiler–Atherton),
   triangulate with ear clipping, extrude. **Do not do 3D CSG.** The restriction
   to planar surfaces is what makes it "robust, fast, simple", and this project
   already has only planar surfaces. The design consequence to note *now* is that
   a cuttable surface wants to be **polygons in surface-local 2D**, not triangles
   in world space.
2. **Two collision fidelities, chosen by consumer.** §2.7. Simplified convex
   decomposition for movement — with small holes *semantically* removed because
   a body cannot fit through them — and exact surface geometry for shooting.
   Feed planes rather than hulls where the surface is planar.
3. **Decorations as geometry, not decals**, on any surface that can be cut. §2.5.
   Cut decorations go through the same clipper; feature-bound decorations just
   vanish when their edge does.
4. **Batch by material, let geometry be arbitrary.** §4.2. Destruction multiplies
   unique geometry and not materials, so organise the renderer around the
   invariant. `StaticsMesh` already batches; the point is that the batching key
   must be material-and-state, and geometry must be free to change under it.

**Take — the split that is the whole design:**

5. **Simulate what the player decides around; fake everything else.** §2.6. The
   hole is procedural because it is gameplay. The debris is instanced, pooled,
   box-collided and deleted on explosions because it is decoration. **This looks
   backwards and is not.** The corollary, §3.4: debris was kept *small on purpose*
   so it could be declared gameplay-irrelevant and therefore left unreplicated —
   and the community's sight-line complaints are exactly what happens at the
   boundary where that premise fails.

**Take — process and scheduling rules:**

6. **Keep a single-threaded, non-time-sliced path through concurrent code,
   permanently.** §2.9. A shipped team's stated position, and the payoff is being
   able to separate logic bugs from concurrency bugs in one run.
7. **Resumability is a shape, not a retrofit.** §2.9. Siege hand-rolled
   coroutines with macros to stop 60 ms spikes.
   [`moving_frame_navigation.md`](../../topics/agents/moving_frame_navigation.md) §8.2 argues the
   same for sliced pathfinding. Design it in or do not have it.
8. **Put expensive work behind an animation the fiction already required.** §2.9's
   pre-destruction. The wind-up is not hiding a stall, it is budgeting one.
9. **Bound complexity by policing the data**, not by optimising the code that
   consumes it. §2.9 — "limit degenerate cases", "implement features to help
   bound complexity", and train the artists.

**Take — determinism, if anything here is ever networked or replayed:**

10. **Symmetrical compression.** §3.1. Compress *and decompress locally* so both
    ends feed identical quantised inputs to a generator. Invisible until two
    machines disagree, and general to every deterministic system behind a lossy
    transport.
11. **Seed randomness from the event, not from a stream.** §3.2 — Siege uses
    impact position; ED uses surface position
    ([`elite_dangerous.md`](../space/elite_dangerous.md) §3.1). Needs no synchronisation
    and no ordering.
12. **Rollback does not scale to geometry.** §3.3. The revert unit is a full
    surface backup and the queue is unbounded in latency. Fighting games get away
    with it because their state is small.

**Confirmations, and one warning:**

13. **Cache the static, repair with the dynamic** — §4.6's cascade scheme is
    [`re_engine_rendering.md`](../rendering/re_engine_rendering.md) §1 again, under the harder
    constraint that destruction invalidates the static half. Siege pays for that
    in quality and says so.
14. **Sparse/clustered lighting with cubemaps as lights** — §4.7 matches the
    clustered-forward plan and [`source2_rendering.md`](../valve/source2_rendering.md)
    §13.
15. **⚠ "Poking holes degrades occlusion efficiency."** §4.9. The single most
    important warning in this note for this project. Destructible geometry does
    not merely dirty `OcclusionGrid` — **it makes occlusion culling
    progressively less effective as a match proceeds**, so the worst frame is the
    latest frame. That is a budgeting consequence, not a correctness one, and it
    will be found in a profile rather than in review.

**Explicitly not to take:**

- **Checkerboard rendering.** §4.4. It was an ESRAM-and-Jaguar-era answer, its
  reconstruction needs velocity buffers and a bespoke teeth filter, and modern
  temporal upscalers occupy the same ground better. Read for the *reasoning* —
  they tested PSNR across an image set before committing — not for the technique.
- **Believing the benchmark table.** §2.8. L'Heureux's own words: "to take with a
  heap of salt." Use the *shape* — that a heavy explosion exceeds a frame, and
  that this is why async and slicing exist — not the digits.

---

## Sources

**Primary — the two GDC 2016 decks**
- [The Art of Destruction in 'Rainbow Six: Siege'](https://media.gdcvault.com/gdc2016/Presentations/LHeureux_Julien_Art_Of_Destruction.pdf) — **Julien L'Heureux, Technical Lead / Physics Programmer, Ubisoft Montreal.** RealBlast's structure, the connection-based leaf graph, surface procedural destruction (3D→2D projection, Weiler–Atherton clipping, ear-clipping triangulation, cutter taxonomy, texture cutters), decorations vs decals, the debris decision, collision simplification, budgets and the platform benchmark table, async / pre-destruction / time slicing, and the determinism and replication section. [GDC Vault page](https://www.gdcvault.com/play/1023003/The-Art-of-Destruction-in) · [talk video](https://www.youtube.com/watch?v=SjkQxowsL0I)
- [Rendering 'Rainbow Six | Siege'](https://media.gdcvault.com/gdc2016/Presentations/El_Mansouri_Jalal_Rendering_Rainbow_Six.pdf) — **Jalal El Mansouri, Technical Architect, Ubisoft Montreal.** Frame budgets, material-based draw calls and the unified buffers, three-level culling with the results table, checkerboard rendering and the resolve, TAA and the teeth-removal filter, cached shadows, clustered lighting, the two GI voxel volumes, GBuffer layout, ESRAM. [GDC Vault page](https://gdcvault.com/play/1022990/Rendering-Rainbow-Six-Siege) · [OCR full text, Internet Archive](https://archive.org/stream/GDC2016Mansouri/GDC2016-Mansouri_djvu.txt) · [talk video](https://www.youtube.com/watch?v=RAy8UoO2blc)
- [How Rainbow Six Siege was rendered](https://www.gamedeveloper.com/programming/how-i-rainbow-six-siege-i-was-rendered) — Game Developer's write-up of the same talk

**Ubisoft, on gameplay systems**
- [Dev Blog: Limb Penetration System](https://www.ubisoft.com/en-us/game/rainbow-six/siege/news-updates/3JoP5bWperGVXX3N0nNHlM/dev-blog-limb-penetration-system) — the three penetration classes, body-part modifiers, armour reduction, and the first-hit rule that stops back shots registering as headshots
- [Dev Blog: Explosions & Shrapnel in Y5S1](https://www.ubisoft.com/en-us/game/rainbow-six/siege/news-updates/1QkezaGoRkDWqcQ6duGvtk/dev-blog-explosions-shrapnel-in-y5s1) — explosion shapes, raycasts to multiple query points, the damage curve, blocker metadata, and the shift from radius-capping to per-object attenuation
- [The Art of Destruction in Rainbow Six Siege — interview with Julien L'Heureux](https://news.ubisoft.com/en-us/article/4GHX2yepSaKkflLjLAlpwO/the-art-of-destruction-in-rainbow-six-siege-an-interview-with-julien-lheureux) — the design framing around the GDC talk
- [Rainbow Six Siege Status Report](https://www.ubisoft.com/en-us/game/rainbow-six/siege/news-updates/HyDyYTuRkWtyYQ3WDFeEL/rainbow-six-siege-status-report) — including the acknowledged client-side debris consistency problem

**Audio**
- [Game Design Deep Dive: Dynamic audio in destructible levels in Rainbow Six: Siege](https://www.gamedeveloper.com/design/game-design-deep-dive-dynamic-audio-in-destructible-levels-in-i-rainbow-six-siege-i-) — Propagation Nodes, path cost from length + cumulative angle + destruction penalty, nodes opening and closing with destruction and reinforcement, source repositioning to fake diffraction, baked vs real-time obstruction. Also summarised at [80.lv](https://80.lv/articles/learn-all-about-dynamic-audio-in-rainbow-six-siege)

**Community (weakest tag)**
- [Bullet Penetration](https://rainbowsix.fandom.com/wiki/Bullet_Penetration) and [Destruction](https://rainbowsix.fandom.com/wiki/Destruction) — weapon penetration classes, breachable / semi-breachable / unbreachable surface types
- [A comparison of damage falloff in PvP FPSs](https://zekevirant.medium.com/a-comparison-of-damage-falloff-in-pvp-fpss-7be74fbb131) — the piecewise falloff equation
- [A Year Playing with Destruction in Rainbow Six Siege](https://www.youtube.com/watch?v=2OL_58miezE) — Alexandre Ouimet, Ubisoft physics programmer, at 4C 2018. A follow-up to the GDC talk; **not read for this note**, and the obvious next source if this section is ever extended
