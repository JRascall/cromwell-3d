# Animation and physics over the network

What actually crosses the wire when a character animates or a physics object
moves, and why the two are solved in opposite directions.

Companion to [`valve_networking.md`](valve_networking.md) (one server, one
tick) and [`mmo_architecture.md`](mmo_architecture.md) (who owns what). This
note is the third question: **given that some machine has authority, what does
it actually send?**

Source tags as in the sibling notes: **[VALVE-SDK]** is transcribed from the
Source SDK 2013 tree; **[EPIC]** is Epic's documentation; **[GDC]** is a
conference talk by the people who built it; **[inferred]** is my reasoning.

---

## 1. The thesis, up front

**Animation is replicated as *intent*, and reconstructed locally. Physics is
replicated as *state*, and blended locally.**

That asymmetry is not arbitrary. It follows from one property:

|  | Deterministic from a small input? | So you send… |
|---|---|---|
| **Animation** | **Yes.** "Sequence 14, cycle 0.3, weight 1.0" reproduces the pose exactly on any machine with the same asset. | The *description*. A few bytes. |
| **Physics** | **No.** Contacts, friction, solver iteration order and floating-point differences diverge, and the divergence compounds. | The *result*. And then you lie about it. |

Everything below is a consequence. The failure mode for animation is sending
too much; the failure mode for physics is believing you can send too little.

---

## 2. Animation — nobody sends bone poses

### 2.1 The rule, and Epic stating it plainly

A skeleton is 60-100 bones; a bone is a position and a rotation. Sending poses
is roughly a kilobyte per character per tick before compression, and it is
**completely unnecessary**, because the receiving machine has the same
animation data and can evaluate it itself.

So what crosses the wire is the animation's *identity and phase*. Epic's
documentation is unusually blunt about the consequence **[EPIC]**:

> The server and the autonomous proxy on the owning client **do not check to
> see that they are playing the same animation, as this is normally considered a
> cosmetic feature**, so you must program your gameplay logic to ensure that any
> AnimMontages are triggered correctly on all machines.

Read that carefully. Unreal does not replicate animation at all in the general
case. It replicates the *event that triggers* it — a montage play via RPC, or
a Gameplay Ability — and then trusts every machine to arrive at the same place.
Animation is explicitly categorised as **cosmetic**, and keeping it consistent
is the game programmer's problem, not the engine's.

The one exception Epic do handle is **root motion**, because root motion moves
the character, and character position is not cosmetic. `FSavedMove_Character`
records the montage it came from and the track position within it **[EPIC]** —
so root motion joins the movement prediction and correction system, and the rest
of the animation does not.

**This is the correct split and the whole design in one line: animation that
only affects pixels is fire-and-forget; animation that affects the simulation
is simulation.**

### 2.2 Source's version, and why it is more careful

TF2 sends an activity-derived sequence number, an **animation parity** counter,
and a playback rate **[VALVE-SDK]** — see
[`source_fps_viewmodel.md`](source_fps_viewmodel.md) §8.4 for the full chain.
The parity counter exists because a repeated sequence number cannot signal a
restart, which is the one bug every naive "replicate the animation index"
implementation ships with.

But TF2 goes further than Unreal in a way that matters, and it is driven by
lag compensation. From [`valve_networking.md`](valve_networking.md) §7.3, the
server records, every tick, per player:

```cpp
for ( layerIndex = 0; layerIndex < layerCount; ++layerIndex )
{
    record.m_layerRecords[layerIndex].m_cycle    = currentLayer->m_flCycle;
    record.m_layerRecords[layerIndex].m_order    = currentLayer->m_nOrder;
    record.m_layerRecords[layerIndex].m_sequence = currentLayer->m_nSequence;
    record.m_layerRecords[layerIndex].m_weight   = currentLayer->m_flWeight;
}
record.m_masterSequence = pPlayer->GetSequence();
record.m_masterCycle    = pPlayer->GetCycle();
for ( i = 0; i < MAXSTUDIOPOSEPARAM; i++ )
    record.m_flPoseParameters[i] = pPlayer->GetPoseParameter(i);
```

`game/server/player_lagcompensation.cpp:319-337` **[VALVE-SDK]**

**Still not poses.** Sequence, cycle, weight, order, pose parameters — the
*description* — from which the pose is reconstructed by running the same
animation evaluation. Four numbers per layer instead of 100 bone transforms.

### 2.3 The line: when animation stops being cosmetic

Epic's "normally considered a cosmetic feature" carries a hidden condition, and
the condition is the whole design decision:

**Animation is cosmetic right up until something reads the skeleton.** The
moment hitboxes hang off bones, or root motion drives position, or an IK foot
plant affects collision, the pose is simulation state and must be:

1. **Deterministically reconstructible** from what you replicate, and
2. **Reconstructible at an arbitrary past time**, if you rewind for hit
   detection.

Requirement 2 is the one that bites. It is not a networking requirement — it is
a constraint on the **animation system's architecture**. An animation system
that can only tell you the current pose, because it accumulates state
incrementally frame to frame, cannot support server-side rewind at all, and you
will not find that out until you try to add lag compensation years later.

**The property to demand of an animation system is: given (sequence, cycle,
layer weights, pose parameters) and nothing else, produce the pose.** Stateless
evaluation from an explicit description. TF2 has it, which is why its
`LagRecord` is 200 bytes and works. Anything that caches, blends against "last
frame", or advances an internal accumulator breaks it.

### 2.4 What Overwatch did with the same problem

Blizzard built Overwatch on a strict ECS, server-authoritative and
prediction-heavy, and the striking number from Timothy Ford's talk **[GDC]** is
the ratio: of roughly **46 client-side systems and 103 component types, only
three systems handle gameplay netcode** — movement, weapon, and state script.

[inferred, but it is the natural reading] Everything else — including all the
animation, effects and audio systems — is downstream of those three and is
regenerated locally from replicated gameplay state. That is the same
architecture as §2.1 stated as a system count: **a small, explicit,
replicated core, and a large cosmetic layer derived from it.** If your
animation system is in the replicated set, you have made a mistake somewhere
upstream.

---

## 3. Physics — three tiers, and only one of them is hard

The mistake is treating "physics" as one problem. It is three, with completely
different answers.

### 3.1 Tier 1 — cosmetic physics: never replicate it

Ragdolls, debris, cloth, shell casings, destructible chaff. Nobody replicates
these, and nobody should. Each client simulates them locally, they disagree
visibly, and it does not matter — a corpse landing differently on two machines
is not observable as wrong, because there is nothing to compare it against.

TF2's own instinct here is visible in the small: the view model's weapon
suppresses its shadow when not actively held, ragdolls are client-side, and
effects are recreated from events rather than replicated **[VALVE-SDK]**.

**The discipline is to keep this tier as large as possible.** Every object you
can argue into it is an object you do not have to solve §3.3 for. The question
to ask of any physics object is not "how do I replicate this" but "**what would
break if two players saw it differently?**" — and for most debris, honestly,
nothing.

### 3.2 Tier 2 — deterministic gameplay physics: predict and roll back

If the physics *is* the game, and you can make it deterministic, you can treat
it exactly like player movement: simulate everywhere, predict locally,
reconcile against the server.

**Rocket League** is the reference implementation, and its numbers are
instructive **[COMMUNITY, reporting Jared Cone's GDC 2018 talk]**:

| | |
|---|---|
| Physics rate | **120 Hz**, on both client and server |
| Network update rate | **60 Hz** |
| Physics engine | Bullet |
| What is predicted | Not just the local car — **other players' cars and the ball** |
| Reconciliation | Replays the physics scene, essentially continuously |

Two things are worth extracting.

**The physics rate is decoupled from and higher than the network rate.** 120 Hz
physics with 60 Hz updates means the simulation is fine-grained enough to be
stable and reproducible, while the network carries half as much. Coupling those
two numbers — the instinctive thing to do — gives you either an expensive
network or bad physics.

**Everything is predicted, not just you.** This is the decision most teams do
not make. Predicting only the local player means the ball you are about to hit
is 80 ms in the past, and in a game about hitting a ball that is fatal. The cost
is that a correction re-simulates the *whole scene*, not one entity — which is
affordable precisely because the scene is one ball and a few cars.

The precondition is bounded scope. Rocket League can replay the entire physics
scene every frame because the entire physics scene is tiny. **This tier does not
scale**, and knowing that is more useful than the technique.

### 3.3 Tier 3 — chaotic physics: it is not solvable, so blend

The honest tier. From the framing of Matt Delbosc's *Watch Dogs 2* talk
**[GDC]**:

> Accurate peer-to-peer replication of game objects in a highly chaotic physical
> simulation is **essentially an unsolvable problem**.

That is a shipped AAA vehicle programmer saying the problem has no correct
solution, and it is the most useful sentence in this note. Many vehicles,
driven by different players on different machines, colliding with each other and
with a destructible city — the divergence is not a bug to be fixed, it is a
property of the system.

So the goal changes from *correctness* to *plausibility*, and the techniques are
named accordingly **[GDC]**:

| Technique | What it does |
|---|---|
| **Snapshot buffering** | Hold received states in a buffer and render from slightly in the past, so you interpolate between known states instead of guessing |
| **Projective velocity blending** | Dead reckoning that blends toward the extrapolated position using velocity, rather than snapping to the last known one |
| **Physics simulation blending** | Run the local physics *and* the received state, and blend between them — so the object obeys local collisions while being pulled toward the authoritative answer |

The third is the interesting one and it is the general answer to this tier
[inferred, but it is what the name describes]: **do not choose between "simulate
locally" and "follow the network". Do both, and crossfade.** The local
simulation keeps the object out of walls and reacting to nearby collisions; the
network term stops it drifting away from the truth. Neither alone is acceptable
— pure local diverges, pure network interpenetrates geometry and jitters.

Delbosc closed on unsolved issues **[GDC]**, which is the correct posture. Tier
3 is a permanent maintenance cost, not a feature you finish.

### 3.4 Choosing the tier

The decision procedure, in order:

1. **Can it be cosmetic?** Push as hard as possible here (§3.1).
2. **Is the interacting set small and boundable?** Then determinism plus
   rollback (§3.2), and accept that the bound is real.
3. **Otherwise**, blend (§3.3), and accept that it will never be right.

There is no fourth option, and specifically **there is no "just make it
deterministic" for tier 3** — Rocket League's determinism is affordable because
the scene is small, not because Psyonix were more careful.

---

## 4. Who simulates a shared physics object

Tier 2 and 3 both need an owner. Two shipped answers, from
[`mmo_architecture.md`](mmo_architecture.md):

**Destiny elects a physics host** among the peers in an activity, hands off
authoritative simulation state when it leaves, and calls **`reconcile()`** on a
freshly-spun-up simulation to bring it in line with the authoritative summary
**[GDC — Truman, GDC 2015]**.

**Star Citizen transfers entity authority** between server nodes as the entity
crosses a boundary or to rebalance load; only the authoritative node writes
state back, and every other node treats the entity exactly as a client does
**[CIG]**.

The shared shape, and the thing to build once:

> A physics object needs **one authority at a time**, an **explicit handover**,
> and a **`reconcile(fresh_state, authoritative_summary)`** that makes a
> cold-started simulation agree with the truth.

If you have `reconcile()`, then host migration, late join, crash recovery,
rejoining and authority transfer are the *same function*. If you do not, they
are four separate systems that each fail differently. This is the single most
reusable idea across both notes.

---

## 5. What `cromwell` should do

Judged against RTS / FPS / third-person, and against the fact that Jolt is the
chosen physics library for future ragdolls, debris and destruction
(`physics-library-jolt`):

**Do now, because it is architectural and cheap today:**

1. **Make animation evaluation stateless — pose from an explicit description.**
   Given (sequence/clip, cycle, layer weights and order, pose parameters),
   produce the pose, with no dependence on what happened last frame. §2.3. This
   is a constraint to impose *before* ozz-animation is wired in
   (`third-party-library-choices`), because retrofitting it means rewriting
   every consumer. It pays off immediately even single-player: it is also what
   makes animation debuggable, scrubbable in a tool, and testable.

2. **Classify physics objects by tier at the point of creation** (§3.4), as an
   explicit property, not a later discovery. "Cosmetic" should be the default
   and the cheap path. This costs one enum today and is the difference between
   a tractable and an intractable networking retrofit.

3. **Keep the physics rate independent of any future network rate**, and fixed.
   Rocket League's 120/60 split (§3.2) is the shape. A fixed physics timestep is
   already correct practice for stability reasons alone, so this is free.

**Design for, do not build:**

4. **`reconcile()` as a named concept** (§4) — a function that takes a
   cold-started simulation and an authoritative summary and makes them agree.
   Even without networking there is a use: this is exactly what loading a save
   into a running world is.

5. **The cosmetic/simulation line as an explicit boundary in the entity model.**
   Overwatch's 3-of-46 ratio (§2.4) is the target shape: a small replicated
   core, a large derived layer. Knowing which side a system is on should be a
   property of the system, not folklore.

**And the general lesson.** Animation is solved by sending a *description* and
recomputing; physics is unsolvable in general and is managed by sending a
*result* and blending. The distinguishing question is only ever **"is this
cheaply reproducible from a small input?"** — and it is worth asking of any data
that must cross any boundary, network or not. It is the same question behind
this codebase's derived caches: `OcclusionGrid` exists because occlusion is
cheaply reproducible from tiles, and the `kNeedsTile` escape hatch exists
because sometimes it is not, and the honest move is to admit that per-cell
rather than pretend uniformly.

---

## Sources

**[VALVE-SDK]** — Source SDK 2013, local at
`E:/Game Development/Tools/source-sdk-2013-master`:

- `game/server/player_lagcompensation.cpp` — the animation-layer recording, §2.2
- `game/shared/baseviewmodel_shared.cpp` — animation parity

**[EPIC]**:

- [Understanding Networked Movement in the Character Movement Component](https://dev.epicgames.com/documentation/unreal-engine/understanding-networked-movement-in-the-character-movement-component-for-unreal-engine?lang=en-US) — the "normally considered a cosmetic feature" statement and `FSavedMove_Character`
- [Animation Montage in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/animation-montage-in-unreal-engine?lang=en-US)

**[GDC]**:

- [Matt Delbosc — Replicating Chaos: Vehicle Replication in *Watch Dogs 2*, GDC 2017](https://www.gdcvault.com/play/1024597/Replicating-Chaos-Vehicle-Replication-in) — snapshot buffering, projective velocity blending, physics simulation blending ([free on YouTube](https://www.youtube.com/watch?v=_8A2gzRrWLk))
- [Jared Cone — It IS Rocket Science! The Physics of *Rocket League* Detailed, GDC 2018](https://www.gdcvault.com/play/1024972/It-IS-Rocket-Science-The) ([slide PDF](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf))
- [Timothy Ford — Overwatch Gameplay Architecture and Netcode, GDC 2017](https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and)
- [Justin Truman — Shared World Shooter: Destiny's Networked Mission Architecture, GDC 2015](https://archive.org/details/GDC2015Truman)

**Note on §3.2's numbers:** the 120 Hz physics / 60 Hz network figures are
widely reported from Cone's talk but were read here from secondary summaries —
the slide PDF is 182 pages of images and did not text-extract. Treat as
**[COMMUNITY]** until checked against the video.

**Related notes:** [`valve_networking.md`](valve_networking.md),
[`mmo_architecture.md`](mmo_architecture.md),
[`source_fps_viewmodel.md`](source_fps_viewmodel.md) §8 for the animation chain
in full.
