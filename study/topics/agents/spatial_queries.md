# Spatial queries — the games that build a grid of tactical facts by casting rays

Deep dive on **tactical position selection**: the family of systems that answer
"where should this agent stand?" by generating candidate points in space and
testing them against the world with raycasts. Killzone 2's baked visibility
table, CryEngine's Tactical Point System, Bulletstorm's Environmental Tactical
Querying and its descendant UE4 EQS, plus the cover-generation bakes that feed
them.

Written as the companion to [`gears_tactics.md`](../../games/strategy/gears_tactics.md), which raised
the question. That note establishes what a gridless tactics game pays for its
spatial queries; this one is the survey of how everyone else has answered the
same problem.

Source tags follow the rest of `study/`. **[DEV]** is a first-party technical
publication by someone who shipped the system. **[DOCS]** is engine
documentation. **[COMMUNITY]** is secondary write-ups. **[inferred]** is our
reading.

> **The one sentence.** Almost nobody ray-marches a grid to build tactical data
> — the exception is **Killzone 2**, which bakes a polar visibility table and
> gets *2,000 waypoints into 32 KB*. Everyone else discovered that the rays are
> the expensive part and spent twenty years learning to **avoid casting them**:
> generate few candidates, filter cheaply first, defer the raycast as late as
> possible, and stop the moment one point passes.

---

## 1. What "ray march grid" resolves to

Three distinct things get called this, and separating them is most of the answer.

| | What it is | Who does it |
|---|---|---|
| **A. Bake a grid of visibility** | Offline, precompute what can see what, store it compactly, query it for free at runtime. | **Killzone 2** (§2). Genuinely rare. |
| **B. Generate points, cast rays at runtime** | Make candidates on demand (grid, ring, navmesh), test each with traces, score, pick the best. | **Everyone**: CryEngine TPS (§3), UE4 EQS (§4), Gears Tactics, FF XV, Brink. |
| **C. Bake cover markup from geometry** | Offline or on-spawn: find where cover exists by raycasting or silhouette analysis, store cover points. | Killzone 3, Crysis 2, the UE4 dynamic cover article (§6). |

**[inferred]** The thing that looks most like "a grid of ray marching" — slice
the world into cells, cast a ray from every cell, keep the results — is
**category A, and it has almost died out.** Not because it does not work, but
because dynamic and destructible environments invalidate the bake, and because
the memory cost of naive pairwise visibility is quadratic. Killzone 2 is the
best-documented survivor and it survived by refusing to store the thing
exactly (§2.2).

Category B won. And the whole engineering history of category B is a story about
making the rays not happen.

---

## 2. Killzone 2 — the one that really is a baked visibility grid

**[DEV]** Remco Straatman, Arjen Beij and William van der Sterren, *Killzone's
AI: Dynamic Procedural Combat Tactics* (GDC 2005). The primary source, and the
most directly relevant document in this note.

### 2.1 The world representation

**[DEV]** Two things, and the paper is explicit that this is the minimum:

> - navigation info supporting position enumeration and connectivity queries
> - visibility info supporting line-of-fire (LoF) and line-of-sight (LoS) queries

**[DEV]** Killzone is **waypoint-based**: waypoints roughly every **2 metres**,
placed closer together near cover, with a connectivity graph whose links are
annotated by **estimated travel time**. The paper notes the navigation system
could equally be mesh- or cell-based — "as long as its granularity matches the
size of the individual cover and attack positions."

**[inferred]** That last clause is the real design rule and it is engine-agnostic.
The spatial resolution of your navigation structure has to match the spatial
resolution of the *tactical decisions* you want to express. Two metres is not
arbitrary — it is roughly "one guy behind one piece of cover".

**[DEV]** Two extras ride on the graph: units **claim** destination positions so
others avoid picking or pathing through them, and **danger zones** mark observed
threats like an incoming grenade.

### 2.2 The worst-case visibility lookup table — the centrepiece

The problem: characters have multiple stances (stand, crouch), geometry is
complex, and the tactics depend entirely on "can A see B". Full pairwise
visibility over *n* waypoints is O(n²) and unaffordable.

**[DEV]** Killzone's answer stores, for each waypoint `w`, each stance `s`, and
each **polar direction** `d`:

> the largest distance for which an AI actor in stance `s` at or near waypoint
> `w` does **not** have cover from an attacker in some stance at or near some
> waypoint positioned in direction `d` relative from `w`.

**[DEV]** Figure 5 shows eight polar sectors per waypoint, each holding one
distance. Queries become table comparisons:

```
w_a may have line-of-fire to w_t   if  table(w_a, d) ≥ 7  and  table(w_t, d") ≥ 7
w_t has cover from w_a             if  table(w_t, d") < 7  or  table(w_a, d) < 7
```

**[DEV] The memory result, which is the headline:**

> "The visibility table's polar representation results in a memory consumption
> that is **linear in the number of waypoints**. For **2,000 waypoints**,
> Killzone's visibility look-up table uses a mere **32 Kbyte**."

**[inferred]** 16 bytes per waypoint. The quadratic problem was not solved, it
was **refused** — by storing a per-waypoint directional summary instead of a
pairwise relation. That is a lossy projection of the visibility function onto a
much smaller basis, and §2.3 is why the loss is acceptable.

### 2.3 Why the approximation is safe — and why it matches this project's rules

**[DEV]** The table is deliberately **conservative in one direction**. It records
the *worst case*: if a single position at that distance and direction could
establish a line of fire, the whole sector is assumed to offer no cover. The
paper states the two consequences precisely:

> "although our table is inaccurate in its representation of line-of-fires, the
> AI can **fully rely on a position offering cover when the table says so**.
> Solely statements about a position *not* offering cover (thus having a
> line-of-fire) may be too optimistic and require **verification with a ray
> cast**."

**[DEV]** And why that asymmetry is the right one:

> "in position picking and path-finding, the AI has a far greater need for
> **reliable positive statements about cover** than about lines-of-fire."

**[DEV]** A second benefit falls out free: robustness to threat movement and
stance changes. If a threat could easily establish a line of fire *from a nearby
position*, the table already says there is no guaranteed cover — "a limited
ability to anticipate threat movement and stance changes."

**[inferred] This is the derived-cache escape-hatch pattern from this project's
`CLAUDE.md`, arrived at independently in 2005.** Compare directly:

| `CLAUDE.md` rule | Killzone's version |
|---|---|
| "The fast path may only skip work that provably does nothing. It never decides anything the slow path would have decided differently." | The table is exact where it says *cover*. Only *no cover* is approximate. |
| "The slow path is the original code, unmodified. Complicated cells set a `kNeedsTile`-style bit and fall through to it." | The optimistic answer falls through to **a real raycast** for verification. |
| Test derived data against its source | (not addressed in the paper — the one gap) |

**[inferred]** The generalisable idea is sharper than either statement of it:
**a conservative approximation is only useful if its error points the way your
caller needs.** Killzone's table is wrong in exactly the direction that costs a
raycast rather than a bad decision. An approximation that erred the other way —
claiming cover that isn't there — would be worthless at any memory saving,
because there is no cheap way to detect the error.

### 2.4 Position evaluation

**[DEV]** Candidate positions are the waypoints within a radius of the agent (or
of the squad's area-of-operations), minus those claimed by others or outside the
area. Each surviving waypoint is scored by a **weighted sum of inputs**, sorted,
and the top one is post-checked for suitability.

**[DEV]** Table 1 of the paper lists the inputs actually shipped:

| Input | Higher score for… |
|---|---|
| Proximity to current position | being closer to / more quickly reached from current position |
| Proximity from specific location | being closer to / more quickly reached from a specific location |
| Cover from primary threat | offering cover from the primary threat (given a stance) |
| Line-of-fire to primary threat | offering line-of-fire to the primary threat (given a stance) |
| Distance to primary threat | being at preferred fighting distance |
| Outside danger zone | being outside the blast range of an expected projectile |
| Cover from secondary threats | cover from one or more threats other than the primary |
| Outside friendly line-of-fire | being outside the LoF of specified friendly units |
| Distance from friendly positions | being some distance from friendly positions |
| Wall hugging | being close to a wall or obstacle in a specified direction |
| Nearby cover | being *near* positions offering cover from the primary threat |
| Player line-of-fire | not crossing the player's line-of-fire to get there |
| Preferred fighting range | being inside preferred fighting range |

**[DEV]** One scoring rule, stated as a hard constraint:

> "To prevent inputs from canceling out each other, we solely use **non-negative
> valued weights and inputs**. We also make sure all inputs reflect the
> *attractiveness* of a position. For example, we use **safety rather than
> danger, proximity rather than distance**."

**[inferred]** Worth pausing on, because Crytek made the opposite choice (§3.5:
normalised [0,1] values × signed multipliers) and both shipped. Killzone's rule
buys monotonicity — adding an input can never make a good position score worse
— which makes tuning predictable and makes the post-check in §2.5 sound. Crytek's
buys expressiveness at the cost of needing the balancing discipline in §3.5.
**Killzone's is the better default**, and the trick is entirely in the
*naming*: phrase every input as a virtue and the sign problem disappears.

**[DEV]** Figure 3 is the canonical illustration and worth reproducing mentally:
the same waypoint field annotated five times — proximity, line-of-fire to
primary threat, cover from secondary threats, preferred fighting range — then
summed, and the highest total wins. Every layer is a scalar field over the same
point set.

### 2.5 Two design rules that generalise

**[DEV] The post-check.** The highest-scoring position is verified to be
*suitable*, not merely best. "when picking a cover in the center of a football
field, even the highest scoring position will not actually provide cover." The
check works only if the cover input is weighted more strongly than all other
inputs combined — then a low top score is itself the signal, and the AI may
abort its search for cover.

**[DEV] Evaluation range must match the decision cycle.** Too small a range and
the AI forgoes good nearby positions and fights from bad ones; too large and it
strings together a series of movements toward promising locations without ever
arriving, spends its turn manoeuvring rather than firing, and costs more CPU.
The rule: *the travel duration corresponding to the evaluation range should be
on the order of the AI's decision cycle.*

**[inferred]** The second is the more useful and the less obvious. It says the
spatial parameter and the temporal parameter of an AI are **not independent**,
and tuning one without the other produces a characteristic failure — the agent
that is always walking somewhere and never doing anything.

### 2.6 Suppression fire, as an example of reusing the machinery

**[DEV]** Figure 7 is a nice demonstration that the query system is general.
To suppress a threat that is *hidden*, Killzone selects waypoints near the
threat's last known position, annotates them for "would offer the threat a LoF
to me" and "offers the threat cover from me", takes everything scoring ≥ 40,
**merges targets that overlap in yaw and pitch**, and fires at the resulting
handful of points — bracketing the hidden enemy.

**[inferred]** The AI does not know where the enemy is, so it shoots at the
*set of places the enemy could usefully be*. Same point database, same scoring,
completely different behaviour, no new system.

---

## 3. CryEngine's Tactical Point System — the runtime query architecture

**[DEV]** Matthew Jack, *Tactical Position Selection: An Architecture and Query
Language*, Game AI Pro ch. 26. Describes the TPS used in **Crysis 2**, shipped
publicly in the CryEngine Free SDK.

### 3.1 The shape

Behaviour tree picks a **context** (behaviour × environment) → context selects a
**query** from a library → query runs **Generation → Filtering → Weighting →
Results**, timesliced by a scheduler over a point database.

**[DEV]** The core problem, stated as the designer's question you will be asked:
*"Why did he move here, when it's just common sense that he should move there
instead?"*

### 3.2 The query language

**[DEV]** A DSL built on Lua tables, parsed to a bytecode-like form so **no Lua
runs at runtime**. Keywords joined by underscores, with discarded "glue" words
(`from`, `to`, `the`) purely for readability. A real shipped query:

```lua
Query_CoverCompromised_FindNearby =
{
    {   -- Option 1: hidespot from our target, a short distance away
        Generation = {hidespots_from_target_around_puppet = 15},
        Conditions = {min_distance_from_puppet = 5,
                      canReachBefore_the_target = true},
        Weights    = {softCover = -10, distance_from_puppet = -1.0},
    },
    {   -- Option 2 (fallback): move away, prefer anything that blocks LOS
        Generation = {grid_around_puppet = 10},
        Conditions = {min_distance_from_puppet = 5,
                      max_directness_from_target = 0.1},
        Weights    = {visible_from_target = -10,
                      distance_from_puppet = -1.0}
    }
}
```

**[DEV]** `min`/`max` prefixes turn a scalar criterion into a Boolean, so the
*same* criterion can serve as a weight or a condition. Objects (`puppet`,
`target`, `player`, `leader`, `referencePoint`, `squadCenter`) let one criterion
apply to many subjects. **This is the whole reason the language is small.**

**[DEV]** Note `grid_around_puppet = 10` in the fallback — "we generate the
candidate positions in a 10 m square grid around the agent." **There is the ray
grid, and it is the fallback, not the primary path.**

### 3.3 Where the points come from

**[DEV]** A survey worth having, because it is the whole design space:

| Game | Cover point source |
|---|---|
| **Far Cry, Crysis** | Designer-placed **"hide anchors"** — a position *plus a directional cone* from which cover is provided. Tree hidespots generated on demand **on the opposite side from the hide-target**, refreshed per request so the agent orbits the obstacle as the target moves. |
| **Crysis 2** | Automatic generation from **designer hints**. The cover system **maps the silhouette of cover objects and remaps them on destruction** — allowing occlusion tests against that geometry **without raycasts**. Also **cover rails**: cover stored as a *path* rather than discrete points, with locations generated on it on demand (nearest point to agent, or spaced away from a squadmate already on the rail). |
| **Killzone 3** | Hidespots **pregenerated by an automatic process** (Mononen 2011). |
| **Brink** | **No static database at all.** All cover found dynamically, generating points in **concentric rings**. |

**[inferred]** The Crysis 2 silhouette trick is the most interesting entry and
the closest thing in this note to a genuine alternative to raycasting. Storing a
cover object's silhouette turns "am I hidden from that direction" into a
geometric test against a small local representation, instead of a physics trace
through the whole world — and because the silhouette is *remapped on
destruction*, it survives a destructible environment that would invalidate a
Killzone-style bake.

### 3.4 Directness — one criterion that does four jobs

**[DEV]** Distance-to-goal is a poor weight for cover-to-cover advances: it
pushes the agent to close as much distance as possible when what you want is a
*succession of good waypoints*. Instead:

```
                distance(agent → goal) − distance(point → goal)
directness =    ──────────────────────────────────────────────
                        distance(agent → point)
```

**[DEV]** Predictable and distance-independent: the closer to a straight line
toward the goal, the higher the score. And it yields four behaviours from one
criterion:

| Want | Specification |
|---|---|
| Advance | `min_directness_to_referencePoint = 0.5` — at least 5 m closer per 10 m travelled |
| Retreat | `max_directness_to_referencePoint = -0.5` |
| **Flank** | constrain directness to **≈ 0**, e.g. `[-0.1, 0.1]` — move without progressing |
| **Zigzag** | `min_directness = 0.5` as a *condition* **and** `directness = -1.0` as a *weight* — "as indirect as it can be, while always making minimum progress" |

**[DEV]** The zigzag case shipped, and the paper notes irregular tree placement
and player movement supply the randomness for free.

**[inferred]** The pattern to steal is not the formula, it is the shape: **a
single normalised, sign-carrying scalar that means "progress", used as both a
gate and a preference.** Most tactical intent decomposes into progress plus
something else.

### 3.5 Best practice: prefer conditions to weights

**[DEV]** Stated flatly, and it is the opposite of what most people build:

> "when we want to flank left, rather than weighting all points according to
> 'leftness', just **invalidate** any point that is not left of your current
> position; rather than tweaking a tradeoff between hard/soft cover and
> distance, write an option that only considers hard cover, then a **fallback**
> that only considers soft."

**[DEV]** "Keeping the number of weights down—**ideally to one or two**—results
in queries with predictable results for given contexts and saves a lot of tuning
time." Weights are normalised to [0,1] and multiplied by signed user multipliers
of any magnitude; **distance weights are clamped to a chosen maximum useful
range** (say 30 m) so scores stay comparable at unusual distances.

**[DEV] `canReachBefore`** — discard any point closer to the enemy than to us.
"so pervasive to good behavior that in the Crysis 1 hiding system it was not
optional." Far Cry and Crysis additionally exploited perfect knowledge of every
agent's claimed hidespot, never double-booking.

**[DEV] Query failure is information.** A failed flank-left query does not just
mean "retry" — it means *there is no cover to our left*, which the behaviour tree
should act on, or a squad should use to abort a group manoeuvre. TPS queries can
name a signal to send or blackboard state to set on failure.

### 3.6 Performance — the section everyone should read

**[DEV]** The blunt statement:

> "Raycasts … In many games, they are **the dominant cost of position selection
> or of the whole AI system**."

**[DEV]** A physics raycast "will generally traverse a large number of memory
locations"; done synchronously it costs cache misses, cache thrashing and
possible physics-thread synchronisation — "on consoles this is quite
prohibitive." TPS therefore prefers an **asynchronous API** even if only to
batch raycasts.

**[DEV] The evaluation order rule:**

> 1. **Cheap filters first**, to discard points early
> 2. **Weights**, to allow us to sort into order
> 3. **Expensive filters**, evaluating **from highest scoring down, until a
>    point passes**

**[inferred]** Step 3 is the important one and it is easy to miss. You do not
evaluate the expensive filter on all survivors — you evaluate it on the *best*
survivor, and if it passes you are done. In the common case that is **one
raycast for the whole query.**

**[DEV] The heap trick, for expensive *weights*.** Step 3 handles expensive
conditions but not expensive weights (relative exposure to several threats,
true path distance). The observation: you rarely need a point's final score to
know it beats the others. So keep per-point metadata —

```c
struct PointMetadata { TPSPoint point; int evalStep; float minScore, maxScore; };
```

— evaluate all *cheap* criteria first, seed `minScore`/`maxScore` from the sum
of negative and positive user multipliers, build a **binary heap** ordered by
`maxScore`, and repeatedly refine the top element. Each weight evaluation
narrows that point's `[min, max]` window. When the top point's `minScore`
exceeds the `maxScore` of **both its heap children** — which, by the heap
property, bounds every other point — its remaining weights can be **skipped
entirely**.

**[inferred]** A branch-and-bound over utility, and a genuinely good idea. Note
the honest caveat the chapter gives: in the worst case (no valid point) you still
evaluate everything, and the heap overhead only pays when criteria are genuinely
expensive — which is why cheap criteria are handled *before* the heap exists.

**[DEV]** The same heap is the timeslicing and async unit: evaluate one criterion,
check the clock, and either continue or park the heap until next frame. An async
raycast parks it the same way, and resumes with the result waiting. Crysis 2's
scheduler was first-come-first-served on a single query at a time; the chapter
notes sharing time between agents would be better.

### 3.7 Squad coherence, almost free

**[DEV]** Rather than weighting distance-to-squad-centre, **centre the generation
on the squad centre** and set the radius:

```lua
Generation = {hidespots_from_target_around_squadCenter = 10}
```

**[DEV]** The emergent result is worth the whole section: members at the front
find no valid points (the only ones progressing toward the goal are outside the
squad-centre radius), so they wait; laggards catch up; the centre moves forward;
the front unblocks. **"a loose leapfrogging behavior without any explicit
consideration of the relative position of squad members."** An individual can
also ignore coherence temporarily — chasing ammo, dodging a grenade — and still
contributes to the centre, so the squad cannot leave without them.

**[inferred]** Same trick as Gears Tactics' plan-order coordination
([`gears_tactics.md`](../../games/strategy/gears_tactics.md) §7.4): coordination expressed as a
*constraint on where you may look*, not as a negotiation. Both are cheap because
neither represents the group plan anywhere.

### 3.8 Debug rendering, which the chapter treats as non-optional

**[DEV]** A sphere per candidate, coloured **white** (winner), **green** (passed
all conditions), **red** (failed a condition), **blue** (only partially
evaluated), with the final score drawn above it, and the parsed query echoed to
the log. Raycast criteria draw their rays. Grading spheres by *relative* score
was tried and abandoned — the differences were too small to judge by eye.

**[DEV]** Bulletstorm went one better: every invalid point annotated with **the
name of the condition that rejected it**.

---

## 4. Bulletstorm → UE4 EQS — the descendant nearly everyone now uses

**[DEV]** Per Eric Johnson's chapter: Bulletstorm's **Environmental Tactical
Querying** system (People Can Fly, described by Zielinski) "is now integrated
into Unreal Engine 4 as the **Environment Query System (EQS)**", and Crysis 2's
TPS shipped publicly in CryEngine — "making these techniques accessible to a
massive audience."

**[DOCS]** The UE4/UE5 vocabulary maps one-to-one onto TPS:

| EQS | TPS equivalent |
|---|---|
| **Generator** (Points: Grid, Donut, Circle, Cone, Pathing Grid, Actors of Class) | Generation criterion |
| **Item** | Candidate point |
| **Context** (querier, target, custom) | Object (`puppet`, `target`, …) |
| **Test** (Distance, Dot, Trace, Overlap, Pathfinding) | Condition or Weight |
| Test as **filter** vs **score** | Conditions vs Weights |

**[DOCS]** The **Points: Grid** generator makes a grid of items around the
querier, parameterised by extent and spacing. The **Trace** test fires a trace
from the item to (or from) a context and keeps or scores by the result —
"the engine fires a raycast from the AI's eyes to every single generated point",
and setting the Boolean match to false requires the trace to *miss*, discarding
points behind walls.

**[inferred]** That documentation phrasing — a raycast to *every* generated point
— is exactly the naive implementation both primary sources spend their
performance sections telling you to avoid. EQS does support test ordering and
cheap-first filtering, but the default a newcomer builds is the O(points)
raycast sweep. **This is the "ray march grid" the original question was
describing, and it is best understood as the thing shipped systems are designed
to avoid doing.**

**[DEV]** Other implementations of the same idea, both cited by Johnson: **FINAL
FANTASY XV**'s *Point Query System* (Shirakami et al., CEDEC 2015) and **MASA
LIFE**'s **SQL-based SpatialDB** (Mars, GDC 2014).

---

## 5. Johnson's practical guide — the failure modes

**[DEV]** Eric Johnson, *Guide to Effective Auto-Generated Spatial Queries*,
Game AI Pro 3 ch. 26. Where the other two describe architecture, this one
describes what goes wrong.

### 5.1 Do not raycast the floor to find the floor

**[DEV]** On generating a grid of points on walkable ground:

> "Although it is possible to use **collision raycasts against level geometry** to
> map out the level floor, this is not only **computationally expensive**, but the
> generated points **may not be reachable** by the agent (e.g., if they lie on a
> steep slope or narrow corridor). By sampling along the surface of the
> **navigation mesh** instead of the actual level geometry, we can both reduce
> generation cost and ensure that the sample position is reachable."

**[inferred]** A direct rejection of the down-raycast grid, on two independent
grounds — and the second is the one that actually kills it. A raycast tells you
where the *floor* is. It does not tell you where the *agent can stand*, and those
are different sets. The navmesh already encodes the difference.

**[DEV]** Two ways to project a grid onto the navmesh, with a real tradeoff:

| | How | Cost |
|---|---|---|
| **One-to-one** | Nearest navmesh point per (x,y), via Recast/Detour's bounded search | Efficient, but **only finds one level** — bridges and multi-storey buildings lose all but one floor |
| **One-to-many** | Vertical **navigation raycast** per (x,y), emitting a hit per navmesh polygon passed through | Handles multi-level terrain, costs more |

**[DEV]** And a choice about which polygons to gather first: a **bounding box**
around the origin is simple but can yield points that are far or unreachable by
path; **path distance** guarantees reachability but discards spatially-near
locations that are topologically distant. Bounding box suits line-of-sight
behaviours (ranged attacks); path distance suits spatial ones (following,
surrounding). Or gather at `2r` by path and then reject beyond `r` linearly.

### 5.2 Grid bias — the failure mode most relevant to this project

**[DEV]** Consider "get as close to the target as possible, but stay 3 m away."
On a grid, generate points, discard those under 3 m, rank the rest by distance.
The problem:

> "Depending on the desired radius, the closest points to the target invariably
> lie either **on the diagonal or cardinal directions**. As a result, agents not
> only **cluster around four points**, they may also approach the target at an
> unnatural angle to do so — that is, instead of moving directly toward the
> target to a point that is 3 m away, they will **veer to one side or the other**
> to get to one of the 'optimal' points found by the search."

**[DEV]** Also grossly inefficient: most grid samples are either inside the
rejection radius or too far to ever win. The fix is a **ring generator** — all
candidates equidistant by construction, bias gone, and a fraction of the samples
needed. Johnson reports this was "by far the most common" query category on their
project, and switching it to rings both improved the behaviour and freed budget
for richer tests.

**[inferred] This one transfers to us directly and is a real hazard.** Any
scoring pass over a square cell grid inherits cardinal/diagonal anisotropy, and
it shows up wherever a *radius* meets a *lattice* — ability ranges, threat
avoidance, "stand near but not adjacent". The rendering-side symptom is a range
indicator that looks like a diamond or a square; the gameplay symptom is units
that converge on eight preferred angles. **Being on a grid does not exempt us —
it is precisely the condition that causes it.** The remedy is the same:
enumerate candidates in the shape the *intent* has, not the shape the storage
has.

**[inferred] And it generalises further than AI queries — it is a property of
per-cell enumeration itself.** [`elite_dangerous.md`](../../games/space/elite_dangerous.md) §3.6
records the second shipped instance: Elite Dangerous' *Horizons* planet
generator scattered impact craters per terrain patch and produced a **visibly
regular grid of craters** across whole worlds. Same cause, entirely different
domain, and a scale eleven orders of magnitude larger. The aggregate always
shows the lattice even when each individual sample looks fine, which is what
makes this class of bug survive review — nobody inspects the aggregate until it
ships.

### 5.3 Two tests do most of the work

**[DEV]** "by only mixing and matching the two most versatile tests in a query
system's toolkit, **distance and dot product**, we can support a surprisingly
wide range of tasks."

**[DEV]** Dot product recipes, all from the same test with different subjects:

| Vectors compared | Meaning |
|---|---|
| (agent→sample) · (agent forward) | in front of / behind me |
| (agent→sample) · (agent right) | to my left / right |
| both of the above together | a diagonal heading |
| against **world** forward/right | cardinal / ordinal directions |
| (agent→sample) · (sample→target) | **between me and the target**, ranked by directness |
| same, **sine-scored** | ranked by *indirectness* → curved, flanking approach |
| first vector flipped | retreat, ranked by directness *away* |
| (camera→sample) · (camera forward) | near the centre of the screen |

**[DEV]** With multiple test subjects, the aggregation choice matters:
**minimum** distance gives local attraction/avoidance around each subject;
**average** distance gives the centroid, useful for team cohesion.

**[DEV]** A single cheap win: a minimum-distance test weighted against both ally
*locations* **and ally *destinations*** "can eliminate most location contention
without the need to implement specific countermeasures such as point reservation
systems."

### 5.4 Scoring functions

**[DEV]** After normalising, pass the score through a curve:

| Curve | Effect |
|---|---|
| **Linear** | as-is; the backbone |
| **Square** | strongly de-emphasises all but the highest-ranked |
| **Square root** | over-emphasises all but the lowest-ranked |
| **Sine** | emphasises the **mid-range**, de-emphasising both extremes |

**[DEV]** Sine is the interesting one because it repurposes a test rather than
tuning it: on a distance test with min and max range it defines an **ideal
radius** (their average) while still accepting nearer and farther positions; on
the (agent→sample)·(sample→target) dot product it produces a **circle between
agent and target** — a roundabout approach.

**[DEV]** The chapter's argument for curves over hard conditions is the best
thing in it. Invalidating everything within 2 m of another actor gives a
"predictable and artificial" response. A negatively-weighted **square**-scored
distance test gives an agent that ignores others until they get close, then
shifts slightly; **square root** gives a nervous agent that flees distant
company. And because it is a preference rather than a rule, it adapts: the agent
"will naturally maintain a polite distance from other passengers, but will
gradually permit that distance to shrink as it becomes packed at rush hour."

### 5.5 Continuous queries — behaviours with no behaviour code

**[DEV]** Most queries run once per behaviour, with a cheap periodic *validation*
of the chosen destination en route. But re-running the **whole query** on a loop,
in parallel with the move, turns the query itself into the behaviour. Orbiting,
surrounding, zigzag approaches and random walks become **pure data**.

**[DEV]** The shipped example tables are worth having as concrete calibration:

```
Orbit — moves in a circle around a target, avoiding others
  Ring generator          5–9 m around target
  weight 8   Distance     relative to agent            Sine, 0–6 m
  weight 4   Dot          (Agent→Sample)·(Agent→Destination)   Linear
  weight 2   Dot          (Agent→Sample)·(Agent→Target)        Sine
  weight 1   MinDistance  vs other agents              Sigmoid, 0–5 m
  weight 1   MinDistance  vs other agents' destinations Sigmoid, 0–5 m
```

**[DEV]** The sine-scored distance sets an ideal step length — far enough not to
arrive before the next query, close enough to keep the heading smooth. The
tangent dot product makes the agent join the ring *along the tangent* rather
than head-on-then-turn, and lets it **reverse direction when blocked**.

**[DEV]** **Boids as a query**: separation = minimum-distance test vs other
agents; alignment = dot product vs their average heading; cohesion = distance
test vs their centroid. Reynolds' three rules, expressed as three tests.

**[DEV]** The caveats are stated: continuous querying is expensive, and it is
harder to avoid local minima, oscillation between destinations, and stop-and-go
movement from picking destinations too close to the current position.

### 5.6 Failure resistance

**[DEV]** Three levers, and a nice optimisation hiding in the third:

- **Permissiveness** — admit worse points but rank them low, so they win only
  when nothing better exists.
- **Robustness** — widen what counts as ideal (5–8 m instead of 5 m; 45° instead
  of 30°).
- **Fallback options** — and here: define a **narrow** initial sample set so you
  can afford **expensive tests like collision raycasts** in the primary option,
  then a **wide** fallback with those tests removed for a mediocre-but-acceptable
  answer. You pay the extra cost only when conditions are bad.

---

## 6. Cover generation — the bake, where a ray grid does appear

Covered in [`gears_tactics.md`](../../games/strategy/gears_tactics.md) §2.3; summarised here for
completeness because this is where category **C** lives.

**[DEV]** From the UE4 dynamic cover write-up, two strategies:

| | Mechanism | Verdict |
|---|---|---|
| **3D object scan** | Slice an actor's bounding box into an X/Y/Z **grid**; **raycast downward** from every point to find where the perimeter meets the ground; filter on ground gap and cover height; project survivors to the navmesh. | Thousands of points per object; cost scales with bounding-box size; poor on landscape. Uniform distribution, near-zero error, and the **only** option for objects with no collision geometry (force fields). |
| **Navmesh edge-walking** | Cast perpendicular to each navmesh edge in both directions; with ledge detection and slope tolerance, up to 8 rays per vertex. | "Considerably faster"; object size irrelevant; handles rugged terrain and multi-storey. Error-prone at navmesh tile boundaries. Recommended for ~90% of needs. |

**[DEV]** Modern practice ties the bake to **navmesh tile update events**, so
cover regenerates only in tiles that changed. **[COMMUNITY]** The lineage runs
from hand-placed cover nodes (Half-Life 2, Gears of War 1) through Crysis 2's
designer-hinted automatic generation to fully dynamic systems.

**[inferred]** Both are bakes — run offline or on spawn, never per frame. The
grid-scan variant is the closest thing in shipped practice to "a grid of ray
marching", and even its advocate recommends it only for the cases edge-walking
cannot see.

---

## 7. Recast — the voxel grid you are already using

**[COMMUNITY]** Worth naming because it is the most widely-deployed
grid-built-from-geometry in games, and almost nobody thinks of it as one.

Recast (Mikko Mononen, 2009) builds a navmesh by **rasterising** input triangles
into a **multi-layer heightfield** — a 2D array on XZ where each cell holds a
list of **spans** on Y, each span a contiguous run of walkable or non-walkable
voxels. Filters prune where a character could not fit or stand; the voxel mould
is partitioned into regions; regions are peeled off as polygons.

**[inferred]** Rasterisation, not ray casting — triangles are scan-converted into
the grid rather than the grid probing outward for triangles — which makes it
cheaper by roughly the ratio of triangles to cells. But it is exactly the
"discretise the world into a grid, derive gameplay facts, throw the grid away"
shape the question was asking about, it is under nearly every UE4/Unity game
shipped since 2010, and it is the *source* of the navmesh that §5.1 says to
sample instead of raycasting. Worth knowing that the thing you sample was itself
built from a voxel grid.

---

## 8. What everyone agrees on

**[inferred]** Across three independent primary sources spanning 2005–2017, the
same conclusions recur. This is the actual answer to the question:

1. **Rays are the budget.** Jack: "in many games, they are the dominant cost of
   position selection or of the whole AI system." Killzone built a 32 KB table
   to avoid them. Gears Tactics could not, and built a latency-hiding
   architecture instead.
2. **Never cast a ray you can avoid.** Cheap filters first; raycasts last;
   evaluate top-scoring-down and stop on first pass; keep expensive tests in the
   narrow primary option and drop them in the wide fallback.
3. **Generate in the shape of the question.** Rings for radial intent, not grids.
   The generator is a bigger quality lever than the tests.
4. **Sample the navmesh, not the geometry.** Raycasting the floor is both slower
   and wrong — it finds where the floor is, not where an agent may stand.
5. **Approximate in the direction that is safe to be wrong in**, and fall through
   to the exact test only for the unsafe direction (Killzone §2.3).
6. **Prefer conditions to weights**, and keep weights to one or two. Both
   architecture chapters say this independently.
7. **Failure is a signal**, not an error. A flank query returning nothing means
   there is no flank.
8. **Build the debug view first.** Both chapters devote real space to coloured
   spheres and per-point rejection reasons.

---

## 9. Sources

| Source | Tag | What it gives |
|---|---|---|
| [Killzone's AI: Dynamic Procedural Combat Tactics](http://cse.unl.edu/~choueiry/Documents/straatman_remco_killzone_ai.pdf) — Straatman, Beij, van der Sterren, GDC 2005 (PDF) | **[DEV]** | **The primary source for a genuinely baked visibility grid.** Waypoint graph at ~2 m, the polar worst-case visibility LUT, the 32 KB / 2,000 waypoint figure, the conservatism argument, the 13-input position evaluation table, evaluation-range-vs-decision-cycle, suppression fire. *Note: the PDF has no Unicode mapping — text extraction returns garbage; it must be read as rendered pages.* |
| [Tactical Position Selection: An Architecture and Query Language](https://www.gameaipro.com/GameAIPro/GameAIPro_Chapter26_Tactical_Position_Selection.pdf) — Matthew Jack, Game AI Pro ch. 26 (PDF) | **[DEV]** | **The architecture reference.** CryEngine TPS as shipped in Crysis 2: the Lua-derived DSL, generation sources across Far Cry / Crysis / Crysis 2 / Killzone 3 / Brink, directness, the evaluation-order rule, the binary-heap branch-and-bound, timeslicing and async raycasts, squad-centre coherence, debug rendering. |
| [Guide to Effective Auto-Generated Spatial Queries](http://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter26_Guide_to_Effective_Auto-Generated_Spatial_Queries.pdf) — Eric Johnson, Game AI Pro 3 ch. 26 (PDF) | **[DEV]** | **The practical guide, and the best on failure modes.** Navmesh sampling vs raycasting the floor, one-to-one vs one-to-many projection, **grid distance bias and the ring fix**, distance+dot recipes, scoring curves, continuous queries (orbit/boids/random walk with real tables), permissiveness vs robustness vs fallbacks. |
| [EQS Node Reference: Generators](https://dev.epicgames.com/documentation/unreal-engine/eqs-node-reference-generators-in-unreal-engine) and [Tests](https://dev.epicgames.com/documentation/unreal-engine/eqs-node-reference-tests-in-unreal-engine) — Epic | **[DOCS]** | The shipping vocabulary of the system most readers will actually use: grid/donut/circle generators, trace tests. |
| [Real-Time Dynamic Cover System for Unreal Engine 4](https://www.gamedeveloper.com/programming/real-time-dynamic-cover-system-for-unreal-engine-4) | **[DEV]** | The two cover-generation bakes (§6), with the tradeoff argued. |
| [Recast Navigation — Introduction](https://recastnav.com/md_Docs_2__1__Introduction.html) | **[COMMUNITY]** | The heightfield/voxelisation pipeline (§7). |
| [Cover — The Level Design Book](https://book.leveldesignbook.com/process/combat/cover) | **[COMMUNITY]** | Cover-node lineage from hand-placed to navmesh-tied baking. |

**Cited but not read.** Zielinski, *Asking the Environment Smart Questions*
(Game AI Pro ch. 33) — the Bulletstorm ETQ chapter, and the direct ancestor of
EQS. Mononen, *Automatic Annotations in Killzone 3 and Beyond* (Paris Game/AI
2011) — how Killzone 3's hidespots were generated. Shirakami et al., FF XV's
Point Query System (CEDEC 2015). Straatman et al., *Dynamic Tactical Position
Evaluation* (AI Game Programming Wisdom 3). van der Sterren, *Terrain Reasoning
for 3D Action Games* (Game Programming Gems 2). **The first two are the
highest-value unspent leads.**

---

## 10. What this means for this project

### 10.1 The direct hit: Killzone's table is our derived-cache pattern

**[inferred]** §2.3 is the single most valuable thing in this note. Killzone
independently derived the escape-hatch rule this codebase already follows, and
its statement of the rule is *better* than ours because it names the asymmetry
explicitly: **be exact in the direction the caller must trust, approximate in the
direction that is cheap to verify.**

`OcclusionGrid` should be read against that. Its `kNeedsTile` bit is the same
mechanism — a marker that says "the summary cannot decide this, go ask the
tiles." Killzone's refinement is that the *whole table* is a one-sided
approximation by construction, rather than an exact summary with an escape hatch
for hard cases. Both are valid; the one-sided version buys much more compression
(16 bytes per waypoint) at the cost of raycasting whenever the answer is
"exposed."

**[inferred]** The one thing Killzone does *not* have, and we do, is
`testOcclusionGrid` — verification of the derived structure against its source.
Their paper never mentions testing the table. That is the gap, and it is worth
noting that our discipline is stricter than a shipped AAA system's.

### 10.2 The direct hazard: grid bias

**[inferred]** §5.2 is a live risk here and worth acting on before it appears. We
*are* a square lattice, so any radial intent scored over cells — ability range,
"near but not adjacent", threat avoidance, spread-out-from-allies — will
concentrate on cardinal and diagonal directions. Johnson's fix is not "use a
better distance metric", it is **enumerate candidates in the shape of the
intent**: for a radial question, walk a ring of cells, not a bounding square.
Cheaper *and* unbiased, which is the rare case where the fix has no downside.

### 10.3 What is worth taking

1. **One-sided approximation with a verify path** (§2.3). The pattern, stated
   properly: pick which way the error must point, make the cheap structure exact
   in that direction, and fall through to the expensive test only for the other.
2. **Ring/shaped generators over grid sweeps** (§5.2) wherever a query is radial.
3. **Evaluation order: cheap filters → sort → expensive filters top-down, stop on
   first pass** (§3.6). Already the spirit of "cull cheaply before testing
   expensively" in `CLAUDE.md`, but the *top-down-and-stop* part is the piece we
   do not currently state, and it is what turns O(n) expensive tests into ~1.
4. **Non-negative inputs, all phrased as virtues** (§2.4). Safety not danger,
   proximity not distance. Costs nothing, removes a whole class of tuning bug.
5. **Conditions over weights, one or two weights maximum** (§3.5), with fallback
   options instead of a tuned tradeoff.
6. **Query failure as a behaviour signal** (§3.5).
7. **Coloured debug spheres with per-point rejection reasons** (§3.8). Both
   architecture chapters treat this as load-bearing rather than a nicety.
8. **The branch-and-bound heap** (§3.6) — worth remembering but *not* worth
   building now. It only pays when expensive **weights** exist, and by
   `CLAUDE.md`'s own rule that is a measurement to take, not an assumption.

### 10.4 What not to take

**[inferred]** Continuous re-querying (§5.5) is a real-time technique. In a
turn-based game the query runs once per decision, so the entire orbit/boids/
random-walk family is inapplicable — and the CPU concern that governs it does not
arise. Noted only so it is not mistaken for a general result.

**[inferred]** More broadly: everything in §3 and §5 is architecture for
*continuous space*. We index cells by arithmetic, which makes generation free and
removes most of what those systems exist to manage. The parts that transfer are
the **scoring discipline** (§10.3 items 3–7) and the **approximation discipline**
(§2.3) — not the query engine.
