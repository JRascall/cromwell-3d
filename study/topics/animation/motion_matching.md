# Motion matching — replacing the animation graph with a search

What it actually is, read from Unreal's shipping implementation; what the
textbook description leaves out; and an honest verdict on whether it belongs in
a grid-based tactics game.

Read against [`source2_animation.md`](../../games/valve/source2_animation.md), which is the
*authored* answer to the same problem. The two notes are a pair, and the
interesting content is the trade between them.

## The verdict, first

**I do not think this project should build motion matching**, and §8 says why at
length. It is the right tool for free-form third-person locomotion driven by an
analogue stick, with a mocap budget, viewed close. This game is none of those.

But three specific mechanisms inside it are worth taking, and one of them
applies to this project's **tactical AI** rather than its animation (§8.2). So
the note is written for what is transferable, not as a build proposal.

## Sourcing

| Tag | Meaning |
|---|---|
| **[UE-SRC]** | Read from `C:/Program Files/Epic Games/UE_5.7/Engine/Plugins/Animation/PoseSearch/`. Names, defaults and structure are Epic's |
| **[TALK]** | A conference talk — cited, not read here |
| **[inferred]** | My reasoning |

Unreal ships the whole system as readable C++, which makes this one of the
better-documented animation techniques despite there being almost no written
material about it.

---

## 1. The idea, for someone who has not seen it

In an animation graph ([`source2_animation.md`](../../games/valve/source2_animation.md)) a human
decides, in advance, every transition: *if `b_grounded` goes false, leave the
run state, play `jump_start`, crossfade 0.2s.* 219 states and 399 transitions
later you have a character, and adding a new movement style means editing the
graph everywhere it could occur.

Motion matching throws that away and asks a different question, every so often:

> Out of every frame of animation I own — tens of thousands, unlabelled — which
> single frame should I be playing **right now**?

Then it plays from there, and asks again shortly after.

The answer comes from a **cost function** with two intuitions:

1. **Don't pop.** Prefer frames whose pose resembles the pose the character is
   already in — similar joint positions, similar joint velocities.
2. **Go where I'm going.** Prefer frames whose *future* motion matches the
   trajectory the player is asking for — where will this clip be in 20, 40, 60
   frames, versus where do I want to be?

Sum those, take the minimum, blend in. No states, no transitions, no conditions.
The character's animation becomes a **nearest-neighbour query in a database of
motion**.

The pitch is that it dissolves the exact failure
[`source2_animation.md`](../../games/valve/source2_animation.md) §4.4 warns about — *"combinatorial
explosion, or, in layman's terms, spider web syndrome"* — because there is
nothing to explode. You never author a transition. New movement is new *data*.

---

## 2. What is actually compared

The textbook says "pose and trajectory". Unreal's schema is built from
composable **feature channels**, and the real list is longer and more revealing
**[UE-SRC]**:

```
PoseSearchFeatureChannel_Pose            PoseSearchFeatureChannel_Trajectory
PoseSearchFeatureChannel_Position        PoseSearchFeatureChannel_Velocity
PoseSearchFeatureChannel_Heading         PoseSearchFeatureChannel_Distance
PoseSearchFeatureChannel_Phase           PoseSearchFeatureChannel_Curve
PoseSearchFeatureChannel_TimeToEvent     PoseSearchFeatureChannel_PermutationTime
PoseSearchFeatureChannel_SamplingTime    PoseSearchFeatureChannel_Group
PoseSearchFeatureChannel_Padding         PoseSearchFeatureChannel_FilterCrashingLegs
```

Three of these are worth stopping on because they are not in any description of
the technique I have seen:

- **`FilterCrashingLegs`** — a channel whose entire job is to reject candidate
  poses whose legs would intersect. That is a *correctness* constraint smuggled
  into a similarity metric, and it exists because the search will otherwise
  cheerfully pick a physically impossible transition that scores well on
  everything else. [inferred, from the name and its presence alongside genuine
  similarity channels.]
- **`TimeToEvent`** — match on *how long until something happens*. This is how
  you get a footplant, a weapon contact or a hit reaction to land on the right
  frame instead of merely nearby.
- **`Phase`** — gait phase, so a left-foot-down frame does not get matched to a
  right-foot-down frame with an otherwise similar pose.

**The schema is the design.** Choosing channels and their weights is the whole
authoring surface — it is what replaces the graph, and it is where a motion
matching setup is good or bad. That is a genuinely different skill from
authoring transitions, and it is worth being honest that it is not obviously an
easier one.

---

## 3. What the textbook description gets wrong

Reading `AnimNode_MotionMatching.cpp` **[UE-SRC]**, the naive picture — "search
every frame, jump to the best result" — is wrong in five ways, and each
correction is a lesson.

### 3.1 It does not search every frame

```cpp
const bool bSearch = !bCanAdvance || (MotionMatchingState.ElapsedPoseSearchTime >= SearchThrottleTime);
```

`AnimNode_MotionMatching.cpp:189` **[UE-SRC]**

There is a **`SearchThrottleTime`**. Between searches the character simply keeps
playing. The search is a periodic re-decision, not a per-frame one — which
halves the cost argument against the technique, and is the first thing to know
before assuming it is unaffordable.

### 3.2 There is an explicit "keep doing what you're doing" candidate

`GetContinuingPoseSearchResult()`, `bIsContinuingPoseSearch`, `bCanAdvance`
**[UE-SRC]**. The frame you would naturally advance to is entered as a
*candidate in the search*, not as a special case. So "continue" and "jump" are
compared on the same terms.

And it gets a discount:

```cpp
float ContinuingPoseCostBias = -0.01f;
float BaseCostBias           =  0.f;
float LoopingCostBias        = -0.005f;
```

`PoseSearchDatabase.h:530-540` **[UE-SRC]**

**`ContinuingPoseCostBias` is negative — a bribe to stay put.** Without it the
search would jitter between near-equal candidates every time it ran, because
real cost functions have plateaus and noise. This is hysteresis, and the
magnitude is tiny (0.01) because it only needs to break ties, not to dominate.

`LoopingCostBias = -0.005` similarly nudges toward looping clips, which are the
stable long-term choice for sustained states like idle or a steady run.

### 3.3 It remembers what it recently played

`FPoseIndicesHistory`, `PoseReselectHistory`, and a per-entry
`bDisableReselection` **[UE-SRC]**. Recently-selected poses are penalised or
excluded, so the system cannot lock onto one small neighbourhood of the database
and replay it. Without this, a large library collapses to the handful of frames
that happen to sit at a cost minimum.

[inferred] This is the same failure as any greedy selector with a static scoring
function, and the same fix: **penalise recency, or the optimum becomes a rut.**

### 3.4 It blends inertially, not by crossfade

`bUseInertialBlend` **[UE-SRC]**. Rather than playing both clips and
cross-fading — which needs both evaluated, and which smears detail — inertial
blending records the pose *difference* at the moment of the switch and decays it
to zero over the blend time. One clip plays; the residual is subtracted.

This matters more than it sounds, and it is the mechanism I would take first
(§8.1): the cost is independent of how many transitions are in flight, and the
result preserves the incoming animation's detail instead of averaging it away.

### 3.5 Mirroring doubles the database for free

`bIsMirrored`, `GetMirrorDataTable` **[UE-SRC]**. A clip can be matched and
played mirrored, so a library of right-foot-lead turns also supplies left-foot
ones. Cheap, and it directly attacks the technique's biggest weakness — data
coverage.

---

## 4. Making the search affordable

The database is tens of thousands of feature vectors of maybe 30-60 dimensions.
Brute force is possible and Unreal supports it, but the shipped path is
**[UE-SRC]**, from `PoseSearchIndex.h:582-624`:

```cpp
// array containing the data of FSearchIndexBase::Values encoded in PCA space. used to create the kdtree.
TAlignedArray<float>       PCAValues;
FSparsePoseMultiMap<int32> PCAValuesVectorToPoseIndexes;
TAlignedArray<float>       PCAProjectionMatrix;
FKDTree                    KDTree;
float                      PCAExplainedVarianceEditorOnly;
```

So the pipeline is:

1. Extract a feature vector per pose.
2. **Project into PCA space** — `PCAProjectionMatrix`, `PCAProject()`,
   `GetNumberOfPrincipalComponents()`. Reduce 40-odd correlated dimensions to a
   handful of meaningful ones.
3. **Build a KD-tree in the reduced space** and query nearest neighbours there.

`PCAExplainedVariance` being surfaced to the editor is the tell that choosing
the component count is a tuning decision with a quality trade-off — keep too
few and the tree returns the wrong neighbours.

There is also **deduplication**: the comment notes `Values` and `PCAValues` are
*"pruned out from duplicate data"*, which breaks the 1:1 pose↔vector mapping and
requires `FSparsePoseMultiMap` to invert **[UE-SRC]**. Mocap contains enormous
numbers of near-identical frames — standing still is thousands of them — and
storing one and mapping many poses to it is free compression.

[inferred] The general shape here is worth noting independently of animation:
**high-dimensional nearest-neighbour is made tractable by reducing dimensions
first, not by a better tree.** A KD-tree in 40 dimensions is barely better than
linear scan; in 6 it is excellent. Reduce, then index.

---

## 5. Where the data comes from

Blendspaces are sampled into the database on a grid —
`NumberOfHorizontalSamples = 9`, `NumberOfVerticalSamples = 2`, `bUseGridForSampling`,
`bUseSingleSample` **[UE-SRC]**. So existing authored blendspaces can be
*converted* into database entries rather than discarded, which is how a project
migrates incrementally instead of recapturing everything.

`BranchInId` and `PoseSearchEvent` **[UE-SRC]** provide hooks for gameplay to
steer or force the search — because a purely emergent system cannot be told
"play the door-opening animation now", and every shipped game needs to say that.

---

## 6. Read against the AnimGraph

| | AnimGraph ([`source2_animation.md`](../../games/valve/source2_animation.md)) | Motion matching |
|---|---|---|
| Where the logic lives | 81 state machines, 399 transitions, authored | A cost function and a schema |
| Adding a movement style | Edit the graph everywhere it applies | Add data |
| Failure mode | Combinatorial explosion; a missed transition | Bad nearest match — plausible but wrong |
| Debugging | "Which transition fired?" — visible in the tool | "The cost preferred frame 14,203" |
| Authoring surface | Transitions, conditions, blend times | Channel weights, biases, data coverage |
| Data required | Clips for the states you authored | A library that covers the space |
| Determinism / rewind | Explicit state, easy to serialise | A search result plus history |
| Foot placement, weight | Whatever you authored and blended | Correct by construction — it is real capture |

**The honest summary:** motion matching does not remove the work, it moves it
from *authoring transitions* to *curating data and tuning a metric*. Whether
that is a win depends entirely on whether you have the data and whether your
movement space is large enough that authoring it was the bottleneck.

The last row of that table is why it wins where it wins. A blend tree
interpolating between a walk and a run produces a gait that is *neither*, with
sliding feet. Motion matching plays a real captured frame, so contact and weight
are right because they were measured — the same argument as
[`surface_depth.md`](../surfaces/surface_depth.md) §2 about photogrammetry, in a different
domain. **Measured data beats interpolated data, and the technique's job is
just to pick the right measurement.**

---

## 7. Where it came from, and where it went

- **Simon Clavet and Michael Büttner, *Motion Matching — The Road to Next Gen
  Animation*, Nucl.ai 2015** **[TALK]** — the origin, out of Ubisoft.
- **Kristjan Zadziuk, GDC 2016** **[TALK]** — the talk that popularised it,
  on *For Honor*.
- **Daniel Holden et al., *Learned Motion Matching*, SIGGRAPH 2020** (Ubisoft
  La Forge) **[TALK]** — replaces the database and the search with trained
  networks that reproduce the same behaviour at a fraction of the memory. This
  is the direction the technique went: the database is the cost, so compress it
  into weights.
- **Unreal**: the `PoseSearch` plugin (§2-§5), plus Epic's **Game Animation
  Sample** project — which is on this machine at
  `E:/Game Development/GameAnimationSample` and is the official worked example.

---

## 8. The verdict for `cromwell`

### 8.1 Why not

Judged against this project specifically, four things count against it, and none
are about implementation difficulty:

1. **The input space is tiny.** Motion matching earns its keep when a player can
   ask for any trajectory at any speed in any direction — an analogue stick in a
   third-person game. Here, units move along **grid-constrained paths at
   controlled speeds**. The trajectory term, which is half the cost function,
   has almost no variation to match against. Most of the technique is answering
   a question this game does not ask.
2. **The camera hides the payoff.** The fidelity motion matching buys — exact
   foot placement, weight shift, plausible turn-in-place — is a close-range,
   character-focused benefit. A fixed oblique tactical camera at distance
   throws most of it away.
3. **It is a data technique, not a code technique.** The system is only as good
   as the capture library. Building the runtime is the small part; the cost is
   mocap, cleanup and curation, and that is not a solo-project budget.
4. **The problem it solves does not exist here.** Spider-web syndrome comes from
   a large free-form movement space. Grid movement with a handful of states —
   idle, move, turn, fire, take cover, die — is exactly the case an authored
   graph handles well, and §6's table says a graph is *better* at determinism,
   debugging and serialisation, all of which matter for a tactics game with
   deterministic simulation.

### 8.2 What to take anyway

Three mechanisms, and the third is the one I would actually act on:

1. **Inertial blending instead of crossfading** (§3.4). Independent of motion
   matching entirely. It costs one clip evaluation instead of two, it preserves
   detail rather than averaging it, and its cost does not grow with the number
   of overlapping transitions. This is worth adopting when ozz-animation is
   wired in, whatever the animation architecture above it looks like.

2. **Dimensionality reduction before indexing** (§4). If this engine ever does
   nearest-neighbour over feature vectors — animation, tactical scoring,
   anything — the lesson is that a KD-tree in 40 dimensions is close to useless
   and in 6 is excellent. **Reduce, then index.** Currently `SpatialHash` deals
   in 3D positions where this does not arise, so this is a "know it when the
   time comes" item.

3. **The continuing-pose cost bias, applied to tactical AI** (§3.2). This is the
   transferable idea and it has nothing to do with animation.

   Motion matching re-decides periodically, and would jitter between near-equal
   options without a small negative bias on "what I chose last time". `-0.01`,
   just enough to break ties.

   This project's AI re-scores candidates every time it decides —
   `spatial_queries.md` §3.6's cover scoring, target selection, ability
   placement. **Any per-frame or per-turn re-decision over a scored candidate
   set has exactly this failure mode**: two cover positions score 0.71 and 0.70,
   noise flips them, and the unit oscillates. The fix is the same and it is one
   line — **give the incumbent a small discount**. It is cheap, it is invisible
   when unnecessary, and its absence produces a bug that reads as "the AI is
   indecisive" and gets misdiagnosed as a pathfinding or scoring problem.

   Worth adding to the target-selection and cover-scoring rules in CLAUDE.md
   alongside "sort before the expensive test", because it is the same class of
   guidance: **incumbency is information, and a scorer that ignores it thrashes.**

---

## 9. What I did not verify

- The cost function's actual assembly and per-channel weighting maths — I read
  the channel list and the biases, not the accumulation.
- `KDTree.cpp`'s implementation and query behaviour.
- Whether `SearchThrottleTime` has a meaningful default, or is always
  project-set.
- Learned Motion Matching (§7) — cited from its publication, not read.
- Epic's Game Animation Sample — present on disk, not opened.

---

## Sources

**[UE-SRC]** — Unreal Engine 5.7,
`C:/Program Files/Epic Games/UE_5.7/Engine/Plugins/Animation/PoseSearch/Source/Runtime/`:

| Area | Path |
|---|---|
| The node: throttling, continuing pose, blending, mirroring | `Private/AnimNode_MotionMatching.cpp` |
| Feature channels (14 of them) | `Private/PoseSearchFeatureChannel_*.cpp`, `Public/PoseSearch/PoseSearchFeatureChannel_*.h` |
| PCA, KD-tree, dedup | `Public/PoseSearch/PoseSearchIndex.h`, `Private/KDTree.cpp`, `Private/PoseSearchEigenHelper.h` |
| Cost biases, blendspace sampling | `Public/PoseSearch/PoseSearchDatabase.h` |
| Indexing and sampling | `Private/PoseSearchAssetIndexer.cpp`, `Private/PoseSearchAssetSampler.cpp` |

**[TALK]** — cited, not read here:

- Simon Clavet & Michael Büttner, *Motion Matching — The Road to Next Gen Animation*, Nucl.ai 2015
- Kristjan Zadziuk, *Motion Matching: The Future of Games Animation… Today*, GDC 2016
- Daniel Holden, Oussama Kanoun, Maksym Perepichka, Tiberiu Popa, *Learned Motion Matching*, SIGGRAPH 2020

**Also on disk, unopened:** `E:/Game Development/GameAnimationSample` — Epic's
official motion matching sample project.

**Related notes:** [`source2_animation.md`](../../games/valve/source2_animation.md) — the authored
alternative, and the source of §6's comparison;
[`networked_animation_physics.md`](networked_animation_physics.md) §2.3 — why
§6's "determinism / rewind" row matters;
[`spatial_queries.md`](../agents/spatial_queries.md) §3.6 — the candidate-scoring rules
§8.2's hysteresis idea belongs with.
