# Battle scale — Total War and Men of War

Deep dive on two games that are **not** crowd games. Their characters are all
real: every soldier fights, takes orders, has morale, and in one case carries an
inventory. There is no cheap tier to hide behind.

Read against [`crowd_scale.md`](crowd_scale.md), which covers the opposite
strategy — thousands of characters, almost none of them real.

Source tags as elsewhere. **[CA]** is Creative Assembly speaking directly.
**[AMD]** is the GPUOpen engine analysis written with CA's cooperation.
**[COMMUNITY]** is press, wikis and player-measured figures. **[inferred]** is
our reading.

> **The one sentence.** These two sit at opposite ends of the same trade:
> **unit count × simulation depth is roughly constant**, and each picked a
> different end of it deliberately. Total War buys count and spends it on
> breadth; Men of War buys depth and spends it on ballistics.

---

## 1. Total War — thousands of shallow-but-real soldiers

### 1.1 The actual numbers

**[COMMUNITY]** At the **Ultra** unit-size setting a basic infantry unit is
**160 men**; higher-tier units are smaller with more health each (Orc Boyz 160,
Black Orcs 100). An army is **20 units**.

| | Count |
|---|---|
| Men per basic unit (Ultra) | 160 |
| Units per army | 20 |
| Men per side | ~**3,200** |
| Men on the field (2 armies) | ~**6,400** |

**[inferred] Compare that with `crowd_scale.md` §2 and the comparison is not
close.** AC Unity's 10,000 contains 40 real AI. Total War's ~6,400 are *all*
real: each has collision, a combat state, morale that responds to what it can
see, and — per §1.3 — its own physically simulated projectiles. Six thousand
genuine combatants is a substantially harder problem than ten thousand mostly
fake pedestrians, and the two figures should never be compared directly.

### 1.2 The architecture that makes it possible: simulate ahead of the display

**[CA]** This is the most transferable idea in either game. Scott Pitkethly:

> *"the logic generates the 'future' whilst the display renders the 'now'. This
> future logic-state can be created over many display frames, allowing us to
> calculate complex interactions without impacting frame rate."*

**[CA]** Battle logic and animation — both very CPU-intensive — are **decoupled
from the display** and run simultaneously with it. Warhammer was also where CA
moved to genuine multicore, spreading work across many threads.

**[inferred] Why this matters more than it sounds.** The usual framing of a game
loop is *simulate this frame, then draw this frame*, which makes every simulation
spike a frame-rate spike. Decoupling breaks that link: a complex interaction may
take several display frames to resolve, and the renderer never waits. The
simulation is effectively running at its own variable rate slightly **ahead** of
what you see.

The cost is latency and the burden of interpolating between simulation states
for display — and it makes the simulation harder to reason about, because "now"
on screen is not "now" in the logic. That is the price of never hitching.

### 1.3 Every projectile is real

**[CA]** The engine physically simulates **every individual projectile** fired by
every soldier, with accuracy varying by the firer's quality and the target's
situation. Because *"calculating those shots en-masse is expensive"*, collision
checks against terrain and buildings go through **queuing systems** rather than
being resolved immediately.

**[inferred]** That queue is the same pattern as §1.2 one level down: work that
cannot be afforded synchronously is spread over time instead of approximated
away. A volley from 160 archers is not resolved in the frame it is loosed; it is
fed through a budgeted queue. Worth noting because the naive alternatives —
hitscan, or a single "unit fires at unit" roll — are exactly what CA chose not to
do, and the visible arcs of arrows are the reason.

### 1.4 Melee: matched animations between pairs

**[CA]** Different unit classes need bespoke animation solutions — animation-set
culling on castle walls, and **matched fighting animations between specific
entity pairs**, up to set pieces like a dragon lifting a Carnasaur before being
dragged down.

**[inferred]** "Matched pair" is the key phrase and it is a real constraint.
Two soldiers agreeing to play complementary animations means melee is a
*negotiation between two entities* rather than each acting independently: they
must be paired, aligned, kept in sync, and released cleanly if one dies mid-
animation. It looks excellent and it is why Total War melee has always been a
line of duels rather than a press — a limitation the community notices and asks
about repeatedly.

### 1.5 Line of sight, and an honest failure

**[CA]** A documented discrepancy: **small terrain undulations invisible from the
overhead camera can block a soldier's eye-level sight line**, so riflemen advance
toward an enemy instead of firing as ordered.

**[inferred]** Worth recording because it is a *correct* simulation producing a
bad player experience. The LOS is right; the player's mental model — formed from
a camera three hundred metres up — is wrong. The general lesson is that any
sim-versus-perception mismatch will be read as a bug regardless of which side is
accurate, and this project has the same exposure with `kEyeHeight` and cover.

### 1.6 AI and maps

**[CA]** Battle AI uses invisible **hint-lines** around terrain features to mark
hills as desirable and indicate where to position. Pathfinding struggles with
broken terrain and complex urban layouts, and needs **well-connected street
networks** — poor connectivity makes the AI take absurd routes or treat passable
ground as impassable.

**[CA]** Consequently every battle map is **hand-crafted**, not procedurally
generated, so terrain, paths and sight-lines can be coordinated.

**[inferred] That is a significant admission and it is the honest counterpart to
`navigation.md` §5.** Even with Recast-class navigation available industry-wide,
a studio shipping large-scale battle AI chose to constrain the *content* rather
than make the pathfinder cleverer. Hand-authored hints and hand-built maps are
not a failure of engineering; they are frequently the cheapest correct answer.

### 1.7 The renderer

**[AMD]** From the GPUOpen engine analysis (describing *Attila*, the base
Warhammer improved on):

| | |
|---|---|
| Shadows | cascaded shadow maps, **2–4 cascades** by settings; terrain **excluded**, but many small meshes included |
| GBuffer | **3 textures** — diffuse+empty alpha, 2-channel compressed normal+empty alpha, specular/gloss |
| Lighting | statistically based, physically correct BRDF with Gaussian micro-facet distribution, introduced in Rome II |
| Particles | GPU quad, point-light and projected-decal types; limited CPU mesh particles |

**Terrain is the interesting part.** **[AMD]** A battlefield is composed of tiles
taken from the campaign map. Each tile carries **8 texture layers** (diffuse,
normal, specular each), and at tile junctions up to **24 layers per pixel** can
blend — **72 texture reads per pixel** in the worst case. The implementation is a
**depth-only pass** for geometry followed by **one screen-space pass per tile**,
projecting the layers as decals.

**[inferred]** Two things follow. The empty GBuffer alpha channels are not waste
— they are the blend space that lets terrain and decals composite into the
buffer afterwards, which is why the layout looks under-packed. And rendering
terrain layers *as screen-space decals* rather than as a mega-shader is the same
instinct as §1.2 and §1.3: convert an unbounded per-pixel cost into a bounded
number of passes over the pixels that actually exist.

---

## 2. Men of War — few units, simulated absurdly deeply

**[COMMUNITY]** The GEM engine (GEM 2 in the modern titles) takes the opposite
bet: far fewer entities, each carrying a simulation depth Total War never
attempts.

### 2.1 What each soldier actually is

**[COMMUNITY]** Every soldier has an **individual inventory** — weapons,
ammunition, helmets, grenades, kit — and can equip or drop items scavenged from
corpses, enemies and supply crates on the field. **Direct control** lets the
player take over any single unit and drive it as a third-person shooter or tank
sim, aiming manually at weak points.

**[inferred] Direct control is the tell.** It means every soldier and vehicle
must *already* be simulated to a standard that survives being played manually.
There is no LOD tier a unit can hide in, because the player may take the wheel of
any of them at any moment. That single design decision forecloses every technique
in `crowd_scale.md`.

### 2.2 Ballistics and armour

**[COMMUNITY]** Projectiles are real objects. They can **ricochet and fly on**
after glancing off armour at a shallow angle. Penetration is computed from
**impact angle, the armour's own angle, armour strength and gun strength**.
Vehicles have **no health bars at all** — they are a set of armour faces with
thicknesses and angles, and damage is where you hit.

**[inferred]** This is a physics simulation wearing an RTS costume, and it is the
budget. A Total War arrow needs to fly convincingly and hit; a Men of War shell
needs an angle-of-incidence calculation against an oriented plate, and may then
continue as a new projectile. Per-shot cost is orders of magnitude apart, which is
precisely why the unit counts are.

### 2.3 Destructible everything

**[COMMUNITY]** Buildings, vehicles and terrain take progressive damage and can
be demolished by gunfire, explosions and vehicle impacts — a tank can flatten a
house with infantry inside, **changing cover and movement paths mid-battle**.

**[inferred]** Note the consequence for navigation: cover and paths are *runtime*
data, not authored data. That is the opposite of Total War's hand-crafted maps
with hint-lines, and it is only affordable because the unit count is two orders
of magnitude lower.

### 2.4 The numbers, and a surprise

**[COMMUNITY]** Reported figures:

| | |
|---|---|
| MoW: Assault Squad 2 (32-bit engine) | 2×2 km maps, **10–15k entities**, **500–800 units** spawned before instability |
| Gates of Hell (GEM 2, 64-bit) | ~10 vehicles + 100 infantry **per side** with no significant impact on a mid-range laptop |
| Engine ceiling | 64-bit GEM is described as near-unbounded in principle; the PC is the limit |

**The surprise:** **[COMMUNITY]** in GEM games the **map itself costs more frame
time than the NPCs** — large battles run better on maps with less environmental
detail.

**[inferred] That is the exact inverse of every game in `crowd_scale.md`**, and
it is the most useful single fact in this section. When each entity is
individually expensive, you would expect entities to dominate — instead the
destructible, densely-propped environment does. It is a reminder that "what is
the bottleneck" is an empirical question per engine, and that intuition
transferred from another game's architecture is frequently backwards.

Note also the distinction between **entities** (10–15k: soldiers, vehicles,
crates, dropped weapons, debris) and **units** (500–800: things under command).
Most of a Men of War map is *inert simulated objects*, and that is where the
entity count goes.

---

## 3. Read together

### 3.1 The trade, stated plainly

| | Total War | Men of War |
|---|---|---|
| Combatants on field | ~6,400 | ~200–800 |
| Per-soldier state | position, morale, combat pairing | + inventory, stance, individual ballistics |
| Projectiles | simulated, **queued** | simulated, with ricochet and penetration |
| Vehicles | n/a (monsters/artillery) | armour plates and angles, **no health pool** |
| Terrain | hand-crafted, static, hint-lines | fully destructible, paths change at runtime |
| Bottleneck | CPU battle logic, hence decoupling | **the map**, not the units |
| Player control | orders to units | orders **or direct control of any single body** |

**[inferred]** Roughly an order of magnitude of count traded for roughly an order
of magnitude of depth. Neither is wrong. What is wrong is wanting both without
naming which one is being given up — and the fastest way to find out which a
design has actually chosen is to ask **"can the player take direct control of
one?"**. If yes, every entity pays full price forever.

### 3.2 Three techniques worth taking

**[inferred]**

1. **Decouple simulation from display and let it run ahead.** Total War's
   "future/now" split is the strongest idea in this document. It converts
   simulation spikes from frame-rate spikes into latency, which is nearly always
   the better currency. Costs interpolation and reasoning difficulty.
2. **Queue expensive work instead of approximating it.** Projectile collision is
   spread across a budget rather than replaced by a dice roll. Preserves fidelity
   *and* bounds cost — the answer when neither "do it all now" nor "fake it" is
   acceptable.
3. **Constrain the content rather than perfecting the algorithm.** Hint-lines and
   hand-built maps beat a cleverer pathfinder, from a studio that could afford
   either.

### 3.3 And one to be wary of

**[inferred]** **Matched-pair melee animations** look superb and quietly become a
constraint on the combat design — fights become duels because pairs are the unit
of animation. Worth knowing before adopting, not after.

---

## Sources

**Total War**
- [Designing Total War: Warhammer II to handle tons of units and massive battles](https://www.gamedeveloper.com/design/designing-i-total-war-warhammer-ii-i-to-handle-tons-of-units-and-massive-battles) — Creative Assembly. Primary source for the future/now decoupling, projectile queuing, matched animations, hint-lines, LOS discrepancy and hand-crafted maps
- [Anatomy Of The Total War Engine, Part I](https://gpuopen.com/learn/anatomy-total-war-engine-part/) — AMD GPUOpen. GBuffer, shadows, the tiled terrain layer system
- [Ultra unit size discussion](https://steamcommunity.com/app/364360/discussions/0/357287304424683838/) and [unit size](https://www.gamepressure.com/total-war-three-kingdoms/unit-size/zcc43b) — the 160-per-unit figure

**Men of War / GEM**
- [GEM 2 engine](https://www.moddb.com/engines/gem-2) — ModDB
- [Men of War](https://en.wikipedia.org/wiki/Men_of_War_(video_game)) — Wikipedia; ballistics, penetration, inventory, direct control
- [Gem Engine and its possibilities](https://steamcommunity.com/app/400750/discussions/0/4835136555187954873/) — player-measured entity and unit ceilings, and the map-costs-more-than-NPCs observation

**Related notes**
- [`crowd_scale.md`](crowd_scale.md) — the opposite strategy: thousands of characters, almost none real
- [`navigation.md`](../agents/navigation.md) — the navigation and spatial layers underneath both
