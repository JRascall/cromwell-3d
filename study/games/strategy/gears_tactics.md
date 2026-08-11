# Gears Tactics — the gridless tactics game, and what runtime level assembly costs

Deep dive on **Gears Tactics** (Splash Damage + The Coalition, 2020, UE4): how it
handles space without a grid, how its levels are assembled at load time from a
prefab library, what that decision forced the renderer to become, and how its AI
schedules a turn.

Source tags follow the rest of `study/`. **[DEV]** is a first-party technical
write-up by someone who shipped it. **[MS]** is a Microsoft/DirectX engineering
blog. **[PRESS]** is interview or press material — good for design intent and
quotes, weaker on mechanism. **[COMMUNITY]** is player-side documentation of
observable behaviour. **[inferred]** is our reading.

> **The one sentence.** Gears Tactics made two decisions that look unrelated and
> are the same decision: **no grid** and **no pre-built levels**. Both push
> authority out of a data structure and into the geometry — and almost every
> system described below exists to pay for one of them. The rendering effort pays
> for the second; the AI's entire latency-hiding architecture pays for the first.

**Relevance warning.** This is the study note most opposed to how this project is
built. Read it as the argued alternative, not as a target. §10 does that reading
properly.

---

## 1. The design decision everything hangs off

**[PRESS]** Design director Tyler Bielman, to PCGamesN:

> "We don't take place on a grid, and that allows you to go anywhere in the map
> you want, take any shooting angle you want, and find cover anywhere you want."

**[PRESS]** And on how much that cost them:

> "We went in prototyping really hard to make sure [that would work], because it
> was a risk for us, and we think it paid off wonderfully because the game feels
> so much more fluid."

**[DEV]** Splash Damage's own level-design write-up repeats it from the other
side: the system works without a traditional grid, giving "freer space and unique
angles that cannot be achieved in other tactics games."

**[PRESS]** The origin story is worth keeping because it explains the shape of
the whole game. Executive producer Grimbley: "We actually just took existing
Gears and just moved the camera up. The cover-based nature of [the game] really
stood out even from that first test of the camera angle." The tactics layer was
grafted onto a third-person cover shooter, not designed from a board game
outward. That is why the unit of tactical space is a **cover object**, not a cell.

**[inferred]** This is the load-bearing distinction and it is worth stating
plainly before anything else. In XCOM, and in this project, *the cell is the
primitive* and geometry is a rendering of it. In Gears Tactics, **the cover
volume is the primitive** and position is continuous. Everything downstream —
what you can query cheaply, what has to be authored by hand, what can be
procedurally assembled, what has to be verified by playtest rather than by
assertion — follows from which of those two you pick.

---

## 2. Movement

### 2.1 The model

**[COMMUNITY]** Three action points per unit per turn, spendable in any mix:
move, shoot, grenade, ability. Movement spends one AP for a **distance budget**,
not for a number of tiles. Units move freely in all directions inside that
radius, vault cover, and slide into doorways.

**[COMMUNITY]** Two refinements that only exist because the space is continuous:

| | |
|---|---|
| **Cover magnetism** | A unit may **exceed** its movement budget slightly to slide into cover. Ending a move in cover reduces the effective cost — the extra metres are a gift for choosing a defensible endpoint. |
| **Waypoints** | Hold Ctrl and click to place intermediate points, routing a move around an overwatch cone or an opportunity-fire radius while spending minimum range. |

**[PRESS]** The magnetism is deliberate and was tuned against a specific failure:
the developers ensured units snap into cover rather than standing exposed
alongside it, because standing exposed made "the unit feel unintelligent."

**[inferred]** Cover magnetism is the tell that the free-movement model is not
actually free. It is a **continuous space with discrete attractors**, and the
attractors are exactly the hand-authored cover volumes. Functionally that is a
graph of cover nodes with continuous interpolation between them — the same
information a tactics grid carries, expressed as geometry plus markup instead of
as cells. The player-facing win is real (any angle, any distance) but the
underlying tactical decision set is not as much larger as it looks.

### 2.2 The spatial layer

The Game AI Pro chapter never sets out to document navigation, but it leaks the
whole thing in asides. Collected:

**[DEV]** **Navmesh, and a rebuildable one.** Among the causes of stuck enemy
turns: "wrong content that triggered navigation mesh rebuilds every frame." So
there is a navmesh, it can rebuild at runtime, and *content* can provoke it.

**[DEV]** **UE4 EQS.** The performance section names it outright — "The AI module
in UE4 is already heavily time-sliced for things like environment queries," and
"we had different planning layers triggering several different environment
queries at different times." The Visual Logger captures "environment query
results" per session.

**[DEV]** **Cover nodes.** Figure 4's caption: "Rectangles represent cover nodes."
The authored markup of §3.2 becomes discrete nodes at runtime.

**[DEV]** **And the sentence that answers the whole question:**

> "Free movement requires testing a lot of sample points in the world while the
> live bullet system requires ray casts from each of those locations **since they
> cannot be precomputed**."

**[inferred]** That is the cost of gridlessness stated by the person who paid it.
Position selection is **EQS sampling** — generate candidate points, score them —
and each candidate needs raycasts because the game simulates real projectiles
("live bullets") rather than resolving a shot as a dice roll against a cover
flag. Live bullets are what forbids precomputation: a stray round's path depends
on the actual geometry between two actual points, so no per-cell visibility
summary can stand in for it. Combined with high enemy counts, this is why
planning time became the game's defining performance problem (§7.6) — and why an
entire subsystem exists to *hide* it.

**[inferred]** So the answer to "is there a baked spatial structure": **no, and
they say why not.** They pay per-query, at planning time, and engineer around the
latency rather than removing it. That is the exact trade a cell grid refuses.

### 2.3 Ray marching: not this

Since the question that started this note was whether Gears Tactics builds a grid
by ray marching — **it does not, and the chapter says so in as many words.** Rays
are cast on demand from EQS sample points, and §2.2's quote states they "cannot
be precomputed" because of the live-bullet system. There is no lattice, no bake,
no march. There is a large number of individually cheap traces, run at planning
time, whose aggregate cost is the game's main AI performance problem.

The technique does exist, though, and it is worth recording because it is the
road not taken. **[DEV]** The canonical UE4 dynamic-cover write-up lays out the
two ways to generate cover data automatically:

| | How | Verdict |
|---|---|---|
| **3D object scanning** | Slice an actor's bounding box into a 3D grid on X/Y/Z; raycast downward from every point to find where the object's perimeter meets the ground; filter on ground gap and cover height; project survivors onto the navmesh. | Thousands of points per object, cost scales with bounding-box size, poor on landscape. Uniform point distribution, near-zero error, and the **only** option for objects with no collision geometry (force fields). |
| **Navmesh edge-walking** | Take pairs of navmesh vertices, cast perpendicular to each edge in both directions. With ledge detection and slope tolerance, up to 8 rays per vertex worst case. | "Considerably faster"; object size irrelevant; handles rugged terrain and multi-storey. Error-prone at navmesh tile boundaries, less uniform distribution. |

The author's recommendation is edge-walking for ~90% of needs, grid-scan reserved
for what edge-walking cannot see. **Both are bakes**, run on spawn or on navmesh
tile update — neither is a per-frame march.

**[COMMUNITY]** Gears Tactics is on the far side of even this: **Gears of War 1
used hand-placed cover nodes**, as did Half-Life 2, and Gears Tactics inherits
that lineage. Its cover is hand-marked-up per prefab tile (§3.2), not generated.

**[inferred]** That is the right call for their content model and the wrong one
for most. With a fixed library of a few hundred authored prefabs, hand markup is
a bounded, one-time cost per tile, and it buys designer intent that a bake cannot
express — *this* is the flank route, *this* wall is the one you're meant to
break. Automated cover baking earns its keep when levels are open-world or
user-generated, i.e. when there is no author to do the marking.

---

## 3. Cover, and the metric system that stands in for a grid

### 3.1 The metrics

**[DEV]** Level design holds to fixed measurements, in Unreal units:

| | |
|---|---|
| Full cover height | **96 u** |
| Half cover height | **48 u** |
| Minimum playable width | **96 u** — "cannot be lower than 96u wide to ensure that two characters can walk next to each other without blocking the path" |
| Elevation change | **384 u** |

**[DEV]** And from the art side, modular wall pieces are authored at **384×384**
and **192×384** units.

**[inferred]** Every one of those numbers is a multiple of 96: 48 = 96/2,
192 = 2×96, 384 = 4×96. **There is a grid. It lives in the authoring
conventions and the asset dimensions rather than in a runtime array.** That is
what makes blockouts look gridded to the eye while the movement genuinely is not.
The design gets continuous positioning; the art and level pipeline get the
modularity, snapping and reuse that only a fixed module affords. Splitting the
grid in two like this — module for authoring, continuum for play — is the actual
clever bit of the game and it is barely discussed anywhere.

### 3.2 The markup

**[DEV]** Cover is placed by hand, per tile, with a colour convention:

| Marker | Meaning |
|---|---|
| Red boxes | Standing (full) cover |
| Pink boxes | Low cover |
| Yellow targets | Firing angles and vault possibilities |

**[DEV]** Tiles pass a validation playtest before going to environment artists,
and the design team stays involved through art dressing to keep readability and
metric adherence intact.

**[inferred]** That last sentence is a process cost, and it is the price of the
whole approach. Because gameplay meaning is painted onto geometry rather than
derived from it, **art dressing can silently break gameplay**, and the only
defence is a human watching. Contrast the derived-cache discipline in this
project's `CLAUDE.md`: `testOcclusionGrid` checks every bit against the tiles, so
geometry and gameplay cannot drift. Gears Tactics has no equivalent check
available to it — there is no authoritative source for the markup to be tested
against, because the markup *is* the source.

### 3.3 The combat model over the top

**[COMMUNITY]** Percentage hit chance, fully exposed through a "Tac Com" breakdown
(bound to R) showing every contributing factor. Inputs: shooter **Accuracy**,
target **Evasion**, **distance** (closer is worse for the shooter's odds in
several weapon profiles — an inversion of the XCOM convention), and target
**cover state** (none / half / full).

**[COMMUNITY]** Cover types behave differently rather than just scaling a number.
Low cover lets you shoot over the top and leaves you more exposed; full cover
protects frontally but restricts you to shooting around corners.

**[COMMUNITY]** **Cover is destructible by material.** Wooden cover breaks after
roughly one shot and knocks the occupying unit back; metal cover does not.
Grenades destroy cover and can interrupt an enemy's overwatch.

**[COMMUNITY]** **Overwatch is a cone, and the cone is a resource.** The player
drags out the arc: wider arc = more area covered but **reduced range and
accuracy**; narrower = more lethal but easier to walk around. Unspent AP at end
of turn become overwatch shots, so a unit holding three AP fires three times.
Overwatch also triggers on an enemy taking an offensive action inside the cone
without moving.

**[COMMUNITY]** **Executions** are the tempo mechanic: finishing a downed enemy
grants an action point to the **entire squad**, not just the executioner.

**[inferred]** Executions and the multi-shot overwatch are the two mechanics that
make this play fast rather than deliberate, and both are AP-economy valves. The
execution refund in particular makes aggression self-financing, which is the
stated design goal — Splash Damage repeatedly frame the game as more aggressive
and faster-paced than XCOM. It also means the AP budget is *not* the pacing
constraint the way it is in XCOM; positioning is.

---

## 4. Parcels — the level pipeline

**[DEV]** "Gears Tactics levels are generated at level load time from a library of
pre-built encounter spaces we call 'Parcels'." Splash Damage's design-side term
for the same thing is **tiles**.

| | |
|---|---|
| **Library size** | Hundreds of tiles, varying sizes |
| **Connector tiles** | The majority. Traversal between points; carry the bulk of gameplay. |
| **Objective tiles** | Larger, built around a point of interest, designed to offer multiple approaches so the approach decision is meaningful. |
| **Assembly** | Campaign levels are handcrafted *from the same tiles*; non-campaign maps assemble procedurally from tile metadata. |
| **Rotation** | Tiles rotate at runtime — the same space plays differently depending on which elevation you enter from. |

**[inferred]** The campaign-uses-the-same-tiles detail matters more than it
reads. It means there is **one content pipeline**, not a bespoke campaign pipeline
plus a procedural one, and the procedural generator's vocabulary is guaranteed to
be as good as the handcrafted levels because it is the same vocabulary. It also
means every tile has to be good in isolation and in arbitrary neighbour context,
which is what the per-tile validation playtest (§3.2) is enforcing.

---

## 5. What runtime assembly costs the renderer

This is the part worth reading closely, because it is a clean worked example of
**one architectural decision paying for itself in one place and billing for it in
four others.**

**[DEV]** Everything in this section exists because the world does not exist until
load time, so nothing about it can be baked in the usual offline sense.

### 5.1 Global illumination

**[DEV]** A **custom GI system using precomputed radiance transfer** to bounce
light seamlessly across parcel boundaries.

**[inferred]** PRT is the obvious fit and probably the only affordable one. The
lighting *within* a parcel can be precomputed offline because the parcel's
geometry is fixed; what cannot be precomputed is the light exchange *between*
two parcels that have never met before. PRT gives you a per-parcel transfer
basis — "how does this geometry respond to incoming light from any direction" —
which is exactly the quantity that survives being placed next to an arbitrary
neighbour. Bake the transfer, evaluate the incident lighting at runtime, and the
seam disappears. Standard lightmapping cannot do this at all, because a lightmap
bakes the *answer* rather than the *response*.

### 5.2 Planar reflections

**[DEV]** Custom planar reflections that "render the reflections of the
environment outside of the view frustum, which is quite often the case with an
isometric camera," using **shadow data and the custom GI** to generate reflections
"without needing to render a mirror of the entire scene."

**[DEV]** The shipping graphics option describes it as reflecting off-screen
objects via a top-down camera perspective.

**[inferred]** Two problems solved at once. The isometric camera means most of
what should appear in a wet floor is off-screen, so SSR alone fails badly — this
is a camera problem, not a parcel problem. But reusing the GI and shadow data
rather than re-rendering geometry is a parcel problem: a mirror pass over a
runtime-assembled world would double an already-expensive scene submission.

### 5.3 Everything else that had to move to runtime

**[DEV]** Reflections, shadows, lighting **and the global animation system** are
all done at run time, and these were systems that "had to be built from scratch"
because they did not exist in UE4.

### 5.4 Materials — the parcel constraint pushed into shading

**[DEV]** The material work is the clearest illustration of the tax, because the
problem is stated explicitly:

> Because levels were generated at run-time, puddles had to work across different
> parcels of different sizes that could be randomly rotated, so manually painting
> puddles with vertex colour on a per tile basis was not an option.

**[DEV]** The solution: procedural noises authored in Substance Designer, **projected
vertically at different scales**. Macro scale isolates wetter areas across
multiple tiles with no seam at parcel boundaries; micro scale blends using the
underlying material's heightmap to break up the tiling pattern.

**[inferred]** A world-space vertical projection is invariant to tile rotation
and tile size, which is precisely why it works. Anything authored in tile-local
UV or vertex-colour space is destroyed by a random rotation. **The rule the whole
material set obeys: if a feature must be continuous across a seam, it must be a
function of world position, not of the asset.**

**[DEV]** The wetness response itself is physically argued rather than a lerp to
dark:

| Surface | Response |
|---|---|
| Dielectrics | Albedo darkens (water absorbed into pores); roughness extrapolates porosity |
| Metals | Specular reflectance reduced via Fresnel interpolation from 90° to grazing |
| Vertical vs horizontal | Separate accumulation behaviours — drips vs drops — blended across slopes |

**[DEV]** The fire/scorch system is the same idea for a dynamic input. Material
functions take fire source **positions and radii** and output a burnt variant at
runtime per physical material; metal near a source gets vertex deformation based
on mesh scale and thickness at melting point; soot is applied as **deferred
decals** to save on performance. The number of fires that can contribute to one
mesh is **capped at the 4 closest**, resolved by an offline preprocess.

**[inferred]** That 4-nearest cap resolved offline is the same move as the
hot-loop rules in this project's `CLAUDE.md` — bound the per-element work to a
constant and resolve the association outside the loop, rather than letting a
shader iterate a variable-length list of fires.

**[DEV]** Two validation view modes shipped in-editor: a **PBR view mode with a
colour chart** so artists could see whether albedo and metallic were physically
plausible (replicated as both a UE material function and a Substance Painter
filter, so it reads the same in both tools), and an **HDR validation mode** for
light intensity consistency from armour LEDs to street lamps to the sun.

**[inferred]** Worth stealing outright. These are unit tests for art, in the
authoring tool, and they are cheap. A project that bakes light has an even
stronger case for them than one that doesn't.

### 5.5 Asset pipeline

**[DEV]** Substance Painter/Designer throughout, replacing a Photoshop workflow;
character shading "entirely designed around the capabilities of the Substance
toolset."

**[DEV]** Environments built from trims and tileables authored in Designer, with
masks enabling in-editor variation and albedo "kept in a simple and neutral
range" so it can be customised in engine. Wall modules at 384×384 and 192×384 u
needed centre-tiling solutions via gradient blending.

**[DEV]** Non-destructive **RGB mask layering** — weathering, edge wear, rust and
decal placement adjustable without rebuilding textures, so one asset yields many
in-game variations.

**[DEV]** Claimed **~80% memory saving** on character customisation from the
procedural workflow. Typical armour: two BC7 maps (albedo, AMST) plus a BC5
normal = **64 MB at 4K with 13 mips**.

---

## 6. Variable Rate Shading

**[MS]** Gears Tactics shipped **Tier 1 VRS only** — shading rate set per draw
call. The stated reason is hardware reach: Tier 1 works on far more GPUs, and the
goal was lowering the PC barrier to entry.

### 6.1 Which passes took it

| Accepted | Rejected (visible artifacts) |
|---|---|
| PreLighting / light composition | Translucency and opacity-masked objects |
| Screen-space reflections, SSR temporal AA | Composition after lighting |
| Light attenuation, light shaft bloom | Dynamic objects casting fully dynamic shadows |
| Direct deferred lighting | |

### 6.2 The three heuristics

**[MS]** Rather than a flat rate, three context rules decide where quality can go:

1. **Object size** — aggressive VRS on meshes that are tiny in world space.
2. **Depth-of-field masking** — reduced rate on objects already blurred, in cutscenes.
3. **Fog-of-war masking** — rate scaled by how obscured that part of the battlefield is.

**[inferred]** The third is a genuinely game-specific idea and the most
transferable one here. It is free information: the game already knows which
regions are fogged, so it already knows which pixels the player is not being
asked to read detail from. **Any game with a visibility model has this signal
lying around unused.** The general principle — *drive shading rate from gameplay
state you already compute, not only from screen-space analysis* — is the takeaway.

### 6.3 Numbers

**[MS]** RTX 2080 Super, 4K, Ultra:

| Mode | GPU frame time | Saving |
|---|---|---|
| Off | 23.3 ms | — |
| **On** (minimal quality loss) | 20.9 ms | **10.3%** |
| **Performance** (moderate tradeoff, permits 4×4) | 18.9 ms | **18.9%** |

### 6.4 The Tier 2 sequel, for contrast

**[MS]** The Coalition's later Tier 2 work (Gears 5, not Tactics) is the more
sophisticated system and shows what Tactics gave up. The shading rate image is
built by a **Sobel edge-detection compute shader over the final scene colour
buffer**, analysing luminance in sRGB so the threshold is perceptual. It picks up
not just edges but shadowed regions, volumetric fog occlusion, dense particles,
and motion/DoF blur.

**[MS]** Implementation details worth keeping:

- SRI tile granularity is **8×8 or 16×16** depending on hardware.
- They **skip edge detection on the outer boundary** of each tile — 36 pixels
  instead of 64 for an 8×8 tile, a **44% cut** to the analysis cost.
- A **second, conservative SRI** with stricter thresholds exists for passes where
  coarse shading misbehaved — **explicitly citing SSR in Gears Tactics** as the
  case that motivated it.
- SSGI runs on compute, so it needed **VRS emulated in-shader** rather than
  hardware-driven.
- Tier 2 gains: **up to 14%** with no perceptible impact on a 6900 XT at 4K
  Insane with SSGI; **8–20%** across settings.

**[inferred]** The interesting comparison is that Tier 1 got 10.3% and Tier 2 got
~14% for vastly more machinery. The per-draw approximation captures most of the
available win. Tier 2's real value is not the extra 4% — it is that it composes
with translucency and dynamic resolution scaling, which Tier 1 could not, so the
quality compromises go away rather than the frame time.

### 6.5 Shipping targets

**[DEV]** The PC tuning blog's stated targets, which are a useful sanity check on
what this class of renderer costs:

| Hardware | Target |
|---|---|
| Intel Gen 11 laptop | 30+ fps at 900p, VRS on |
| GTX 1060 | 60+ fps at 1440p, medium-high |
| RX 5700 XT | 60 fps at 4K with the minimum-frame-rate (dynamic res) setting |

**[DEV]** Also shipped: dynamic resolution driven by a minimum-frame-rate target,
compute-based contact shadows ("compute-based ray casting in screen space"),
compute-based glossy reflections, async compute, HDR10 with wide colour gamut.

---

## 7. AI — the best-documented system in the game

**[DEV]** Matthias Siemonsmeier (Lead AI Programmer, Splash Damage) presented
*Make It Fast: Simultaneous AI Actions in Gears Tactics* at Unreal Fest Online
2020, written up as **Game AI Pro Online 2021, chapter 3**. Everything in this
section is from the chapter text unless tagged otherwise.

### 7.0 The problem, which is not the problem you would guess

Worth following the derivation, because the design constraint that produced the
architecture is a *content* observation, not a technical one.

**[DEV]** Two enemies existed in the first prototype. The **Drone** is a "mirror
unit" — cover, machine gun, roughly the player's capabilities — and worked
immediately at parity numbers. The **Wretch** is fast, agile, melee, and at
comparable counts it *failed*: the player simply shot them before they closed,
"leading to a reduction of decision space for the player. The player did not have
to think about the best action as the best action was always to shoot at them as
soon as possible."

**[DEV]** At high counts the Wretch became good — real decisions about when to
attack, use an ability, or run — and the kill count matched Gears' action-game
identity. **So the game needed many enemies.** And with many enemies, sequential
turn execution was unwatchable: Wretches spawn in groups far away and all move
the same direction, so "the player typically had all the information they needed
after the first unit moved."

**[DEV]** Executing everything simultaneously fixed the boredom and broke
comprehension — the player "often now missed crucial information." Traditional
squad AI was rejected too: units would pursue one goal and never split, and
"Gears Tactics has a free movement system without an underlying grid. The levels
can have huge open areas, but also narrow indoor areas." Flocking, follow-the-
leader and dynamic group splitting were all tried and discarded.

**[DEV]** The resulting goal statement, quoted in full because every clause is a
constraint that shows up later:

> "Fast-paced enemy turns that pose a challenge to the player, without losing
> tactical clarity in a game with a high enemy count, a free movement system,
> live bullets, and semi-procedural generated maps."

**[inferred]** Four of those five clauses are consequences of decisions made
elsewhere in this document. Free movement (§1), live bullets, semi-procedural maps
(§4), and the high enemy count forced by the Wretch. The AI architecture is
downstream of all of them. This is the clearest case in the whole note of an
early design choice billing a distant system.

### 7.1 The Rules of Tactical Clarity

**[DEV]** Three rules, arrived at empirically and stated as law:

> 1. **Never attack more than one player unit with direct attacks simultaneously.**
> 2. **Try to split the actions of a single unit as little as possible.**
> 3. **Highlight actions that apply drastic changes to the game situation for the
>    player** (e.g. push-back actions) **by playing them exclusively.**

**[inferred]** These are readability constraints on a scheduler, and they are the
actual specification the whole system implements. Note that all three *cost*
speed — the system's stated purpose is to be fast, and its three governing rules
each forbid a speedup. That tension is the design.

### 7.2 The WorldState — the foundation, and the part worth stealing

**[DEV]** Pre-planning an entire turn requires every unit to know what the others
*intend*, not just where they are. Two failure cases motivate it: two units
planning to move to the same spot, and a unit planning a shot through a lane a
friendly is about to move into.

**[DEV]** The **WorldState** is the state of the game after executing an ability
— health, locations, status effects of all units. Abilities are **node
sequences** (a content-driven system), and **each node can simulate its outcome
into the WorldState instead of modifying the game world**. Every planning layer
reasons on it and mutates it as it queues actions. Because the simulation is a
property of the node, "designers did not have to take simulation into account
when creating new abilities."

**[DEV]** One rule prevents the obvious exploit: simulation happens **after the
unit commits to an action**, not before. Otherwise "it would lead to enemies
never taking a shot they would miss, for example, which would be cheating and not
very interesting to the player."

**[DEV]** The chapter's closing advice is about this and nothing else:

> "Work on the basics first and make sure that the WorldState is an integral part
> of all systems, not just AI. This will help further down the line, I promise!"

**[inferred]** The commit-then-simulate ordering is a small detail with a large
consequence and it generalises to any speculative AI. A planner that can see the
outcome before choosing is an oracle, and oracles read as cheating even when they
are not optimal. **The fix is not to weaken the simulation — it is to move the
commit point in front of it.**

### 7.3 Five layers, not three

**[DEV]** Each layer takes a plan and produces a new plan — Figure 1 labels every
arrow between layers **"Plan"**, and only the arrow *out of* the last one
**"Behavior"**. Four plan-to-plan transforms, then one plan-to-behaviour
translation. The AI never emits behaviour until the final layer.

| Layer | What it does |
|---|---|
| **Level Scripting** | Scripters take over a unit for one or more turns and queue specific abilities via dedicated nodes. Used heavily in the tutorial. **No player-visible difference between scripted and self-planned units** — deliberate, because the tutorial sets expectations for the whole game. |
| **Goal Planning** | Fuzzy-logic goal assignment (§7.4). Mission objectives reduce to **attack** or **defend**; also handles global decisions like buffing before shooting. |
| **Unit Planning** | If goal planning left AP unspent or assigned no goal, the unit falls back to a **default behaviour tree per unit type**, built around that type's desires. |
| **Combo Move Analyzer** | Post-planning reorder and bundle (§7.5). |
| **Plan Execution** | Walks the plan, dispatches to units, handles interruptions (§7.6). |

**[DEV] Correction to the secondary sources.** The AI and Games summary describes
the coordination layer as **"partial-order planning"** with "threats" — classical
planning terminology. **The chapter uses neither term.** What it describes is a
post-hoc reordering pass over a fully-formed plan, constrained by per-unit action
order and pre-ability dependencies. The distinction matters: it is a scheduler
over a committed plan, not a planner searching partially-ordered plan space.

### 7.4 Goal planning is fuzzy assignment, and it is data

**[DEV]** Goals are authored as data. The chapter's worked example — the goal to
execute a downed-but-not-out enemy:

| Field | Value |
|---|---|
| **Target** | DBNO Enemy — one goal instance is created *per* valid target |
| **Destruction** | Target Invalid |
| **Activation** | Target CanBePerceived |
| **Deactivation** | ¬(Target CanBePerceived) |
| **Priority** | 60.0 – 69.0, interpolated by (Distance ToFriendly) |
| **Max Score / Max Units** | 1.0 / 1 |
| **Assignable Units** | Wretch 1.0 × (Distance ToTarget); Drone 0.9 × (Distance ToTarget) |
| **SubGoals** | Empty |

**[DEV]** Priority rules are fuzzy and interpolate between a min and max. Other
cited priority inputs: distance to enemies, objective progress (resources
collected), friendly count in an area and their health. **Goals interact** — a
capture circle held by nobody outranks one already held; healing a friendly
raises the area's troop strength and thereby affects the defend goal.

**[DEV]** The assignment loop: pick a candidate unit for a goal, hand it a
**planning job**; the unit runs a behaviour tree *for that assignment type* and
decides for itself how to satisfy it (a sniper overwatches from range, a melee
unit closes). The tree reports success or failure back; the goal system updates
the goal's **insistence** (its runtime priority), **re-sorts all goals by
insistence, and repeats** until every unit is assigned or every goal is fulfilled.

**[DEV]** On failure, the changes each `Queue Ability` node made to the WorldState
**must be reverted**.

**[DEV]** Collaboration rides the same machinery. A player unit in cover generates
a **push-out-of-cover goal**; units with a cover-push ability get **priority to
plan first**, so units planning afterwards inherit the raised hit chance. The same
system arbitrates exclusive goals like reviving a downed teammate — without it,
several units would independently decide to walk to the same casualty, which
"could contradict with the overall tactical decision that the player would
perceive as natural."

**[inferred]** The plan-order priority for cover-push is the sharpest idea here.
Cooperation is not negotiated between units and is not represented anywhere — it
is produced by **sequencing who plans first**, so a selfish unit planning later
simply finds a better world. Turn order in the *planner* is a coordination
primitive. That is cheap and it composes.

### 7.5 The plan, and the Combo Move Analyzer

**[DEV]** A plan is a sequence of three element types:

| Element | Definition |
|---|---|
| **Ability** | Triple of (unit, ability, run-time data) — "who, what, how". Run-time data holds the shoot target or move destination. |
| **Sync** | Barrier marker. All actions **between two Syncs** are assigned to their units simultaneously; each unit runs its own assigned actions in sequence. |
| **Combo** | A bundle of other elements. |

**[DEV] From the figures rather than the prose.** Figures 4, 5 and 6 draw cover
nodes as **rectangles at arbitrary angles**, not axis-aligned — each node carries
an **orientation**, and units approach and attach to a specific face. Figure 6
draws the player's overwatch as a genuine **wedge with its apex at the unit**,
matching the drag-to-size cone of §3.3. Numbered arrows in each figure are
*planning order*, not execution order — which is the whole point, since the
analyzer is about to change the second without touching the first.

**[inferred]** Oriented cover nodes are the missing half of §2.2. A cell grid
stores "is this cell cover" and derives facing from which neighbour you came
from; a node graph must store the facing on the node, because there are no
neighbours. It also means cover attachment is a *node + face* pair rather than a
position, which is what makes cover magnetism (§2.1) implementable at all — the
magnet has somewhere specific to snap to.

**[DEV]** The worked example (Figure 4: three AI units A, B, C; player units X and
Y). The naive per-unit plan:

```
(A, move); (A, shoot, X); (Sync); (B, move); (B, shoot, Y); (Sync); (C, move); (C, shoot, X); (Sync);
```

Dropping all Syncs runs everything at once — fast, but it attacks X and Y
simultaneously, violating Rule 1. The target:

```
Combo[(A, move); (A, shoot, X); (C, move); (C, shoot, X)]; (Sync); (B, move); (B, shoot, Y); (Sync)
```

**[DEV]** B's actions may go before or after the combo — **the tiebreak is
current camera location, chosen to minimise camera movement during the turn.**

**[DEV]** The analyzer buckets attacking abilities by **target and possible camera
framing**. Complications it must handle:

- **Pre-abilities.** C must move before it can shoot, so the move is pulled into
  the combo. If that move already belongs to another combo, combos **nest**.
- **Planning-time validity.** If a push-back moved a player unit, a unit that
  planned *before* that event must execute *before* it, "This guarantees that line
  of sight (LoS) considerations are still valid for the attack."
- **Per-unit order is inviolable.** "When to execute them can change, but never
  the order."
- **Cross-unit pre-abilities.** In Figure 5, C plans to occupy the cover B is
  about to vacate. Reordering C earlier creates a real-world collision that did
  not exist in the WorldState, so B's move becomes a pre-ability of the combo —
  which then splits B's actions and violates Rule 2, so the final answer moves B
  entirely ahead of the combo instead.
- **Cycles.** "though rare, it is not always feasible because of potential
  cycling dependencies of units in different combo moves. In such instances, it is
  necessary to allow splitting of a unit's actions." Rule 2 is a preference, not
  an invariant.

**[DEV]** **Simultaneous Movement** is the second combo type and has a nice
property: only the *destinations* need to be frameable, not the origins, "as the
direction is often enough for the player to understand what is happening." The
camera zooms out on the destination and enemies converge from off-screen —
deliberately manufacturing the feeling of being surrounded.

**[DEV]** **Flavor actions** are appended after combos are formed: a leader points
and shouts "move" before a group relocates. No gameplay effect, free to execute,
purely to sell the coordination that isn't there.

### 7.6 Execution, interruptions, and hiding the planner

**[DEV]** Execution collects elements up to the next Sync, assigns them, waits for
all units to report done, repeats. **Small random delays** are injected so
simultaneous units do not start on the same frame.

**[DEV]** **Interruptions** restart the entire planning process from a fresh
WorldState. Triggered by an ability failing to execute or by player abilities that
modify the world during the enemy turn (overwatch, mines). Why not plan for them:

> "Pre-planning those abilities in the planning phase would be too expensive in an
> environment with free movement and live bullets."

**[DEV]** The subtle part is undo. Units have per-turn rules ("only attack once"),
tracked on the blackboard. Rather than writing the blackboard directly from
behaviour trees, they use **blackboard modifications** recorded in the plan
elements, so an interruption applies the inverse — queue a shot, increment the
shot count; interrupt before execution, decrement it.

**[DEV] The performance story, which is the most honest passage in the chapter.**
Planning is expensive (§2.2 — sample points × raycasts × enemy count). Internal
team screenings of enemy-turn *videos* went well; **players reported unreasonably
long enemy turns**. "Our goal was to make the enemy turn more exciting, but it
seemed that, despite all the effort we put in, we achieved the opposite."

**[DEV]** The fix exploits the WorldState: because the outcome of a plan is known
before it runs, **sub-plans execute while later units are still planning in the
background**. Three findings from tuning the initial planning window:

1. More initial planning time did **not** always produce faster turns, since
   planning overlaps execution.
2. Players reacted positively to simultaneous actions.
3. Players reacted negatively to **downtime** — and downtime made the whole turn
   feel slower, beyond its actual duration. But they *accepted* a pause at the
   start of a turn, because tactics games have trained them that the AI thinks.

**[DEV]** Which produced two rules: start executing as soon as a push-back action
exists in the plan (and never run elements from before and after it together); and
set **a downtime timeout for turn start and a much smaller one mid-turn after an
interruption** — the start-of-turn banner and sound already absorb some of it.

**[inferred]** This is a latency-hiding architecture, and it is the direct
consequence of refusing precomputation in §2.2. The costs are all still there;
they have been moved underneath something the player is watching. Worth noting
the measurement discipline it implies — they tuned the *initial planning window*
as a parameter and found the intuitive answer (more planning time = faster turn)
was wrong.

### 7.7 Tooling, telemetry, and the bug that was a symptom

**[DEV]** "Enemy turn stuck" was a persistent report, and the lesson recorded is
that **"a stuck enemy turn is a symptom, not the disease"** — causes ranged from
abilities that never terminated to content triggering per-frame navmesh rebuilds.
Every such bug landed on the AI team first, regardless of actual owner.

**[DEV]** On rare repros: "even if you see the issue only once out of a hundred
times you have to take care of it. Scaling up player numbers meant that even a one
in hundred chance would translate to a lot of players stuck in enemy turns."

**[DEV]** **UE4's Visual Logger** carried the load — written automatically in
non-shipping builds, **attached to every bug ticket by expectation**, and
scrubable back and forth over a whole session. Logged: started abilities, goal
assignments, per-unit planning results, **random seeds**, environment query
results, behaviour tree executions.

**[DEV]** **Telemetry** with an offline analysis tool across many sessions tracked
per-unit planning time toward a specific goal, unit-planning time, failed and
successful goal assignments, interruption count, and player downtime — keyed by
**game version, map and platform**. "Instead of fixing what we think might go
wrong, we had a tool that drove our decisions about what would have the most
impact on the player."

**[DEV]** And the measurement caveat: absolute numbers were compared **only in
shipping builds**; elsewhere they compared *relationships* — is this unit 3× another,
does this goal always fail on this map.

**[DEV]** **The one headline number in the chapter:** in extreme situations, the
enemy turn was **up to 4.5× faster with combo moves than the same build with
combo moves disabled.**

### 7.8 Reading it

**[inferred]** The architecture inverts normal squad AI. Instead of a coordinator
deciding and units obeying, **units decide selfishly and a scheduler makes the
result legible.** The tactical planner does not make the AI smarter — it makes it
*readable*, and the chapter is unembarrassed that combo moves, commander shouts
and camera framing are there to imply a cooperation that the simulation does not
contain.

**[inferred]** Three properties follow, and they are why the design is good:

- Unit behaviour trees stay simple and independently authorable, because they
  never reason about each other. A new Locust type touches no coordination code.
- All coordination complexity lands in one post-process, on what is really a
  **presentation scheduling** problem rather than a tactical one.
- Because goals are data and abilities are simulate-capable nodes, designers
  extend the system without touching it. "The systemic handling of simultaneous
  actions is utilized automatically with minimal impact during the design of new
  abilities."

**[inferred]** The replan-on-interrupt choice is the same trade this codebase's
`CLAUDE.md` argues for under "measure before you commit": the theoretically
better answer (pre-plan contingencies) lost to the cruder one (discard and redo)
on cost. Note it is *stated* as a cost judgement in the chapter, not assumed.

---

## 8. What is not published

Stated plainly, because the gaps are large and it would be easy to write around
them:

| Unknown | Status |
|---|---|
| How the navmesh survives runtime parcel assembly | A navmesh exists and can rebuild at runtime (§2.2), but the parcel-stitch strategy is never described. Tiled per-parcel bake remains inference. |
| Movement-range computation and its display | Never described. Navmesh-constrained distance field projected as a decal is inference. |
| **How a position is scored** | EQS is confirmed as the mechanism (§2.2), but **no query, weighting or heuristic is published.** The chapter says units "decide for themselves" and leaves it there. |
| LOS / visibility model, fog of war implementation | Never described. Raycasts from sample points are confirmed; the player-facing fog model is not. Its existence is corroborated obliquely by the VRS fog-of-war heuristic (§6.2). |
| Cover-node runtime format | The term is confirmed (Figure 4) and the authoring convention is public (§3.2). The runtime structure is not. |
| Procedural tile-selection algorithm | Only "uses tile metadata" and "semi-procedural generated maps". No constraint model, no adjacency rules published. |
| Planning cost in absolute terms | Telemetry tracked it extensively (§7.7) but **no figure was published** — only the 4.5× combo-move ratio. |

**[inferred]** The shape of the gap is now clear. The chapter documents the
**turn scheduler** exhaustively and treats the **spatial query layer** as
background — it appears only as a cost to be hidden. That is a defensible
editorial choice for a chapter about simultaneity, but it means the single most
interesting question for this project ("what does gridless actually cost, in
numbers") is answered qualitatively and never quantitatively.

**[inferred]** No postmortem, no GDC talk on the gameplay systems, no Digital
Foundry teardown. The rendering and art side are well covered because Splash
Damage and Microsoft had reasons to publish (Substance marketing, DirectX VRS
advocacy, PC-launch tuning). The AI side got one chapter because one lead wrote
it. Nothing else had a sponsor.

---

## 9. Sources

| Source | Tag | What it gives |
|---|---|---|
| [Blocktober: Gears Tactics](https://www.splashdamage.com/news/blocktober-gears-tactics/) — Splash Damage | **[DEV]** | **The single best source.** Metrics (96/48/96/384 u), cover markup convention, connector vs objective tiles, no-grid statement, blockout process. |
| [Game Tech Deep Dive: Gears Tactics — When art meets scalability](https://www.gamedeveloper.com/design/game-tech-deep-dive-i-gears-tactics-i---when-art-meets-scalability) | **[DEV]** | Parcels, PRT-based cross-parcel GI, planar reflections, LOD tiers and biasing. |
| [Iterating on Variable Rate Shading in Gears Tactics](https://devblogs.microsoft.com/directx/gears-tactics-vrs/) — DirectX blog | **[MS]** | Tier 1 VRS: pass list, three heuristics, the 10.3% / 18.9% numbers. |
| [Moving Gears to Tier 2 Variable Rate Shading](https://devblogs.microsoft.com/directx/gears-vrs-tier2/) — DirectX blog | **[MS]** | Tier 2 (Gears 5): Sobel SRI, tile sizes, boundary-skip optimisation, the conservative SRI motivated by Tactics' SSR. |
| [How Splash Damage Built Efficient Material Systems for Gears Tactics](https://80.lv/articles/how-splash-damage-built-efficient-material-systems-for-gears-tactics) | **[DEV]** | Projected procedural puddles, wetness model, fire/scorch material functions, 4-nearest fire cap, PBR and HDR validation view modes. |
| [Destroyed Beauty — Texturing Gears Tactics with Splash Damage](https://magazine.substance3d.com/destroyed-beauty-texturing-gears-tactics-with-splash-damage/) | **[DEV]** | Trims and tileables, 384×384 / 192×384 wall modules, RGB mask layering, the ~80% memory figure and the 64 MB armour breakdown. |
| [Gearing the Tactics Genre: Simultaneous AI Actions in Gears Tactics](http://www.gameaipro.com/GameAIProOnlineEdition2021/GameAIProOnlineEdition2021_Chapter03_Gearing_the_Tactics_Genre_Simultaneous_AI_Actions_in_Gears_Tactics.pdf) — Game AI Pro Online 2021, ch. 3 (PDF), Matthias Siemonsmeier | **[DEV]** | **The primary source, and the best one for this game.** 15 pages. Read in full (§7). WorldState, fuzzy goal assignment with worked data, plan element grammar, the Combo Move Analyzer's constraint set, interruption/undo, the latency-hiding rules, telemetry, and the 4.5× figure. Also the only source that describes the spatial layer at all (§2.2). |
| [Make It Fast: Simultaneous AI Actions in Gears Tactics](https://www.youtube.com/watch?v=rzR-vetCLYA) — Unreal Fest Online 2020 | **[DEV]** | Video of the same content. **Unwatched** — the chapter supersedes it for text, but the talk likely shows the figures animated. |
| [How AI Helps Achieve 'Tactical Clarity' in Gears Tactics](https://www.aiandgames.com/p/how-ai-helps-achieve-tactical-clarity) — AI and Games | **[COMMUNITY]** | Secondary summary. Broadly faithful, but **describes the coordination layer as "partial-order planning" with "threats" — terminology the chapter never uses** (§7.3). Superseded here; kept for the correction. |
| ["We just moved the camera up"](https://www.pcgamesn.com/gears-tactics/developer-interview) — PCGamesN | **[PRESS]** | The Bielman and Grimbley quotes on no-grid, prototyping risk, cover snapping, AP flexibility, overwatch cones. |
| [Real-Time Dynamic Cover System for Unreal Engine 4](https://www.gamedeveloper.com/programming/real-time-dynamic-cover-system-for-unreal-engine-4) | **[DEV]** | Not about Gears — but the definitive write-up of the two cover-generation strategies in §2.3, with the tradeoff argued. |
| [Developer Blog: Tuning Your PC For Tactics](https://www.gearsofwar.com/dev-blog-tuning/) | **[DEV]** | Shipping graphics options and what each does; hardware performance targets. |
| [Cover — The Level Design Book](https://book.leveldesignbook.com/process/combat/cover) | **[COMMUNITY]** | Cover-node lineage: GoW1 and HL2 hand-placed nodes, and the move to navmesh-tied baking. |
| [Inside Unreal: The Coalition on Gears 5 and Gears Tactics](https://forums.unrealengine.com/t/inside-unreal-the-coalition-on-gears-5-and-gears-tactics/146671) | **[DEV]** | July 2020 stream. Agenda covers facial animation, dynamic rigs, lighting, cover, streaming/level setup. **Unwatched — no transcript pulled.** |

**Sourcing status.** The chapter has been read in full, text and figures — pages
rendered with `pdftoppm`, so Figures 1–6 are accounted for and their content is
folded into §7. The two video sources remain unwatched and are judged low-yield:
the Unreal Fest talk is the same material as the chapter, and the Inside Unreal
stream is mostly Gears 5. **Nothing in this note is blocked on them.**

---

## 10. What this means for this project

### 10.1 The coincidence worth noticing first

Gears Tactics' full-cover height and minimum walkable width are both **96 uu**.
This project's tile is `WORLD_StepSize` = **96 uu** (`study/README.md`), inherited
from XCOM 2's own metric.

**[inferred]** Not a coincidence at all — it is roughly two character widths, and
every cover shooter converges on it. But it does mean **Gears Tactics' entire
metric set maps onto this project's grid with no conversion**: their full cover is
one tile edge, half cover is half a tile, their elevation step is 4 tiles, their
wall modules are 4×4 and 2×4 tiles. Their level-design metrics are directly
readable as tile counts, which makes their tile library a usable reference for
authoring content here.

### 10.2 Where we are opposed, and why we are right to be

| | Gears Tactics | This project |
|---|---|---|
| Primitive | Cover volume, continuous position | Cell |
| Cover data | Hand-marked per prefab, authoritative | Derived from `Tile` into `OcclusionGrid`, testable |
| Verification | Playtest, plus a designer watching art dressing | `testOcclusionGrid` checks every bit against source |
| Spatial queries | EQS sample points + on-demand raycasts, navmesh | Array indexing, `O(1)` per cell |
| Cost of a query | Scales with sample count × ray count × enemy count | Scales with cell count, bounded and known |
| Precomputation | **Impossible** — live bullets, stated outright (§2.2) | The entire point |

**[inferred]** The grid is not a limitation we are working around; it is what
makes the performance discipline in `CLAUDE.md` *possible*. The 146× win recorded
there — replacing a per-cell roll call with one array read — has no analogue in a
continuous world. You cannot precompute an occupancy summary over a space with no
cells.

**[inferred] And Gears Tactics is the case study for what that costs.** They did
not get away with it. Planning time became severe enough that players complained
the enemy turn was too slow, and the response was not to make the queries cheaper
— it was to build a **latency-hiding architecture**: overlap planning with
execution, tune an initial planning window, hide the residue behind a banner and
a sound sting. That is a real engineering answer and it worked, but it is
*several systems* spent on a problem a grid does not have. Read §7.6 as the
invoice for §1.

### 10.3 What is worth taking

Six things, in order of value. The top two are new since reading the chapter.

1. **The WorldState, and building it before the AI needs it.** The chapter's own
   parting advice, and the one thing it insists on: a simulate-an-ability-without-
   executing-it facility, owned by the ability system rather than by the AI, so
   *every* system can ask "what would happen if". Any turn-based game wants this
   for AI planning, move preview, undo, and hit-chance display — and it is far
   cheaper to design in than to retrofit, because it is a property of how
   abilities are structured (nodes that can target a simulated state) rather than
   a component you can add later. **This is the single most transferable item in
   the note.** Its companion rule: **commit, then simulate** — never let the
   planner see an outcome before choosing, or it reads as cheating.

2. **Planning order as a coordination primitive.** Units that set up an advantage
   plan *first*, so selfish units planning later simply find a better world
   (§7.4). No negotiation, no shared plan representation, no squad object. The
   cheapest cooperation mechanism in the note and it composes with anything.

3. **The split grid.** Their design insight, orthogonal to the movement model:
   **module for authoring, continuum for play.** The inverse is available to us —
   *grid for play, continuum for presentation*. Movement is already cell-to-cell;
   nothing requires the visible unit to move cell-to-cell or stand on a cell
   centre. Cover magnetism, in reverse.

4. **Fog-of-war-driven shading rate.** Free information, already computed, and
   applicable the moment there is a visibility model and a VRS path. Generalises:
   drive rendering cost from gameplay state, not only from screen-space analysis.

5. **The two in-editor validation view modes** — PBR colour chart and HDR
   intensity check. Cheap, and they are the art-side equivalent of the
   derived-cache tests this project already believes in. The same section of §7.7
   argues the same case for AI: log **random seeds** with everything else, so a
   one-in-a-hundred bug is reproducible rather than merely reported.

6. **World-space projection as the seam rule.** *If a feature must be continuous
   across a boundary, make it a function of world position, not of the asset.*
   This applies to any chunked or streamed world, which includes ours.

### 10.4 What is worth explicitly not taking

**[inferred]** The parcel system itself. Runtime assembly bought Splash Damage
replayability and bought them a bespoke GI system, a bespoke reflection system, a
runtime animation system, and a material set that cannot use vertex colour or
tile-local UVs. That is an enormous bill, justified by a content model — a
procedurally-varied campaign of a fixed enemy roster — that this project does not
have. **Chunked streaming is not the same thing and does not carry this cost**;
the cost is specifically in *never knowing the world before load*.

Their cover markup is also worth not taking, for the reason in §3.2: it has no
source to be tested against.
