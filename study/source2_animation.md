# Source 2 animation — the AnimGraph, read from shipped graphs

How Source 2 composes a pose: the node graph, state machines, layering, and the
parameter interface game code talks to. Read from **plain-text Source 2 animation
graphs on disk**, including a 2.2 MB production player graph, not from
documentation.

Companion to [`source_fps_viewmodel.md`](source_fps_viewmodel.md) §8, which is
the same question answered by **Source 1** — activity → sequence, overlay
layers, animation parity — and to
[`networked_animation_physics.md`](networked_animation_physics.md), which is
about what crosses the wire.

## Where this came from, and why it is unusually good material

Source 2's animation tooling is not documented publicly. But **s&box** —
Facepunch's game built on Source 2 — ships:

- `bin/win64/tools/animgraph_editor.dll`, Valve's actual AnimGraph editor,
- **21 uncompiled `.vanmgrph` files in plain-text KV3**, with Valve's own class
  names intact,
- a **tutorial series authored inside the format itself**, written by a
  Facepunch developer during a hack week, its prose stored in comment nodes,
- and the **production Citizen player graph** — 2.2 MB, 399 transitions, the
  real thing.

All of it under `E:/SteamLibrary/steamapps/common/sbox/`. Every file carries the
same header:

```
<!-- kv3 encoding:text:version{e21c7f3c-...} format:animgraph2:version{0f7898b8-...} -->
```

So this is **`animgraph2`**, the current format, and both tutorials and
production graphs are the same version.

| Tag | Meaning |
|---|---|
| **[FACEPUNCH]** | The tutorial author's own words, quoted from comment nodes |
| **[S2-ASSET]** | Read directly out of the KV3 — class names, fields and values are Valve's |
| **[inferred]** | My reasoning |

**Caveat stated once, up front.** s&box is Source 2 as Facepunch use it. Where a
convention is theirs rather than Valve's, they say so — e.g. *"on s&box, we use
Parameters first and foremost"* **[FACEPUNCH]**. Node classes and the file
format are Valve's; graph *authoring style* is Facepunch's. I flag the
difference where it matters.

---

## 1. What an AnimGraph is

The author's own framing **[FACEPUNCH]**:

> *"At its core, an Animgraph is a tree of connected nodes; think of this
> metaphor as if the tree were laying on its side, leaves on the left, root on
> the right."*

Leaves are animation clips. The root is a single **Final Pose** node. Every
frame, a pose flows left to right, being blended, masked, offset and corrected
on the way.

Two rules that are pure implementation detail and exactly the kind of thing you
only learn by being bitten:

- **Only one Final Pose node is live**, and if there are several, it is *"the
  earliest-created one"* that works — not the connected one **[FACEPUNCH]**.
  Creation order is load-bearing, which recurs in §4.3.
- A graph is bound to a model in ModelDoc (the `.vmdl`), **or set at runtime
  from game code** — s&box exposes `SetAnimGraph()` **[FACEPUNCH]**.

Sub-graphs exist: `CGroupAnimNode` with matching `CGroupInputAnimNode` /
`CGroupOutputAnimNode` for tidiness (122 groups in the Citizen graph), and
`CSubGraphAnimNode` (11 instances) for genuine reuse across graphs
**[S2-ASSET]**.

---

## 2. The interface — parameters in, tags out

This is the boundary between game code and animation, and it is the single most
reusable thing in this note.

### 2.1 Parameters: game code → graph

> *"Parameters are various values which are sent to us here by game code (usually
> the part of it referred to as an 'Animator')."* **[FACEPUNCH]**

Five types **[FACEPUNCH]**: **BOOL**, **FLOAT**, **INT**, **ENUM** (*"integers
in disguise! Or rather, with labels"*) and **VECTOR**.

The Citizen graph declares **35 float, 18 bool and 9 vector parameters**
**[S2-ASSET]**. The full name list is worth reading as a specification of what
a third-person character animation system actually needs to be told:

| Group | Parameters |
|---|---|
| **Locomotion** | `move_x`, `move_y`, `move_direction`, `move_speed`, `move_groundspeed`, `move_rotationspeed`, `move_style` |
| **Aiming** | `aim_body`, `aim_body_pitch`, `aim_body_yaw`, `aim_body_weight`, `aim_head`, `aim_head_weight`, `aim_eyes` |
| **Weapon hold** | `holdtype`, `holdtype_pose`, `holdtype_pose_hand`, `holdtype_handedness`, `holdtype_attack` |
| **State flags** | `b_grounded`, `b_jump`, `b_swim`, `b_climbing`, `b_noclip`, `b_attack`, `b_reload`, `b_reloading`, `b_reloading_insert`, `b_deploy`, `b_weapon_lower`, `b_long_idle`, `b_firstperson` |
| **Hit reaction** | `hit`, `hit_bone`, `hit_direction`, `hit_offset`, `hit_strength` |
| **Other** | `duck`, `face_override`, `_character_type` |

**[S2-ASSET]**

Observations that transfer regardless of engine:

- **Roughly forty parameters is what a full third-person character costs.** Not
  four, and not four hundred. That is a useful calibration.
- **The naming convention is a type system in disguise** — `b_` prefixes bools,
  and the rest group by concern (`move_`, `aim_`, `holdtype_`, `hit_`). With
  forty untyped names in a UI, prefixes are the only navigation you get.
- **Aim is decomposed into value *and* weight** — `aim_body` and
  `aim_body_weight`, `aim_head` and `aim_head_weight`. The graph is told both
  where to look and how much to care, separately, so gameplay can fade aiming
  out without lying about the direction.
- **`b_firstperson` is a parameter, not a second graph.** One graph serves both
  perspectives. Contrast Source 1, which uses an entirely separate view model
  entity with its own model and animation set
  ([`source_fps_viewmodel.md`](source_fps_viewmodel.md) §9).
- **Hit reactions are parameterised, not animated per-case** — bone, direction,
  offset, strength. That is a procedural system driven by five numbers, and
  there is a `ProceduralHitReactionsList` bone mask to match (§5.2).

### 2.2 Tags: graph → game code, and graph → graph

> *"Tags are ways for the graph to communicate back to the game code, and they
> can also be used for the graph to communicate with itself!"* **[FACEPUNCH]**

`CStringAnimTag` and `CEventAnimTag`, managed by `CAnimTagManager`, spanning
time via `CAnimTagSpan` (60 in the Citizen graph) **[S2-ASSET]**.

The second half of that sentence is the interesting one. **181
`CTagCondition`s** appear in the Citizen graph — tags are used as transition
conditions more often than time conditions (69) and nearly as often as
parameter conditions (265) **[S2-ASSET]**. So a tag is not merely an outbound
event; it is how *"completely unrelated SMs [state machines] interact with one
another"* **[FACEPUNCH]** without either knowing about the other.

[inferred] This is a publish/subscribe bus inside the animation graph, and it
exists because the alternative — every state machine reading every other one's
state directly — is the coupling that makes large graphs unmaintainable.

---

## 3. What a shipped graph actually looks like

The Citizen graph, by node class **[S2-ASSET]**. This is a real production
full-body player character, so the proportions are evidence, not opinion:

| Count | Class | What it is |
|---|---|---|
| 399 | `CAnimStateTransition` | state machine transitions |
| **282** | **`CAnimInputDamping`** | **input smoothing — see §6** |
| 265 | `CParameterAnimCondition` | conditions on parameters |
| 226 | `CSequenceAnimNode` | an animation clip |
| 219 | `CAnimState` | a state |
| 181 | `CTagCondition` | conditions on tags |
| 176 | `CSingleFrameAnimNode` | one frozen frame, usually an additive base |
| 122 | `CGroupAnimNode` (+in/out) | organisation |
| 114 | `CCommentAnimNode` | comments |
| 113 | `CLookAtAnimNode` | look-at |
| 108 | `CBlendAnimNode` | blend two poses |
| 86 | `CAddAnimNode` | additive layering |
| 81 | `CStateMachineAnimNode` | a state machine |
| 77 | `CBoneMaskAnimNode` | masked blend |
| 69 | `CTimeCondition` | time-based conditions |
| 51 | `CSelectorAnimNode` | pick input by bool/int/enum |
| 48 | `CFinishedCondition` | "when the animation ends" |
| 24 | `CChoiceAnimNode` | weighted random pick |
| 23 | `CAimMatrixAnimNode` | aim offset |
| 15 | `CSubtractAnimNode` | build an additive at runtime |
| 14 | `CTwoBoneIKAnimNode` | two-bone IK |
| 11 | `CSubGraphAnimNode` | reuse another graph |

Three things jump out.

**81 state machines and 219 states — under three states each.** Nobody built one
big machine. §4.4 explains why.

**The second-most-common node is input smoothing** (282), outnumbering the
animation clips it feeds (226). §6.

**176 single-frame nodes against 86 additive nodes.** Additive layering needs a
reference pose to subtract against, and a frozen frame is how you get one.
[inferred, but the 2:1 ratio and the co-occurrence with `CSubtractAnimNode` make
it hard to read otherwise.]

---

## 4. State machines

> *"Selectors are just very simple State Machines... it's time to look at a real
> State Machine, which is your primary means of putting 'logic' into the
> graph."* **[FACEPUNCH]**

States, transitions between them, and **conditions** on transitions: *"If **all**
the conditions on a transition are met, then it will be taken"* **[FACEPUNCH]**
— conjunction only, no OR. To express OR you add a second transition, which is
why there are 399 transitions for 219 states.

Four condition types ship **[S2-ASSET]**: `CParameterAnimCondition`,
`CTagCondition`, `CTimeCondition`, `CFinishedCondition`, plus
`CControlValueCondition`.

### 4.1 On Finished vs On Almost Finished — the one that matters

The most valuable paragraph in the whole tutorial series, and a bug that ships
in real games **[FACEPUNCH]**:

- **On Finished** — *"[ANIMATION B] only starts fading in once [ANIMATION A] is
  fully over. Its last frame lingers during the fade-in... you can see the
  character suddenly freezing in their tracks before they slowly resume motion.
  (Especially noticeable looking at the feet.)"*
- **On Almost Finished** — *"The fade-in of [ANIMATION B] starts at [**END** OF
  ANIMATION A] *minus* [BLEND DURATION], making it a cross-fade. There is no
  time during which a frame of either animation stays 'stuck'."*

And the verdict: *"you want to be using 'On Almost Finished' almost all the
time."* **[FACEPUNCH]**

That is the difference between animation that reads as fluid and animation that
hitches, and it is one checkbox. The constraint: the crossfade duration must not
exceed either animation's length, *"otherwise it will look wrong (and this
tool's console will warn you)"* **[FACEPUNCH]** — the tool validates it, which
is the right place to catch it.

Corollary, stated plainly: *"If your animation is looping, then by definition,
it can never finish."* Use a control value condition on the cycle instead
**[FACEPUNCH]**.

### 4.2 Reset signals, and the "reset blocker"

State machines emit **reset signals**; a node receiving one returns to its
initial state — a state machine to its entry state, an animation to cycle 0
**[FACEPUNCH]**. Useful, and dangerous:

> *"resetting is **instant**, and will create a visible snap if it's not
> happening when & where it should be."* **[FACEPUNCH]**

The fix is a genuinely clever piece of tool abuse **[FACEPUNCH]**: take a
`CBoneMaskAnimNode`, wire the same input into *both* of its inputs, give it an
empty weight list so it masks nothing, and untick its `Reset Child1` /
`Reset Child2` settings. Now it passes the pose through unchanged and swallows
resets — *"a 'one-node firewall' for resets: a reset blocker."*

**And the production graph uses it.** The Citizen graph contains bone mask nodes
named `"Reset blocker"` with `m_weightListName = "_empty_anim_weightlist"`, 13
of them **[S2-ASSET]**. The tutorial documents a workaround that shipped.

[inferred] The existence of this idiom says reset propagation is not
individually controllable per-edge — you can only block it with an intervening
node. That is a design smell worth avoiding if you build the equivalent: **make
signal propagation a property of the connection, not something you interpose a
node to stop.**

### 4.3 Transition order is creation order — and it is a real bug source

Transitions out of a state are evaluated **in the order they were created**, and
*"By default, a newly-created transition gets put last"* **[FACEPUNCH]**. The
worked example:

> 1) if `bool_A`, state 1 → state 2
> 2) if `bool_B`, state 1 → state 3
> 3) if `bool_A` **AND** `bool_B`, state 1 → state 4
>
> *"transition 3 is evaluated last. And because it happens to be using the same
> Parameters as the other two transitions, it never gets a chance to be
> evaluated."*

The rule that follows **[FACEPUNCH]**: *"order your transitions so that the ones
with the most conditions are checked first, and the ones with the least are
checked last."*

**Most-specific-first.** This is the same hazard as an if/else-if chain whose
broadest case is written first, promoted into a visual tool where the ordering
is invisible — the author notes you can only see target state, condition count
and colour, with *"no mouseover functionality"*, and resorts to *"temporarily
add a different-coloured transition"* to work out where you are in the list
**[FACEPUNCH]**.

[inferred] The lesson for anyone building such a system: **if evaluation order
is semantically significant, it must be visible and reorderable.** Hiding it
behind creation order guarantees this bug.

### 4.4 Nesting, and combinatorial explosion

Why 81 state machines instead of one:

> *"The more states you have... the more you start running into the danger of
> 'combinatorial explosion', or, in layman's terms, spider web syndrome. Having
> a state machine be an input for another state machine is sort of like
> automatically reusing a lot of transitions implicitly, instead of needing to
> make sure that different copies of the same explicitly-defined transitions are
> always the same (which is tough when there are many; you **will** miss one and
> then later wonder why stuff doesn't work out). But most importantly it lets
> you have a clean separation between what might trigger an exit animation for a
> particular state, vs. what would bypass that exit animation."* **[FACEPUNCH]**

The recurring shape, given for the airborne case **[FACEPUNCH]**: *two entries*
(jumped, or fell without jumping), *an ongoing state*, *an exit* (landing). That
enter/ongoing/exit triple is then itself a state inside a higher machine.

For a nested machine's completion to be usable upstream it needs **at least one
end state**; once there, *"the timing info will get passed downstream... and the
finished condition(s) will be able to make use of it"* **[FACEPUNCH]**.

---

## 5. Layering — the direct answer

Four mechanisms, and they are distinct.

### 5.1 Blend — `CBlendAnimNode` (108)

Two poses, one weight. The plain crossfade.

### 5.2 Bone mask — `CBoneMaskAnimNode` (77)

The workhorse of layering, and **more than a mask**. Its fields **[S2-ASSET]**:

| Field | Purpose |
|---|---|
| `m_inputConnection1`, `m_inputConnection2` | the two poses |
| `m_weightListName` | the per-bone weight list |
| `m_blendValueSource`, `m_blendSpace`, `m_bUseBlendScale` | how the blend weight is driven |
| `m_timingBehavior`, `m_flTimingBlend` | **whose timing wins** |
| `m_footMotionTiming` | foot-phase synchronisation |
| `Reset Child1` / `Reset Child2` | reset propagation (§4.2) |

`m_timingBehavior` takes `UseChild1` (93 uses) or `UseChild2` (85)
**[S2-ASSET]**. **This is the part people forget when they build a layering
system.** Blending an upper-body reload onto a lower-body run is not just a
weighted pose mix — the two clips have different lengths and phases, and you
must decide which one's clock drives the result. Offering it as an explicit
per-node choice, plus a dedicated `m_footMotionTiming`, is Valve treating foot
phase as a first-class concern.

The weight lists in the Citizen graph are a complete masking vocabulary
**[S2-ASSET]**:

```
Only_UpperBody (17)   Only_Pelvis (13)   _empty_anim_weightlist (13)
Only_Arms   Only_LeftArm   Only_RightArm   Only_Fingers
Only_LeftHand_Fingers   Only_RightHand_Fingers
Only_Eyes   Only_Morphs_and_Eyelids   Only_Helpers
Only_L_Foot_IK   Only_R_Foot_IK   Only_Holds_and_IK_hands
Remove_Legs   Remove_Arms   Exclude_ModelSpace_IK
Blend_UpperBody   Blend_UpperBody_HalfArms   Blend_UpperBody_QuarterArms
Blend_UpperBody_HalfSpine_FullArms   Blend_LowerBody_plusIK
ReduceBonesBy50Percent   MovementMSB_body_pitch   EyeAM
ProceduralHitReactionsList   Weapon_2H_Move_weaker
```

Note the naming discipline, which is doing real work:

- **`Only_X`** — include only these bones.
- **`Remove_X`** / **`Exclude_X`** — everything but these.
- **`Blend_X`** — *partial* weights, not binary. `Blend_UpperBody_HalfArms` and
  `Blend_UpperBody_QuarterArms` are the same mask at different arm strengths.
- **`ReduceBonesBy50Percent`** — a global scalar, no spatial meaning at all.

[inferred] Two lessons. **Masks are graded, not binary** — the interesting ones
are half-weight blends at the spine and arms, because a hard cut at a joint is
exactly where layering looks wrong. And **masks are named content, defined once
and referenced by name**, not per-node bone lists — which is what makes 77 bone
mask nodes maintainable.

### 5.3 Additive — `CAddAnimNode` (86) and `CSubtractAnimNode` (15)

Additive layering adds a *difference* pose onto a base — aim offsets, lean,
breathing, recoil, hit flinches — so it composes with whatever is underneath
rather than replacing it.

`CSubtractAnimNode` builds that difference **at runtime**, subtracting a
reference pose from a clip **[S2-ASSET]**, which is why 176
`CSingleFrameAnimNode`s exist: a frozen frame is the reference. So the pipeline
does not require additives to be pre-authored — the graph can manufacture one
from any clip and any reference frame. [inferred, from the node set and its
co-occurrence.]

### 5.4 Selection — `CSelectorAnimNode` (51) and `CChoiceAnimNode` (24)

`Selector` picks an input by bool/int/enum — *"Selectors are just very simple
State Machines"* **[FACEPUNCH]**. `Choice` picks among inputs by weight, which
is the variety mechanism (Source 1's equivalent is `SelectWeightedSequence` on
an activity — [`source_fps_viewmodel.md`](source_fps_viewmodel.md) §8.1).

**The choice between a Selector and a State Machine is the choice between
"switch" and "switch with transitions".** Use a selector when the change can be
instant, a state machine when it needs a blend or an exit animation. 51 vs 81 in
production says both get used heavily.

---

## 6. Damping — the surprise, and the biggest single lesson

**282 `CAnimInputDamping` nodes**, against 226 animation clips **[S2-ASSET]**.
There are more nodes smoothing inputs than there are animations being played.

The node is a spring-damper on a parameter **[S2-ASSET]**:

```
_class = "CAnimInputDamping"
m_speedFunction = "Spring"
m_fSpeedScale   = 20.0
m_fMinSpeed     = 0.0
m_fMaxTension   = 900.0
m_fTension      = 100.0
m_fDamping      = 5.0
```

Three functions are used in the shipped graph: **`Spring` (214)**, **`NoDamping`
(51)** and **`BouncySpring` (17)** **[S2-ASSET]**.

Why this matters more than it looks: **game code produces parameters that
change discontinuously.** `move_speed` jumps when you hit a wall; `aim_body_yaw`
snaps when the mouse flicks; `b_grounded` flips instantly. Feed those raw into
a blend weight and the character twitches. Nearly every parameter in a shipped
character graph is smoothed before it is used, and it is done **in the graph, per
consumption site, not once at the source** — the same parameter can be damped
differently where it drives the legs than where it drives the spine.

That the count is *51* explicit `NoDamping` nodes is itself informative: damping
is the default posture, and not damping is a decision worth writing down.
[inferred]

**If you build an animation system and provide no first-class input smoothing,
every user will reinvent it badly in gameplay code**, where it will be wrong
per-consumer and invisible to the animator. This is the single most transferable
finding in the note.

---

## 7. Aim, look-at and IK

**`CLookAtAnimNode` — 113 instances**, the third most common "real" node. Given
`aim_head`, `aim_eyes` and `aim_head_weight` (§2.1), look-at is a pervasive
procedural layer, not a special case. [inferred, from count plus parameter set.]

**`CAimMatrixAnimNode` — 23** **[S2-ASSET]**:

```
_class          = "CAimMatrixAnimNode"
m_sName         = "Melee_Punch_AimMatrix_Standing_01_delta"
m_sequenceName  = "Melee_Punch_AimMatrix_Standing_01_delta"
m_fAngleIncrement    = 90.0
m_bCanLookStraightDown = true
m_target        = "VectorParameter"
```

An aim matrix is built from a **delta** sequence — note `_delta` in the name,
i.e. an additive — sampled on a grid at `m_fAngleIncrement` degrees, aimed at a
vector parameter. `m_bCanLookStraightDown` is an explicit pole-handling flag,
which is the giveaway that this is a spherical sampling problem: straight down
is a singularity and someone had to decide whether to sample it.

**`CTwoBoneIKAnimNode` — 14**, with nodes named `"Left Arm IKrule"`
**[S2-ASSET]**, plus dedicated `Only_L_Foot_IK` / `Only_R_Foot_IK` /
`Only_Holds_and_IK_hands` / `Exclude_ModelSpace_IK` masks (§5.2). IK is a
masked layer like everything else, and the existence of `Exclude_ModelSpace_IK`
says some of the graph runs in **model space** rather than parent-relative bone
space — `citizen_modelspaceadditive.vanmgrph` is a whole graph for it
**[S2-ASSET]**. [inferred] Model-space additives are how you add a pose that
must not inherit parent rotation — leaning without the arms swinging out.

---

## 8. Motors — locomotion input

`CAnimMovementManager`, `CAnimMotorList`, `CAnimMovementSettings`, and
`CPlayerInputAnimMotor` **[S2-ASSET]**. The author's description
**[FACEPUNCH]**:

> *"Motors are responsible for taking input from the game code about how a
> character should be moving. This is useful for NPCs and other AI-driven
> characters. **If a character doesn't move, it doesn't need a Motor.**"*

And, importantly for reading the rest: *"Contrary to its name, 'Main Inputs' are
not the main source of inputs; on s&box, we use Parameters first and foremost"*
**[FACEPUNCH]** — so the motor path is Valve's built-in locomotion plumbing,
and Facepunch largely bypass it in favour of explicit parameters.

[inferred] That is a meaningful data point for anyone designing this: given both
a bespoke locomotion input system and a generic parameter channel, a shipped
licensee chose the generic one. **The generic channel is the one that must be
good.**

---

## 9. Network modes — the cosmetic/authoritative line, in the graph

Nodes carry `m_networkMode`, and exactly two values appear across the graphs
**[S2-ASSET]**:

```
155  m_networkMode = "ServerAuthoritative"
142  m_networkMode = "ClientPredicted"
```

There is also a graph-level `CAnimGraphNetworkSettings`.

This is the split from
[`networked_animation_physics.md`](networked_animation_physics.md) §2.3 —
animation is cosmetic until something reads the skeleton — **promoted into the
animation graph as a per-node property.** Unreal leaves this to the game
programmer, with Epic's docs explicitly saying the server and owning client
*"do not check to see that they are playing the same animation"* **[EPIC, via
that note]**. Source 2 makes it a field on the node.

**Two honest caveats.**

1. The **production Citizen graph contains no `m_networkMode` at all** — zero
   occurrences — while every tutorial graph and `citizen_debug` does
   **[S2-ASSET]**. Same format version in both. The most likely explanation is
   that KV3 omits default-valued fields and the tutorials were saved by a tool
   pass that wrote them out; but I have not verified that, so **treat the
   production graph as "unset/default", not as "disabled"**. [inferred]
2. I have not established *what the modes do* at runtime. The names are
   Valve's and their meaning is strongly implied, but the behaviour is not
   observable from the asset.

What is safe to conclude: **the format has a per-node concept of whether a piece
of the animation graph is client-predicted or server-authoritative.** That is an
architectural statement regardless of how it is currently used.

---

## 10. The tooling, which is half the system

Worth recording because it is what makes a 399-transition graph tractable:

- **Preview mode** runs the graph live on the model, and **active nodes and
  transitions highlight** — *"This is how you know which nodes are currently
  involved in the Final Pose"* **[FACEPUNCH]**. Highlighting works inside nested
  state machines too.
- **A recording buffer** *"automatically & continuously saves the last few
  seconds of animation"*, and you can scrub it and **step frame by frame**
  **[FACEPUNCH]**. Always on, not opt-in.
- **Per-node live inspection** — selecting a node while previewing shows *"the
  current state of the execution of the node"*, which the author calls *"very
  useful in troubleshooting"* **[FACEPUNCH]**.

[inferred] The always-on ring buffer is the same idea as this project's F9
profile capture and for the same reason: the interesting event has already
happened by the time you notice it. That is a design principle worth
generalising — **for any system whose bugs are transient, record continuously
and let the user look backwards.**

---

## 11. What I could not verify

Being explicit, since half this note is read from assets rather than docs:

- **Tutorials 10–14 are empty stubs.** All five are byte-identical 7,881-byte
  files containing 14 comment nodes and a single sequence node **[S2-ASSET]**.
  Their titles — tags, additive animation, bone mask, damping, first person —
  promise exactly the material §5, §6 and §2.1 cover, and the hack-week series
  never got there. **Everything in this note about layering, damping and
  first-person is read from the production graphs, not taught.**
- Runtime semantics of `m_networkMode` (§9).
- Blend/transition durations, node-by-node — present in the data, not
  systematically surveyed here.
- Whether the node set is complete; this is the vocabulary *these* graphs use,
  and the editor may expose more.

---

## 12. What `cromwell` should take

Directly relevant, because ozz-animation is the chosen library
(`third-party-library-choices`) and it is a *sampling and blending* library — it
gives you clips, sampling and blend jobs, and **no graph, no state machine, no
parameter system, no masking vocabulary, no damping.** Everything in this note
is the layer above ozz that would have to be built. So the question is which
parts are load-bearing.

**Build early, because they are architectural:**

1. **A typed parameter channel as *the* interface**, with damping available at
   the point of consumption (§6). This is the finding with the highest
   confidence and the lowest cost — 282 damping nodes in a shipped graph says
   this is not a nicety. Provide `Spring` at minimum, and make no-damping an
   explicit choice.
2. **Named, graded bone masks defined once and referenced** (§5.2). Not
   per-node bone lists, and not binary — the useful masks are half-weight spine
   and arm blends. A masking vocabulary is content, and it wants a name.
3. **Timing behaviour as an explicit property of every two-input blend** (§5.2).
   Which child's clock drives the result, plus foot phase. Retrofitting this
   means revisiting every blend site.
4. **Stateless pose evaluation**, per
   [`networked_animation_physics.md`](networked_animation_physics.md) §2.3 —
   pose from an explicit description, so it can be rewound. Note the tension:
   `CAnimInputDamping` is a **spring with velocity**, i.e. genuine per-frame
   state. So the damping state must be part of the description, or damping must
   sit outside the rewindable boundary. **Deciding which is a real fork and it
   should be decided deliberately**, not discovered.

**Copy the semantics, not just the shapes:**

5. **Crossfade *before* the clip ends, by default** (§4.1). "On almost finished"
   should be the default and "on finished" the opt-in, because the wrong one
   produces a freeze that everyone notices and nobody can name.
6. **Make evaluation order visible and reorderable** (§4.3). Source 2's
   creation-order rule is a documented footgun; do not reproduce it.
7. **Make signal propagation a property of the edge**, so nobody has to build a
   "reset blocker" node (§4.2).

**Do not copy:**

8. The visual editor. That is a multi-year tools investment and the reason
   AnimGraph is usable; without it, a graph this size is unmanageable. [inferred]
   **The corollary is a warning: do not design a system that requires a
   399-transition graph unless you are also going to build the editor for it.**
   A smaller, more code-driven animation layer is the honest choice for a
   project this size, and §12.1–12.3 are worth having *regardless* of whether a
   graph ever exists.

**And one calibration worth keeping.** A shipped third-person character costs
roughly **40 parameters, 220 clips, 80 state machines and 400 transitions**
(§2.1, §3). If a plan for character animation is much smaller than that, it is
either a simpler character or an underestimate.

---

## Sources

**[S2-ASSET] / [FACEPUNCH]** — s&box, `E:/SteamLibrary/steamapps/common/sbox/`:

| Path | What it is |
|---|---|
| `addons/base/Assets/animgraphs/tutorial/tutorial_0*.vanmgrph` | The hack-week tutorial series, 01–09 substantive, 10–14 stubs. All prose in this note tagged **[FACEPUNCH]** is from their `m_commentText` fields |
| `addons/citizen/Assets/models/citizen/citizen.vanmgrph` | The production player graph — 2.2 MB, 399 transitions. §3, §5, §6, §7 |
| `addons/citizen/Assets/models/citizen/citizen_modelspaceadditive.vanmgrph` | Model-space additive graph, §7 |
| `addons/citizen/Assets/models/citizen/citizen_debug.vanmgrph` | Carries `m_networkMode`, §9 |
| `bin/win64/tools/animgraph_editor.dll` | The editor itself, not disassembled here |
| `download/assets/animgraphs/*.vanmgrph_c` | ~100 compiled community graphs, not read |

Format: `animgraph2`, KV3 text, version `0f7898b8-5471-45c4-9867-cd9c46bcfdb5`.

**Related notes:** [`source_fps_viewmodel.md`](source_fps_viewmodel.md) §8 for
Source 1's answer to the same problem;
[`networked_animation_physics.md`](networked_animation_physics.md) for what
crosses the wire and why §9 and §12.4 matter;
[`valve_networking.md`](valve_networking.md) for the Source 1 → Source 2
architectural delta this note is the animation half of.
