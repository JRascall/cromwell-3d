# Crowd scale — how three games got to thousands

Deep dive on **Assassin's Creed Unity**, **World War Z** and **Left 4 Dead 1/2**:
three different answers to "how do you have far more characters than you can
afford", read for what transfers.

Source tags follow the rest of `study/`. **[VALVE]** is Valve's own published
talk. **[GDC]** is a conference presentation by the team that shipped it.
**[COMMUNITY]** is press, interviews and developer write-ups — good for numbers,
weaker on mechanism. **[inferred]** is our reading.

> **The one sentence.** None of these three made characters cheaper. All three
> made **most** characters *not be characters* — and spent the whole budget on
> the few near the player. The interesting engineering is in the **transition**,
> not in the cheap tier or the expensive one.

---

## 1. Left 4 Dead — the best-documented of the three

**[VALVE]** Michael Booth's *The AI Systems of Left 4 Dead* (AIIDE 2009) is the
primary source and is unusually complete for a shipped commercial game. Almost
everything below is from it.

### 1.1 What the Director is actually for

Four stated goals: robust behaviour performances, competent human player proxies
(the survivor bots), **promote replayability**, and **generate dramatic game
pacing**. Booth's term for the design intent is **"Structured
Unpredictability"** — not randomness, but variation inside a shape that always
produces a story with a beginning, a climax and a breather.

**[inferred]** Worth dwelling on, because it reframes what the AI is *for*. The
Director is not an opponent. It is a **pacing system that happens to use enemies
as its instrument**. That is why it works with a tiny entity budget: it was never
trying to simulate a zombie apocalypse, only to make the player feel one.

### 1.2 The nav mesh, and the two derived quantities

The nav mesh represents walkable space and came from **Counter-Strike bot
pathfinding** — reused, not written for L4D. Areas are axis-aligned quads with
bidirectional links (see `navigation.md` §8 for the structure).

Two derived values do most of the work, and both are cheap precomputations over
that mesh:

| | What it is | What it buys |
|---|---|---|
| **Flow Distance** | travel distance from the starting safe room to each nav area | tells the Director whether any point is **ahead of or behind** the group — the single most useful fact for spawning |
| **Active Area Set (AAS)** | the set of nav areas surrounding the survivor team | the window inside which population exists at all |

**[VALVE]** The AAS moves with the players, and the Director **creates and
destroys population as it sweeps** — which is what lets *hundreds* of enemies be
expressed by a **small set of reused entities**.

**[inferred] This is the whole trick, and it is not an animation or rendering
trick.** L4D never has thousands of zombies. It has a modest pool, recycled
through a moving window, and because the window is defined by *travel* distance
rather than straight-line distance, the recycling happens where the player
cannot see it.

### 1.3 Population rules

| Population | Rule |
|---|---|
| **Wanderers** | spawn as areas enter the AAS; despawn when the area leaves it **or becomes visible** |
| **Mobs** | every **90–180 s**, randomised; **75% spawn behind** the survivors |
| **Special Infected** | individually randomised intervals, in **non-visible** AAS areas appropriate to that class |

**[inferred]** Two design rules are doing the heavy lifting. *Never spawn or
despawn in view* is what makes recycling invisible. *Mostly spawn behind* is what
makes a horde feel like it caught up with you rather than materialised in front
of you — and it is also cheaper, because behind-the-group areas are the ones
about to leave the AAS anyway.

### 1.4 The pacing model

The Director tracks a per-survivor **emotional intensity**, raised by taking
damage, being incapacitated, and infected dying nearby. It uses the **peak across
all four** survivors, and decays it when nothing is engaging them.

That value drives a four-state cycle:

```
Build Up  ->  Sustain Peak (3-5 s)  ->  Peak Fade  ->  Relax (30-45 s)  ->  Build Up ...
```

**[inferred]** The numbers are the design. A three-to-five second peak and a
thirty-to-forty-five second relax is a *rhythm*, and it is authored, not
emergent. The randomness sits inside a fixed envelope — which is exactly what
"Structured Unpredictability" means and why L4D never produces a boring run or an
unsurvivable one.

### 1.5 Boss encounters

Three outcomes — **Tank**, **Witch**, **Nothing** — shuffled, then placed every
*N* units of flow distance along the escape route with a random offset, with
successive repeats prevented.

**[inferred]** "Nothing" being a first-class member of the shuffle is the neat
part: the absence of an encounter is scheduled with the same machinery as the
encounters, which is what stops the player learning the rhythm.

### 1.6 What transfers

- **Precompute one scalar over the nav mesh** (flow distance) and an enormous
  amount of spatial reasoning becomes arithmetic.
- **A moving active window** turns a large population into a small one, and
  belongs to the *game*, not the renderer.
- **Spawn and despawn out of sight** — the rule that makes recycling free.
- **Author the rhythm, randomise inside it.**

---

## 2. Assassin's Creed Unity — LOD applied to the brain

**[GDC]** Francois Cournoyer, *Massive Crowd on Assassin's Creed Unity: AI
Recycling* (GDC 2015).

### 2.1 The numbers

| | Count |
|---|---|
| Real AI brains | **40** |
| High-resolution models | **120** |
| Crowd NPCs on screen | **10,000** |

**[inferred]** Which reframes the famous figure entirely. Ten thousand is a
*rendering* number. The simulation number is forty.

### 2.2 The tiers

**[GDC] [COMMUNITY]** The system moves an NPC through a ladder of
representations, described as running from *full AI entities*, to *semi-AI bots
with basic meshes*, down to *basic meshes with basic animations and basic
behaviours*:

| Tier | Brain | Mesh | Animation |
|---|---|---|---|
| Full AI entity | real | high-res | ~**300 bones**, independent, full reactions |
| Semi-AI bot | simplified | basic | reduced |
| Bulk crowd | shared group rules | basic | ~**11 bones**, moves in simplified groups |

A **pooling system** swaps a low-res NPC for a high-res one as the player closes,
timed so the substitution is not noticed. That substitution is the *AI recycling*
of the title.

### 2.3 Why 11 bones is the answer

Per `navigation.md` §10.1: skinning runs on the GPU and scales with **vertices**;
pose evaluation runs on the CPU and scales with **bones**. So the CPU cost of a
crowd is a bone-count problem.

**[inferred]** Cutting 300 → 11 is roughly a **27x** reduction in precisely the
thing that costs, and eleven bones is about spine, head, two arms and two legs —
ample to read as a walking person at distance. They did not speed up animation.
They removed the skeleton from 9,880 characters.

### 2.4 The hard part is persistence

**[GDC]** The talk's claim is *replicated, **persistent**, interactive* NPCs —
and persistence is the difficult half. A pooled NPC that is demoted and later
promoted must not visibly become a different person.

**[inferred]** That implies the identity (appearance seed, role, rough intent)
outlives the *representation*, and only the expensive part — the brain, the
skeleton, the mesh — is recycled. This is the same separation as L4D's Active
Area Set, arrived at differently: a cheap persistent record, an expensive
transient body.

### 2.5 What transfers

- **LOD the brain, not just the mesh.** Everyone LODs geometry; the win here was
  LODing *decision-making* down to shared group rules.
- **Bones are the CPU axis.** If a crowd is CPU-bound, cut bones before anything
  else.
- **Identity is cheap, embodiment is expensive.** Keep the first, recycle the
  second.

---

## 3. World War Z — the physical horde

Least well documented of the three; most of what follows is press and developer
interview rather than a technical talk, so it is tagged accordingly.

### 3.1 The numbers

**[COMMUNITY]** Saber built the **Swarm Engine** rather than licensing one. It
was *"purposefully built to handle up to 500 zombies on screen at one time"*,
and the shipped game is reported rendering **over a thousand**. Saber have since
said next-gen consoles run it *"amazingly"* and could push well past WWZ's ~1K.

### 3.2 What makes it different from the other two

**[COMMUNITY]** The horde *"moves as one, then splits off as it acquires
targets"* — the developers' own comparison is a school of fish. Its signature
behaviours are **physical**: zombies jam in narrow corridors, climb over each
other, and form ramps out of their own bodies to reach higher ground.

**[inferred] None of that is navigation, and no navmesh can express it.** "Stand
on your friend" is not a walkable surface. It falls out of a dense local
simulation where agents genuinely collide, push and stack — which is why WWZ's
horde is a *physics* problem where AC Unity's crowd is an *animation* problem.

### 3.3 How it is afforded

**[COMMUNITY]** The reported optimisations are all the same shape — full fidelity
near, approximation far:

| | Near the player | In the mass |
|---|---|---|
| Collision | detailed, per-body | **simplified mass interaction** |
| Ragdolls | real physics bodies | **recycled** from a pool on death |
| Behaviour | individual target acquisition | group/flock rules |

**[inferred]** "Simplified mass interactions in large crowds instead of detailed
collisions" is the load-bearing one. A thousand rigid bodies in mutual contact is
a solver problem that does not fit a frame; treating the interior of the crowd as
a *density field* that pushes back, and reserving real contacts for the boundary
the player touches, is what makes the pile-ups affordable **and** is why they
still look right — a crowd crushing through a doorway reads as pressure, not as a
thousand resolved contacts.

### 3.4 What transfers

- **Physical crowd behaviour is a physics LOD**, not an AI feature.
- **Recycle expensive resources** (ragdoll bodies) rather than allocating them —
  the same instinct as AC Unity's NPC pool, one level down.
- **The count a game advertises is set by what each agent is permitted to do.**
  500 high-fidelity WWZ zombies and 10,000 AC Unity citizens are not comparable
  units.

---

## 4. Read together

### 4.1 All three built the same thing

| | Cheap population | Expensive population | Transition |
|---|---|---|---|
| **L4D** | none — a small pool, recycled | ~30 active infected | AAS window sweeps; spawn/despawn out of sight |
| **AC Unity** | 9,880 bulk crowd, 11 bones | 40 AI / 120 high-res | proximity pooling, hidden swap |
| **WWZ** | crowd interior, mass physics | boundary agents near the player | proximity, continuous |

**[inferred]** Three teams, three genres, one architecture: **a small expensive
interactive set near the player, a large cheap non-interactive one everywhere
else, and machinery to move agents between them without the player seeing the
seam.** `navigation.md` §11.3 reaches the same structure from the GPU side. It is
the closest thing to a law in this area.

### 4.2 Where they differ — and it is the *cheap* tier that differs

- **L4D's cheap tier does not exist.** It deletes distant population outright.
  Viable because it is a corridor game: the player cannot see behind them.
- **AC Unity's cheap tier is visual.** Ten thousand bodies must be *seen*, so
  they are kept and stripped to 11 bones and shared behaviour.
- **WWZ's cheap tier is physical.** The mass must *push*, so it is kept as a bulk
  medium rather than as individuals.

**[inferred] So the design question is not "how do I get more agents" — it is
"what do the distant ones still have to do?"** Be seen, push, or nothing at all.
That answer determines the whole architecture, and it is a *design* answer before
it is a technical one.

### 4.3 The transition is the actual engineering

Each team's talk spends its time on the swap, not the tiers. Recycling that is
noticed is worse than no recycling. The three rules that recur:

1. **Never transition in view** (L4D makes it explicit; AC Unity's pooling is
   timed for it).
2. **Persist identity across the transition** — the cheap record outlives the
   expensive body.
3. **Hysteresis.** An agent right at the boundary must not flicker between tiers;
   promote and demote at different distances.

**[inferred]** Point 3 is not stated in any of the three sources but is implied by
all of them, and it is the classic way a first implementation of this fails.

---

## 5. What this project would take

**[inferred]** Nothing here is built. Ordered by what would matter first.

- **Flow distance is free and worth having early.** The lattice already supports
  it — a Dijkstra from the entry point, cached. It answers "ahead or behind",
  "how far along is the player", "where should reinforcements arrive" with
  arithmetic instead of searches. Cheapest good idea in this document.
- **A moving active window** is a game-side concept and belongs with the roster,
  not the engine.
- **Tiered agents need identity split from embodiment** before anything else.
  `Entity` plus components is already close: a persistent record with an
  expensive `BodyComponent` that could be attached and detached is exactly the
  shape AC Unity's pooling needs.
- **Do not build any of it until a project has a crowd.** All three of these are
  answers to a problem this tile game does not have.

---

## Sources

**Left 4 Dead**
- [The AI Systems of Left 4 Dead](https://cdn.cloudflare.steamstatic.com/apps/valve/2009/ai_systems_of_l4d_mike_booth.pdf) — Michael Booth, Valve, AIIDE 2009 (PDF, primary source for §1). [Readable transcription](https://www.readkong.com/page/the-ai-systems-of-left-4-dead-michael-booth-valve-9664541)
- [L4D Nav Meshes](https://developer.valvesoftware.com/wiki/L4D_Level_Design/Nav_Meshes) — Valve Developer Community
- [AI navigation in Left 4 Dead](https://gamingme.wordpress.com/2010/02/25/ai-navigation-in-left-4-dead-part-i/) — reactive path following
- [The Left 4 Dead AI Director](https://www.centerconsulting.com/ai-library/concepts/l4d-director)

**Assassin's Creed Unity**
- [Massive Crowd on Assassin's Creed Unity: AI Recycling](https://gdcvault.com/play/1022411/Massive-Crowd-on-Assassin-s) — Francois Cournoyer, GDC 2015. [Free recording](https://archive.org/details/GDC2015Cournoyer) · [Game Developer write-up](https://www.gamedeveloper.com/programming/video-behind-the-massive-crowds-of-i-assassin-s-creed-unity-i-) · [session notes](https://craigw1701.tumblr.com/post/139350868785/massive-crowd-on-assassins-creed-unity-ai)
- [Developing Systemic Crowd Events in Assassin's Creed Unity](https://www.youtube.com/watch?v=FaV88JAWnbQ) — Christine Blondeau, the design side

**World War Z**
- [Swarm Engine](https://www.moddb.com/engines/swarm-engine)
- [Six Things To Know About World War Z](https://gameinformer.com/preview/2019/03/29/six-things-to-know-about-world-war-z)
- [Next-gen consoles run the Swarm Engine 'amazingly'](https://wccftech.com/next-gen-consoles-run-the-swarm-engine-amazingly-could-render-a-lot-more-than-wwzs-1k-zombies/) — the ~1K figure

**Related notes in this directory**
- [`navigation.md`](../agents/navigation.md) — the spatial and navigation layers underneath all of this, plus the GPU route and the two-population split from the rendering side
