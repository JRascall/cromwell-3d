# MMO architecture — where you put the boundary

How EVE Online, World of Warcraft, Star Citizen and Destiny divide a world
between machines; what each division costs; and what an engine needs to expose
so the choice stays a per-game decision rather than a permanent one.

This is the layer **above** [`valve_networking.md`](valve_networking.md). That
note is about a tick: prediction, interpolation, rewind, one server and its
clients. This note is about what happens when one server is not enough.

## Sourcing

MMO architecture is badly sourced compared to rendering. There is no equivalent
of a SIGGRAPH paper, and studios talk about it in marketing terms. Tags:

| Tag | Meaning |
|---|---|
| **[CCP]** / **[CIG]** / **[BLIZZARD]** | The studio's own technical statement |
| **[PATENT]** | A granted patent. **The best MMO sourcing that exists** — legally obliged to describe the mechanism, with numbers |
| **[GDC]** | A conference talk by the people who built it |
| **[EPIC]** | Epic's engine documentation |
| **[COMMUNITY]** | Player wikis and reverse engineering. Good on observable behaviour, weak on mechanism |
| **[inferred]** | My reasoning |

**Where the numbers in this note are unusually solid:** Blizzard patented
cross-realm zones, so §4's thresholds are from the patent text rather than from
forum archaeology.

---

## 1. The only question that matters

Every architecture below is one answer to: **when two players are in the same
place, which machine decides what happens?** Everything else — persistence,
matchmaking, instancing — follows.

There are five answers in shipped games, and they are not a progression. They
are a trade against three quantities that cannot all be maximised:

- **Concurrency** — how many players can share one space.
- **Continuity** — whether the world is one place or many copies.
- **Cost** — CPU per player, and engineering complexity.

| Topology | Concurrency in one space | Continuity | Shipped example |
|---|---|---|---|
| **Realm / shard** | Whole realm, but realms never meet | One world **per realm** | WoW realms; almost every MMO 2004-2012 |
| **Instance** | Small, fixed (5-40) | Explicitly a copy | Dungeons, raids, battlegrounds |
| **Dynamic zone-instance** | Capped, **split and merged automatically** | Illusion of one world | WoW sharding / CRZ / layering |
| **Single shard, static partition** | One node's worth, degraded not capped | **Genuinely one world** | EVE Online |
| **Meshed authority** | Multiple nodes over one continuous space | Genuinely one space | Star Citizen (partially) |

Plus one outlier that is worth knowing precisely because it refuses the frame:
**Destiny**, which is peer-to-peer for the action layer and cloud-authoritative
for everything that persists (§7).

---

## 2. EVE Online — one world, statically partitioned

### 2.1 The cluster

Tranquility is a **single cluster serving the entire playerbase** — there is one
New Eden, and this is the defining architectural choice of the game. It is three
tiers **[CCP / COMMUNITY]**:

| Tier | What it does |
|---|---|
| **Proxy blades** | Public-facing. Take player connections and route them into the cluster. |
| **SOL blades** | The simulation. **90-100 blades running 2 nodes each**; the full cluster is around **230 nodes**. |
| **Database** | The persistence layer. SQL Server on SSDs, chosen for IOPS. |

The load-bearing sentence, and it is worth reading twice:

> **"A node is primarily a CPU intensive EVE server process running on one
> core."** **[COMMUNITY, quoting CCP]**

**One solar system maps to one node; one node is one process on one core.** The
busiest systems — Jita, Motsu, Saila — get dedicated blades. That is the whole
partitioning scheme. It is static, it is spatial, and it is aligned to a
boundary the game design already had: you cannot be in two solar systems at
once, and travelling between them is a discrete, ceremonial act (a gate jump)
that is a natural handover point.

**This is the deepest lesson in the note** [inferred, but it recurs in every
system below]: EVE's architecture is cheap because the *game design* supplied
the seam. Star systems separated by explicit gates are a partitioning scheme
disguised as fiction. A seamless open world has no such seam, which is why
§5 costs CIG a decade.

The stack is **Stackless Python** on both client and server, which gives
cheap cooperative microthreads without OS thread overhead — appropriate when
you have thousands of concurrent entity behaviours per node and none of them are
individually expensive.

Scale, from the same source **[COMMUNITY]**: ~300,000 active players, up to
40,000 concurrent, 250 million database transactions per day.

### 2.2 Time Dilation — the honest answer to overload

Most games cap concurrency. EVE cannot: the whole proposition is that a
thousand players can converge on one system for a war. So instead of refusing
players, **it slows down time**.

CCP's own account **[CCP]**: TiDi activates *"when the server node has become
overloaded"*, and in battles of **1,300+ concurrent pilots** it held module
response times *"under one second for the vast majority of the action"* —
against a historical baseline of *"20, 40, 600 seconds"* of delay in comparable
fights. Across the whole universe it averaged **45 minutes per day** of dilated
simulation time.

The mechanism is simply that the simulation advances slower than wall-clock, for
everyone in the affected space equally. Nobody gets an advantage, and the
alternative — a node that falls arbitrarily far behind and delivers 600-second
command latency — is worse in every way.

Two things about this are architecturally important:

1. **Graceful degradation beats a hard cap, when the design can absorb it.**
   EVE's combat is at a timescale where half-speed is survivable. Applying TiDi
   to an FPS would be absurd. The technique is not portable; the *principle* —
   decide in advance what degrades and make it degrade evenly — is.
2. **CCP state the granularity is wrong.** TiDi *"operates at the node level
   rather than per-solar-system, affecting multiple star systems
   simultaneously"*, and they acknowledge this as suboptimal but forced by
   existing architecture **[CCP]**. A fight in one system dilates its
   neighbours. That is the static partition's bill arriving: because the node is
   the unit of scheduling, the node is also the unit of degradation.

---

## 3. Instancing — the boring answer that mostly wins

Before the clever architectures: **a private copy of a space, for a known small
group, is the answer almost every MMO reaches for**, and it is the correct one
far more often than it is exciting.

Dungeons, raids, battlegrounds and arenas are separate simulations with a fixed
population (5, 10, 20, 40), spun up on demand and destroyed after. The reasons
are gameplay first and architecture second — encounter pacing, loot fairness,
no griefing — and the architectural benefit is almost free: **bounded
concurrency is a bounded cost, and a process that owns exactly one instance can
be scheduled anywhere.**

The cost is continuity. An instance is visibly a copy; you cannot walk into a
raid and watch. Games that care about the world being *one place* pay to avoid
this. Games that do not, should not pay.

---

## 4. World of Warcraft — copies pretending to be one world

WoW's outdoor world is the interesting case, because it uses three related
mechanisms that players routinely conflate, and Blizzard patented one of them,
so we have real numbers.

| Mechanism | Purpose | Crosses realms? | Scope |
|---|---|---|---|
| **Realms** | The original partition. Each realm is a full copy of the world with its own characters, economy and database. | No | Everything |
| **Cross-Realm Zones (CRZ)** | Fights **under**-population. Merges players from several realms into one zone instance so a quiet zone feels alive. | **Yes** | Per zone |
| **Sharding** | Fights **over**-population. Splits a busy zone into multiple copies. Introduced in *Warlords of Draenor*. | Yes | Per zone |
| **Layering** | Sharding applied to an **entire realm** at once — the whole outdoor world duplicated. Used at *Classic* launch, later removed. | **No** | Whole realm |

The distinction that matters: **sharding and CRZ are the same machinery pointed
in opposite directions.** One splits when there are too many players, the other
merges when there are too few. Layering differs in scope (whole realm, not one
zone) and in that it deliberately does *not* cross realms **[COMMUNITY]** —
because Classic's social contract is that your realm is a community with a
fixed population you can come to recognise.

### 4.1 The patent, and the numbers

US 9220982 B2, *"Cross-realm zones for interactive gameplay"* **[PATENT]**,
states the problem precisely: segregating characters into realms starves
low-population groups of peers, but combining everyone into one instance
*"creates excessive computational load"*. So certain zones are designated
cross-realm and others stay realm-specific — selective blending, not a global
policy.

Placement is by a **zone transfer engine**: on entering a zone it checks which
realm the character belongs to, whether that realm is in the subset assigned to
a particular zone instance, and places them accordingly. Realms are organised in
a **"realm grouping tree"** that can be traversed up or down to widen or narrow
which realms share a zone — so population management is a tree walk, not a
rebuild.

The thresholds, from the patent text:

| | |
|---|---|
| **Split** a zone instance | at approximately **500 characters** |
| **Merge** instances | below approximately **100 characters** |
| Visibility | within a defined *"threshold proximity"* |

Host selection uses *"deterministic formulas (e.g., modulo operations)"* to
assign zone instances to realm servers consistently, minimising cross-network
chatter — i.e. the same zone instance lands on the same physical host every
time, so the routing is computed rather than looked up.

**What is shared versus segregated** is the design-relevant part:

- *Shared* across realms in a CRZ: character visibility within proximity,
  actions and their effects, interactive gameplay, zone objects and terrain.
- *Segregated*: characters in single-realm zones, private instances (dungeons),
  **and dropped items, which exist per zone instance** — because loot is
  economy, and merging economies across realms is a different and much larger
  decision.

That last line is the tell. **The boundary is not drawn by what is technically
convenient; it is drawn around the economy.** Blizzard will share your
*presence* across realms freely and will not share a dropped item, because one
is cosmetic-adjacent and the other is the game.

### 4.2 The honest cost

Sharding works and players dislike it. You and a friend can stand in the same
named place and not see each other; a world boss can be farmed once per shard;
a zone that feels populated is populated by strangers you will never see again.
The mechanism trades *continuity of place* for *continuity of experience*, and
whether that is the right trade is a design question, not an engineering one.

**Note the number: 500.** WoW splits an outdoor zone instance at ~500
characters. Star Citizen caps a shard at 500 players (§5.6). Different decades,
different engines, different genres, same order of magnitude — that is roughly
what one authoritative simulation of interacting characters costs before it
needs help. [inferred, but two independent data points at the same number is
worth noticing.]

---

## 5. Star Citizen — the deep dive

CIG are attempting the thing everyone else declined: **many server nodes
simulating one continuous, seamless space, with no visible instance
boundaries**. It is worth studying carefully whether or not you believe the
schedule, because the architecture is genuinely novel and the failure modes are
being discovered in public.

### 5.1 The central move: separate replication from simulation

Every architecture above conflates two things that do not have to be the same:

- **Who simulates an entity** (runs its physics and logic this tick)
- **Who owns its state** (holds the authoritative values, persists them)

In a conventional dedicated server these are the same process, which is why a
server crash loses the world since the last save. CIG split them.

The **Replication Layer** sits between clients and game servers and holds the
state; the **Dedicated Game Servers (DGS)** hold only the simulation. CIG's
description **[CIG]**: it is *"a central place where we can control the
streaming and network-bind logic"* that can *"replicate the network state to
multiple server nodes."*

Critically, **the Replication Layer is event-driven, not tick-based** — it
*"immediately process[es]"* incoming packets and forwards them, rather than
running a simulation loop **[CIG]**. It is a router and a store, not a
simulator. That is what lets it sit in the hot path without adding a tick of
latency; CIG state they deploy it in the same data centres as the game servers
so the extra hop costs *"less than a millisecond."*

### 5.2 The components

| Component | Role |
|---|---|
| **Gateway** | Routes packets between clients and Replicants. **Holds no game state.** |
| **Replicant** | Handles networked entity streaming and state replication between clients and servers. |
| **EntityGraph** | A **graph database** storing *"the state of every single network replicated entity"*. |
| **DGS** | The actual simulation nodes. |

**[CIG]** for all four. Clients never connect to a game server directly — they
connect to a Gateway, which routes to one or more Replicants, and **a single
client may be served by multiple Replicants simultaneously**.

The current shipping form is a single **"Hybrid"** service combining Replicant,
Atlas, Scribe and Gateway, to be split into separate microservices later
**[CIG]** — i.e. the architecture is designed distributed and deployed
monolithic first, which is the correct order and worth noting as a delivery
strategy.

### 5.3 Authority and handover

> *"One server node that controls the entity, and multiple other server nodes
> that have a client view of this entity."* **[CIG]**

Only the authoritative node may write entity state back to the Replication
Layer. Every other node treats the entity exactly as a client does — it receives
updates and renders them. **This is the elegant part**: a server node's
relationship to a non-authoritative entity is the same relationship a client has
to it, so there is one code path, not two. [inferred, but it is the obvious
motivation for the framing.]

Authority transfers when an entity leaves the current authoritative server's
streaming bubble, **or on demand to rebalance load** **[CIG]**. As an entity
crosses a boundary, authority swaps and the previous owner becomes a receiver
**[COMMUNITY]**.

### 5.4 Persistent Entity Streaming

Because the Replication Layer owns state rather than borrowing it, state can be
written straight to a permanent database. Entities survive between server
instances; a ship left somewhere is there when you return **[CIG]**.

The interesting wrinkle is cross-shard movement: items must be *"placed into an
inventory before they can be unstowed into a different shard"*, with a
**"shard-transition inventory"** that follows the player **[CIG]**. So the
seamlessness has a documented edge — objects loose in the world belong to a
shard, objects in your inventory belong to you. [inferred] That is the same
instinct as WoW's per-instance loot in §4.1: **the boundary gets drawn around
ownership, because ownership is the thing that cannot be duplicated.**

### 5.5 Crash recovery — the actual payoff

This is where the architecture earns its complexity, and it is the part most
worth stealing:

| Failure | Consequence **[CIG]** |
|---|---|
| **Replicant crashes** | Clients freeze temporarily. A replacement spins up, **recovers entity state from EntityGraph**, reconnects clients. CIG hope for *"less than a minute"*, with some entity snapping afterwards. |
| **Gateway crashes** | Recovery *"more in the region of seconds"* — **because Gateways hold no game state.** |
| **Hybrid service crashes** (current) | Everyone disconnects — the "30k error". |

Read the middle row against the top one. **The recovery time of a component is
a direct function of how much state it owns.** The stateless router recovers in
seconds; the state-holding replicator recovers in a minute; the monolith does
not recover at all. That is a design rule, not a Star Citizen fact, and it
applies to any system with components that can fail — which is all of them.

### 5.6 What has actually shipped

Being precise, because this area attracts both hype and dismissal:

| | |
|---|---|
| **Static server meshing** | Shipped. Alpha 4.0, LIVE preview **19 December 2024**, with the Pyro system and the Stanton-Pyro jump point. |
| **Player cap per shard** | **500** for Stanton and Pyro — up from **100**. |
| **Stability** | ~57% reduction in disconnections reported; CIG's CTO a year on: *"It has been surprisingly solid... We expected a lot more turbulence."* |
| **Current state** | Alpha 4.8 stable at that density across three systems. |
| **Dynamic meshing** | **Not shipped.** The 2026 focus — shards merging and splitting as players move. |

**[COMMUNITY / CIG, via press and CitizenCon coverage]**

**Static versus dynamic is the whole game.** In static meshing, server nodes are
assigned to *fixed locations* decided in advance; if everyone converges on one
place, that place's node eats the load or the shard hits its cap **[CIG]**. That
is EVE's model (§2) with a nicer transport — the partition is still spatial and
still static. CIG's stated reason for starting there is blunt and correct: the
authority-transfer problem is *"a nightmare"* **[CIG, paraphrased in
COMMUNITY]**.

So the honest read: **CIG have shipped the plumbing — decoupled state,
recoverable nodes, authority handover between a fixed set of servers — and have
not yet shipped the load balancing that the plumbing exists to enable.** The
plumbing is the hard, valuable, generalisable part. The 100 → 500 jump is real
and came from it.

---

## 6. Interest management — the cost that actually kills you

Everything above is about *who simulates*. This section is about *who gets told*,
and it is where naive implementations die first.

The naive algorithm is: for each replicated object, for each client, decide
whether to send. That is **O(objects × clients) every frame**. Epic state the
problem with their own numbers **[EPIC]**:

> Fortnite Battle Royale starts each match with **100 connected players and
> about 50,000 replicated Actors**, and the standard strategy *"performs poorly
> in cases like this and will bottleneck the server's CPU."*

100 × 50,000 = **five million relevancy tests per frame**. The answer —
**Replication Graph** — is not a better test; it is *not doing the test*.
Actors register with nodes in a graph, spatial data is cached in spatial
structures, and each client's relevant set is gathered by walking the graph
rather than by polling every actor **[EPIC]**. UE5's **Iris** is the successor
system.

This is CLAUDE.md's rule 1 exactly — **do less work, algorithmically, before
doing the same work faster** — and it is measured at the same magnitude the
codebase already found: replacing a per-cell roll call over every unit with one
array read gave **146x** here. Epic replaced a per-actor roll call over every
client with a cached spatial walk. Same shape of fix, same reason.

The three levers, in the order they pay [inferred, from the systems above]:

1. **Spatial** — only tell you about things near you. Every system here does
   this; EVE's grid, UE's spatialization nodes, SC's streaming bubbles.
2. **Frequency** — tell you about distant things less often. A ship 50 km away
   does not need 60 Hz.
3. **Fidelity** — tell you *less* about distant things. Position but not
   animation state; existence but not inventory.

Lever 3 is the one usually skipped and it is the one that composes with
rendering: if you are not going to draw a character's hands, do not replicate
what its hands are doing.

---

## 7. Authority, and the outlier

**Destiny** is worth a section because it refuses the whole framing. Bungie
built *"an intersection of traditional peer-to-peer networking and a new
cloud-based server architecture"* **[GDC — Justin Truman, GDC 2015]**.

The action layer — movement, shooting, physics — is peer-to-peer between the
players in your activity, with one player elected as **physics host**.
Everything that persists — your character, your inventory, your progress — is
cloud-authoritative and never trusted to a peer.

The mechanism when a host leaves **[GDC]**: elect a new physics host from the
remaining players and hand off authoritative simulation state. When a player
re-enters an empty activity bubble, she spins up the simulation in its default
state, becomes physics host, and receives a **`reconcile()`** call to fix up all
simulation state to match the authoritative sensor state.

That `reconcile()` is the transferable idea: **a bounded, explicit function that
takes a freshly-initialised simulation and an authoritative summary and makes
the former match the latter.** If you have that function, host migration is a
solved problem and so is late-join, crash recovery and rejoining. If you do not,
each is a separate bug.

The trade Bungie made is legible: **P2P for the things where latency is the
product and cheating is survivable; server authority for the things where
cheating is the product.** Your aim is peer-simulated; your loot is not. This is
the same boundary WoW draws around dropped items (§4.1) and CIG draw around
inventory (§5.4). **Three independent systems, one line, drawn in the same
place: authority follows value, not latency.**

---

## 8. Rendering under crowd load — the part nobody's architecture saves you from

A brief section because it is genuinely a separate problem from the networking,
and conflating them is a common error.

Even if 500 players are in one space and the server is coping, **you are not
drawing 500 fully-featured characters at 60 Hz.** The client's limit arrives
before the server's, and no amount of meshing helps.

The techniques are the ones in [`crowd_scale.md`](crowd_scale.md) — Assassin's
Creed Unity's 300-versus-11 bone split, the small expensive set near the player
and the large cheap one elsewhere — and they apply unchanged. What the
*networking* layer contributes is lever 3 in §6: **if the renderer has decided
someone is an imposter, the network should stop sending their animation state.**
That coupling is the only genuinely networking-specific thing here, and it
requires the interest-management system to be able to ask the renderer a
question, which most architectures make awkward. [inferred]

---

## 9. What `cromwell` needs — the pluggable version

The brief was: not locked into one model; drop-ins per game. That is achievable,
but only if the *right* thing is made pluggable. Getting this wrong means
building five systems instead of one abstraction.

### 9.1 What must NOT be pluggable

**The entity's identity and its state description.** Every topology above needs
"this entity, these fields, this authority". If the schema layer differs per
topology you have five engines. Per [`valve_networking.md`](valve_networking.md)
§2.1, that layer should be a **runtime-described schema** — field name, bit
count, range, encoder — not compiled macros, and it is shared by every model.

### 9.2 The one interface that makes the rest pluggable

Everything in this note is a different answer to two questions, and they are
separable:

```
AuthorityPolicy  : (entity, world state) -> node
InterestPolicy   : (observer, entity)    -> { rate, fidelity } | none
```

- **Realm/shard** — `AuthorityPolicy` returns a fixed node per realm.
- **Instancing** — returns the instance's node; interest is "everyone in the
  instance".
- **Dynamic zone-instance (WoW)** — returns a zone instance chosen by
  population, with the split/merge thresholds as policy parameters (§4.1's 500
  and 100 are *literally the tuning constants*).
- **Static partition (EVE, SC static meshing)** — returns a node by spatial
  lookup.
- **Meshed (SC dynamic)** — returns a node by spatial lookup *plus* load, and
  the return value changes over time, which is handover.

**The five topologies are five implementations of one function.** That is the
finding that makes the brief tractable — and it only becomes visible by reading
them side by side, which is the argument for the note.

### 9.3 What the engine must provide underneath, for any of them

1. **Authority as a first-class, transferable property**, with handover as a
   supported operation rather than a special case. Even single-node games
   benefit — this is what makes host migration and crash recovery possible
   later. §5.3's insight matters: **a non-authoritative entity on a server
   should use the same code path as an entity on a client.** One path, not two.
2. **State ownership separated from simulation ownership** (§5.1). This is the
   single most valuable structural idea in the note, and its payoff — §5.5's
   recovery-time-proportional-to-state-owned — applies to every failure, not
   just crashes.
3. **A `reconcile(fresh_simulation, authoritative_summary)` function** (§7).
   Late join, host migration, crash recovery and rejoin are the same operation.
   Build it once, deliberately, or debug it four times.
4. **Interest management as a cached spatial walk, never a per-pair test**
   (§6). This is the one with a measured 146x-shaped payoff and it is
   architecturally expensive to retrofit, because it dictates how renderables
   register themselves. It should exist before there are enough entities to
   need it — and this codebase already has the right structure for it in
   `SpatialHash`.
5. **Interest with three levers, not one** — spatial, frequency, *and*
   fidelity. Fidelity is the one that couples to the renderer (§8) and the one
   people omit.

### 9.4 What to take off the shelf

The transport, per [`valve_networking.md`](valve_networking.md) §6:
GameNetworkingSockets, BSD. None of the above is transport work.

### 9.5 The design lesson, which is not about networking

**EVE's architecture is cheap because gate jumps gave it a seam.** WoW's is
cheap because zones gave it one. Star Citizen is expensive because a seamless
solar system has none, and CIG have spent a decade manufacturing one.

Before building the general solution, look at whether the game already contains
a boundary you can partition along. This project's world is a **tile grid with
storeys** — it has seams everywhere, and they are already the basis of
`OccupancyGrid` and `ReachField`. If networking is ever added, the partition
should follow the structure the simulation already uses, not a new one invented
for the network. That is the same rule as `study/spatial_queries.md` §5.2:
enumerate in the shape of the question, not the shape of the grid — here,
partition in the shape of the world, not the shape of the hardware.

---

## Sources

**[PATENT]** — the best-sourced material here:

- [US 9220982 B2 — Cross-realm zones for interactive gameplay](https://patents.google.com/patent/US9220982B2/en) (Blizzard). §4.1's 500/100 thresholds, the zone transfer engine and the realm grouping tree
- [US 10086279 — Cross-realm zones for interactive gameplay](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10086279) (continuation)

**[CCP]** / EVE:

- [Time Dilation — How's That Going?](https://www.eveonline.com/news/view/time-dilation-hows-that-going)
- [Tranquility Tech IV](https://www.eveonline.com/news/view/tranquility-tech-iv)
- [EVE Online Architecture — High Scalability](https://highscalability.com/eve-online-architecture/) — the three-tier breakdown
- [Tranquility — EVE University Wiki](https://wiki.eveuniversity.org/Tranquility) **[COMMUNITY]**
- [Meet Tranquility — PC Gamer](https://www.pcgamer.com/eve-online-1/)

**[CIG]** / Star Citizen:

- [Server Meshing and Persistent Streaming Q&A](https://api.star-citizen.wiki/comm-links/18397) — the primary technical source for §5
- [Server meshing — Star Citizen Wiki](https://starcitizen.tools/Server_meshing) **[COMMUNITY]**
- [Server Meshing Success Defines Star Citizen's Year of Playability](https://soren.com/en/news/star-citizen/2026-01-02-server-meshing-success-defines-star-citizens-year) — the CTO's one-year assessment
- [Star Citizen CTO on server meshing progress, Alpha 4.7](https://massivelyop.com/2026/02/06/star-citizen-cto-outlines-progress-on-server-meshing-and-crafting-with-alpha-4-7-on-track-for-march/)

**[GDC]**:

- [Justin Truman — Shared World Shooter: Destiny's Networked Mission Architecture, GDC 2015](https://archive.org/details/GDC2015Truman) (full text also on [archive.org](https://archive.org/stream/GDC2015Truman/GDC2015-Truman_djvu.txt))
- [Timothy Ford — Overwatch Gameplay Architecture and Netcode, GDC 2017](https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and)

**[EPIC]**:

- [Replication Graph in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/replication-graph-in-unreal-engine?lang=en-US)
- [Unreal Engine Improvements for Fortnite: Battle Royale](https://www.unrealengine.com/en-US/tech-blog/unreal-engine-improvements-for-fortnite-battle-royale) — the 100 players / 50,000 actors figure

**[COMMUNITY]** — WoW:

- [Sharding — Warcraft Wiki](https://warcraft.wiki.gg/wiki/Sharding_(term))
- [Layering — Warcraft Wiki](https://warcraft.wiki.gg/wiki/Layering)

**Related notes:** [`valve_networking.md`](valve_networking.md) for the layer
below (one server, one tick); [`networked_animation_physics.md`](networked_animation_physics.md)
for what actually crosses the wire; [`crowd_scale.md`](crowd_scale.md) for the
rendering limit §8 defers to.
