# Rigging — FK, IK, and the constraint layer nobody budgets for

How a skeleton is actually posed at runtime: forward kinematics, the procedural
constraint pass that fixes what FK gets wrong, and inverse kinematics on top.
Read from **three shipped implementations at once** — Source 1's C++, Source 2's
model data and tool binaries, and Unreal 5.7's engine source — and then from the
open-source options, with their licences, because the point of the exercise is
to ship something.

Companion to [`source2_animation.md`](source2_animation.md), which is the layer
*above* this one: that note is about how a pose is **composed** (graph, states,
blending, masks); this note is about how a pose is **corrected** once composed.
§7 of that note touches `CTwoBoneIKAnimNode` in passing and stops. This is the
rest of it.

| Tag | Meaning |
|---|---|
| **[S1-SDK]** | Quoted from Source SDK 2013's shipping C++ — Valve's code and comments |
| **[S2-ASSET]** | Read out of s&box's plain-text ModelDoc KV3, or strings in Valve's tool binaries |
| **[UE-SRC]** | Read from UE 5.7's `Engine/Source` — Epic's code and comments |
| **[OZZ]** | ozz-animation's own headers |
| **[GODOT]** | Godot's engine blog |
| **[inferred]** | My reasoning |

**Read the licence section (§6) before the engineering sections tempt you.**
Two of the three engines here are readable but **not** copyable, and the
distinction between reading an algorithm and lifting an implementation is the
whole difference between a shippable game and a legal problem.

---

## 1. The three-layer frame

Every one of these engines does the same three things in the same order, and
naming them separately is most of the clarity:

| Layer | What it is | Cost shape |
|---|---|---|
| **FK** | Walk the bone tree parent-first, concatenating local transforms into model space. The pose as authored. | O(bones), trivially fast, nothing to decide |
| **Constraints / helper bones** | Procedural bones driven off other bones — twist distribution, kneecaps, elbow pads, cloth roots. Runs *after* the animated pose exists. | O(constraints), fixed per model, **content, not code** |
| **IK** | Overwrite part of the pose so an end effector reaches a target the animation did not know about — ground, a ladder rung, a weapon grip. | Small but **not** fixed: latching, tracing, and release logic dwarf the solve |

The interesting engineering is all in layers 2 and 3, and **layer 2 is the one
that gets left out of plans.** A shipped character rig has more constraint nodes
than IK chains by an order of magnitude — the Citizen has **4 IK chains and 31
constraint slaves** [S2-ASSET] — and a character without them reads as rubbery
at the shoulders and elbows no matter how good the animation is.

---

## 2. Source 1 — the whole IK system, in readable C++

`src/public/bone_setup.cpp`, 6,048 lines, is the only place in this study where
the *complete* runtime is legible. Everything else is data or a partial view.

### 2.1 The solver is Ken Perlin's, and Valve say so

The two-bone solve sits in a class whose comment is an attribution and a
derivation [S1-SDK]:

```cpp
class CIKSolver
{
public:
//-------- SOLVE TWO LINK INVERSE KINEMATICS -------------
// Author: Ken Perlin
//
// Given a two link joint from [0,0,0] to end effector position P,
// let link lengths be a and b, and let norm |P| = c.  Clearly a+b <= c.
//
// Problem: find a "knee" position Q such that |Q| = a and |P-Q| = b.
```

The method, in three lines: **rotate the problem so the target lies on the x
axis, solve the 2D circle intersection in closed form, rotate back.**

```cpp
   static float findD(float a, float b, float c) { return (c + (a*a-b*b)/c) / 2; }
   static float findE(float a, float d) { return sqrt(a*a-d*d); }
```

and the coordinate frame is chosen so that the *preferred* knee direction lands
in the positive-y half plane — which is how the pole vector enters an otherwise
purely metric problem [S1-SDK]:

```cpp
// If "knee" position Q needs to be as close as possible to some point D,
// then choose M such that M(D) is in the y>0 half of the z=0 plane.
```

That is the entire analytic two-bone IK, and it is about forty lines. **It is
not the expensive part of an IK system and never was.** Everything in §2.3–2.5
exists around it.

### 2.2 Bones are re-aimed, not rotated

Once the knee position is known, Valve do not compute a rotation — they rebuild
the bone matrices so their **X axis points down the bone** [S1-SDK]:

```cpp
void Studio_AlignIKMatrix( matrix3x4_t &mMat, const Vector &vAlignTo )
// Column 0 (X) becomes the vector.
// Column 1 (Y) is the cross of the vector and column 2 (Z).
// Column 2 (Z) is the cross of columns 0 (X) and 1 (Y).
```

with a `// FIXME: check for X being too near to Z` that is still there.

This is a hard convention: **the solver only works on a skeleton whose bones run
down +X.** Source 2 kept the same requirement and made it a checkbox —
`m_bDoBonesOrientAlongPositiveX = true` on every Citizen IK chain [S2-ASSET].
[inferred] Which means it is not a Source 1 quirk but a property of writing the
solve this way, and any rig you intend to solve on has to agree with the solver
about which axis is "along the bone". Decide it once, assert it at load.

### 2.3 The degenerate cases, and how bluntly they are handled

This is the honest part of the code, and every value is a tuning constant
someone arrived at empirically [S1-SDK]:

```cpp
#define KNEEMAX_EPSILON 0.9998 // (0.9998 is about 1 degree)
```

- **Too straight to know where the knee is** — if the hip-to-foot distance
  exceeds `(l1 + l2) * 0.9998`, the no-hint overload simply `return false`s. The
  system gives up.
- **Target out of reach** — the target is clamped back to `(l1+l2) * 0.9998`,
  i.e. the leg extends fully and stops. There is a debug colour change to red at
  that line, which tells you how often it happened in practice.
- **Target too close** — clamped to `max(|l1-l2| * 1.15, min(l1,l2) * 0.15)`,
  commented as *"limit distance to about an 80 degree knee bend"*.
- **The knee-hint exaggeration** is a naked hack, with its own FIXME:

```cpp
	// exaggerate knee targets for legs that are nearly straight
	// FIXME: should be configurable, and the ikKnee should be from the original animation, not modifed
	float d = (targetFoot-worldThigh).Length() - min( l1, l2 );
	d = max( l1 + l2, d );
	// FIXME: too short knee directions cause trouble
	d = d * 100;
```

`d = d * 100` pushes the knee hint a hundred times further away so that the
direction dominates and the magnitude stops mattering. It works, and Valve knew
it was not the right answer. **Unreal solved both FIXMEs properly** — §4.2 and
§4.3 are literally these two comments, fixed.

### 2.4 The big idea: IK rules are baked into the animation

This is the architectural move worth taking, and it is invisible if you only
look at the solver.

An animation clip does not just carry bone curves. It carries **IK rules**
[S1-SDK], `mstudioikrule_t`, one per chain per rule, of six types:

```c
#define IK_SELF 1        // lock to another bone on the same character
#define IK_WORLD 2
#define IK_GROUND 3      // plant on the world
#define IK_RELEASE 4     // stop planting
#define IK_ATTACHMENT 5  // lock to a named attachment
#define IK_UNLATCH 6
```

Each rule has a four-point influence envelope — `start`, `peak`, `tail`, `end`
in cycle units — and, crucially, a **compressed IK error track**:

```c
	int			compressedikerrorindex;
	float		contact;	// frame footstep makes ground concact
	float		drop;		// how far down the foot should drop when reaching for IK
	float		top;		// top of the foot box
```

The error track is the per-frame difference between where the animation puts the
foot and where the lock says it should be. **The compiler bakes it; the runtime
replays it.**

[inferred] The consequence is the point: *the animation itself declares its own
contacts*. A walk cycle knows which frames its left foot is planted and how much
the raw curve drifts off that plant. Gameplay code never says "plant the foot
now" — it says "play walk", and the plant comes along. This is the same
principle as `source2_animation.md` §2.1's parameter interface — **push the
knowledge into the content, and let code carry only what code actually knows**.

The alternative, which many projects build first, is a gameplay-side foot
planter that detects contact from foot velocity. It is worse, and predictably:
it cannot know about a contact that has not happened yet, so it is always at
least one frame late, and it cannot distinguish a slow step from a slide.

### 2.5 Latching, error and release — the machinery that is actually the system

`CIKContext::UpdateTargets` and `AutoIKRelease` are ~250 lines against the
solver's 40, and they exist to answer one question: **the foot is planted, the
character keeps moving — when do you give up?**

The latch [S1-SDK]:

```cpp
	if (pTarget->latched.bHasLatch)
	{
		if (pTarget->est.latched == 1.0)
		{
			// keep track of latch position error from ideal contact position
			pTarget->latched.deltaPos = pTarget->latched.pos - pTarget->est.pos;
```

so a planted foot stores its **error from the ideal animated position**, not an
absolute world position, and when the rule decays that error is ramped to zero:

```cpp
			// ramp out latch differences during decay phase of rule
```

Two break conditions, both with the constants exposed [S1-SDK]:

```cpp
		// unstick feet when distance is too great
		if ((d4 < fabs( d1 - d2 ) || d4 * 0.95 > d1 + d2) && pTarget->est.latched > 0.2)
			pTarget->error.flTime = m_flTime;

		// unstick feet when angle is too great
		float d = fabs( pTarget->latched.deltaQ.w ) * 2.0f - 1.0f;
		// FIXME: cos(45), make property of chain
		if (d < 0.707)
			pTarget->error.flTime = m_flTime;
```

The leg is broken if the latch would need it shorter than `|l1-l2|` or longer
than 95% of full extension, or if the ankle would have to twist more than 45°.
And `// FIXME: cos(45), make property of chain` is another admission that a
global constant is standing in for per-chain data.

Then `AutoIKRelease` **synthesises an `IK_RELEASE` rule at runtime** and pushes
it into the rule list [S1-SDK]:

```cpp
		if (dt < 0.25) pTarget->error.ramp = min( pTarget->error.ramp + ft * 4.0, 1.0 );
		else           pTarget->error.ramp = max( pTarget->error.ramp - ft * 4.0, 0.0 );
		...
		ikrule.type = IK_RELEASE;
		ikrule.flWeight = SimpleSpline( pTarget->error.ramp );
```

Ramp in over 0.25 s, hold, ramp out — 4.0 per second either way, smoothstepped.

**This is the reusable shape, and it generalises well beyond feet:** an error
condition does not disable a system, it *fades* it, through the same weighted
path the normal case uses. The release is expressed as a rule of the same type
the content authors use, so there is exactly one code path that turns IK off.
[inferred] If you build the "broken" case as a separate branch that clamps or
skips, you will get a visible pop at exactly the moment the character is doing
something unusual — which is the moment the player is looking.

---

## 3. Source 2 — two IK systems, and a constraint vocabulary

Source 2 is closed, but s&box ships the Citizen's ModelDoc data as plain-text
KV3, and Valve's `modeldoc_editor.dll` carries its enum strings in the clear.
Same provenance as [`source2_animation.md`](source2_animation.md).

### 3.1 The old system is shipped, complete, and disabled

`citizen_ikdata.vmdl_prefab` contains an `IKRigBiped` node with four
`IKChainOld` chains — and this note on it [S2-ASSET]:

> *"Disabled due to causing sequence bound issues, and most likely being an
> outdated system that should not be used"*

`disabled = true`. But the data is fully authored, which makes it the best
available specification of **what a foot-planting system needs to be told**
[S2-ASSET]:

```
_class = "IKChainOld"          name = "right_leg_IKold"
    _class = "IKRuleGround"
    trace_height = 20.0        trace_radius = 2.5
    z_spring_strength = 10.0   normal_spring_strength = 10.0
end_effector_bone = "ankle_R"
end_effector_target_bone = "foot_R_IK_target"
reverse_footlock_bone = "ball_R"
max_lock_distance_to_target = 5.0
solver = "IKSOLVER_TwoBone"
hyperextension_release_dot_threshold = -0.984808
use_target_instead_of_lock_threshold = 1.0
break_restoration_time = 0.15
soften_time = 0.5
soften_percentage = 0.1
```

and at the rig level:

```
initial_master_blend_amount = 1.0
abs_origin_drop_height = 24.0
abs_origin_drop_height_spring_strength = 500.0
default_tilt_spring_strength = 5.0
system = "IKSYSTEMTYPE_Animgraph"
```

Point by point, against §2.5's Source 1 constants:

- **The break thresholds became per-chain data**, which is exactly what Valve's
  own `// FIXME: cos(45), make property of chain` asked for. And the values
  differ per limb: `-0.984808` is **cos(170°)** on the legs, `0.937995` is
  **cos(20.2°)** on the arms [S2-ASSET]. A leg releases only when nearly
  hyperextended; an arm releases 150° earlier. One global constant could never
  have served both.
- **Springs, not clamps.** `z_spring_strength`, `normal_spring_strength`,
  `abs_origin_drop_height_spring_strength`, `default_tilt_spring_strength` —
  the ground solution is damped over time rather than snapped per frame. This is
  the same finding as `source2_animation.md` §6's 282 damping nodes, arriving
  from a different direction: **Valve smooth every input that a discontinuous
  world can produce**, and a ground trace across a step edge is about as
  discontinuous as it gets.
- **`abs_origin_drop_height = 24.0`** — the whole character's origin drops to
  keep both feet reachable. Foot IK alone cannot handle a big step; the pelvis
  has to come down too, and that is a *separate* mechanism from the leg solve.
- **`reverse_footlock_bone = "ball_R"`** — the lock pivots about the ball of the
  foot, not the ankle, so a heel-lift still reads as planted.
- **`soften_time` / `soften_percentage`** — §4.2 again, in data.

### 3.2 The new system, and where its target comes from

The live system is `IKChain`, a nested `IKChainJoint` tree [S2-ASSET]:

```
_class = "IKChain"    name = "right_leg_IK"
    IKChainJoint "leg_upper_R"  →  IKChainJoint "leg_lower_R"  →  IKChainJoint "ankle_R"
        IKJointConstraint_Hinge   hinge_axis = "Up"
        min_radians = -1.570796   max_radians = 1.570796
m_bDoBonesOrientAlongPositiveX = true
m_DefaultSolverSettings.m_nNumIterations = 6
m_DefaultSolverSettings.m_SolverType     = "IKSOLVER_TwoBone"
m_DefaultTargetSettings.m_Bone           = { m_Name = "foot_R_IK_target" }
m_DefaultTargetSettings.m_TargetSource   = "Bone"
m_Data.m_DefaultTargetSettings.m_AnimgraphParameterNamePosition = { m_id = 4294967295 }
m_Data.m_bParentJointRequiresAlignment = true
```

Two things to take:

**The target has a source, and one option is an animgraph parameter.**
`m_TargetSource = "Bone"` here, with an unset (`0xFFFFFFFF`) alternative
`m_AnimgraphParameterNamePosition`. So an IK goal is either *a bone in the
skeleton the animator can animate* or *a vector parameter game code writes* —
the same typed parameter channel as `source2_animation.md` §2.1. [inferred]
Making "a bone" the default is the better default: it means an animator can
author a hand goal in the DCC package and the runtime honours it with no code
involved, and the code path is the exception rather than the rule.

**Iterations and solver type are chain properties with defaults**
(`m_DefaultSolverSettings`), which implies per-invocation override — the same
chain solved cheaply in a crowd and expensively up close. [inferred, from
"Default" in the field name.]

### 3.3 The solver menu — and Ken Perlin is still in it

`modeldoc_editor.dll` and `hammer.dll` both carry the full enum [S2-ASSET]:

```
IKSOLVER_TwoBone   IKSOLVER_CCD   IKSOLVER_Fabrik   IKSOLVER_DogLeg   IKSOLVER_Perlin
```

- **`IKSOLVER_Perlin`** is §2.1's Source 1 solver, surviving an engine rewrite
  as a named option beside its replacement. [inferred] Which strongly suggests
  `IKSOLVER_TwoBone` is a *different* analytic two-bone solve and Perlin's was
  kept for compatibility with rigs tuned against its exact knee behaviour.
- **`IKSOLVER_DogLeg`** — a three-segment limb, i.e. quadrupeds and birds. Worth
  noting that the digitigrade leg was important enough to get its own solver
  rather than be expressed as a generic chain.
- **`CCD`** and **`Fabrik`** are the two standard iterative solvers, for chains
  longer than a limb — spines, tails, tentacles.

Five solvers is the answer to "which algorithm should I implement": **the
question is wrong.** Limbs get an analytic solve; long chains get an iterative
one; quadruped legs get their own. They are not interchangeable and shipping
engines ship several.

### 3.4 The constraint is authored and the solver ignores it

The single most useful sentence in the whole Citizen model, attached to the knee
hinge constraint [S2-ASSET]:

> *"YES, this looks incorrect, but I can assure you this is the right data. Also,
> the two-bone IK solver completely ignores min/max degrees."*

So `IKJointConstraint_Hinge` with `min_radians = -π/2, max_radians = π/2` is
sitting in production data doing **nothing**, because the solver selected for
that chain does not read it. The rigger knew, and wrote it down in the file,
because otherwise the next person would "fix" the data.

[inferred] Two lessons, and the second is the expensive one. First: joint limits
belong to *iterative* solvers — an analytic two-bone solve produces the one
correct answer and has nowhere to apply a limit. Second, and generally: **if a
data field is silently ignored depending on another field's value, that is a
design defect, not a documentation problem.** Either the editor greys it out, or
the loader warns, or you will ship rigs tuned against constraints that were
never enforced. This is the same class of hazard as `source2_animation.md`
§4.3's invisible transition ordering.

### 3.5 The FK constraint layer — the part that is really "rigging"

`citizen_animconstraintlist.vmdl_prefab`, 24 KB, is where the character stops
being a bone tree and starts looking like a body. The vocabulary, from the
editor binary [S2-ASSET]:

```
AnimConstraintParent   AnimConstraintOrient   AnimConstraintPoint   AnimConstraintAim
AnimConstraintTiltTwist   AnimConstraintTwist   AnimConstraintMorph
AnimConstraintPoseSpaceBone   AnimConstraintPoseSpaceMorph
inputs:  AnimConstraintBoneInput   AnimConstraintAttachmentInput
output:  AnimConstraintSlave
```

The Citizen's own census [S2-ASSET]:

| Count | Class |
|---|---|
| 31 | `AnimConstraintSlave` |
| 19 | `AnimConstraintBoneInput` |
| 13 | `AnimConstraintParent` |
| 12 | `AnimConstraintTiltTwist` |
| 12 | `AnimConstraintAttachmentInput` |
| 5 | `AnimConstraintOrient` |
| 1 | `AnimConstraintAim` |

**Every constraint is inputs → slave, with a weight on each side.** That
uniformity is the design: a constraint is not a special-cased behaviour, it is a
small dataflow node with typed inputs and one output, and the weights are signed.

**Twist bones (12 of them) are the bulk of it.** The shoulder case [S2-ASSET]:

```
AnimConstraintTiltTwist "ShoulderToBicep_R_rotation"
    input:  bone arm_upper_R          weight  1.0
    slave:  bone arm_upper_R_twist0   weight -0.65

AnimConstraintTiltTwist "Bicep_R_rotation"
    input:  bone arm_upper_R          weight  1.0
    slave:  bone arm_upper_R_twist1   weight -0.1
```

and the forearm, which is driven from the *hand*, not the elbow [S2-ASSET]:

```
AnimConstraintTiltTwist "Ulna_R_rotation"
    input:  bone hand_R               weight  1.0
    slave:  bone arm_lower_R_twist1   weight  0.65
```

That is the classic twist-distribution rig stated in five numbers: **the upper
arm's twist is counter-rotated out of the bones near the shoulder (−0.65, −0.1)
so the deltoid does not shear, and the forearm's twist is inherited from the
wrist (+0.65) so the skin winds up gradually instead of all at the wrist.**
Legs get the identical treatment with `relative_angles = [0, 180, 180]` on the
input to account for the thigh's flipped axis, and the calf is driven from the
ankle at +0.6.

**These are not animated.** Twelve twist bones × every clip would be twelve more
curves to author, keep in sync, and get wrong. They are derived, once, in data.

### 3.6 "Poor man's RBF" — helper bones without pose-space deformation

The prettiest trick in the file, and it is labelled [S2-ASSET]:

```
AnimConstraintParent  "Elbow_R_position"   note = "Poor man's RBF..."
    input:  attachment driver_elbow_R_position   weight 1.0
    slave:  bone       arm_elbow_helper_R        weight 1.0
    constrained_bone = "arm_elbow_helper_R"
    weight = 0.15
```

and the driver attachment it reads is itself a **weighted blend of two bone
spaces** [S2-ASSET]:

```
Attachment "driver_elbow_R_position"
    AttachmentInfluence  bone arm_upper_R_twist1  origin [18.3, 0.8, 0]  angles [0,-90,0]  weight 1.0
    AttachmentInfluence  bone arm_lower_R_twist0  origin [-5.5, 0.9, 0]  angles [0,-110,0] weight 1.0
```

The mechanism: an attachment blended 50/50 between the upper-arm and forearm
frames sits on the *bisector* of the elbow, so its position is a smooth function
of how bent the elbow is. Constrain a helper bone to it at **0.15** weight and
you get a bone that slides a little as the joint closes — a fake elbow pad. Add
`AnimConstraintOrient` at −0.5 from the upper arm with an authored
`relative_angles = [0, -81.465, 0]` and it rotates at half rate, too.

The kneecap is the same recipe at **0.1** weight, with the orient input at
`[0, 88.95, 0]` [S2-ASSET].

Two observations that transfer:

1. **This is a corrective driven by joint angle, expressed with no new
   machinery.** Source 2 *has* real pose-space deformation —
   `AnimConstraintPoseSpaceBone` and `CBoneConstraintPoseSpaceBone` are in the
   binary [S2-ASSET] — and the Citizen's constraint list contains **zero** of
   them. A production rig chose the cheap approximation. [inferred] Probably
   because PSD needs authored corrective poses and this needs two numbers.
2. **The magic numbers are all small** — 0.15, 0.1, 0.35, 0.6, 0.65. Helper
   bones move *slightly*. The failure mode of this kind of rigging is doing too
   much of it.

### 3.7 The neck, in two constraints and two comments

The clearest worked example in the file, because the rigger explained both halves
[S2-ASSET]:

```
AnimConstraintParent → neck_clothing, weight 0.75
  "Before we do anything, make neck_clothing follow neck_0, in case it moves
   (height scaling, exaggerated pose that moves the bone). It's set to 75 percent
   so that a bit more neck shows when tall, and less when short. If we don't do
   this, the two constraints can become messy and flip upside-down."

AnimConstraintAim  input head → slave neck_clothing,  aim_offset [0,90,0], up_type 3
  "The first constraint removes the "twist" component of the neck bone by
   "aiming" at the head."

AnimConstraintOrient  input head → slave neck_clothing, weight 0.35
  "The second constraint then lets a bit of the head rotation back in.
   (Not the neck, the head. This works better.)"
```

**Aim to destroy twist, then orient to add back a fraction of it.** That
decomposition — kill the component you don't want, then reintroduce a scaled
version — is the general recipe for "follow, but not all the way", and it is why
`AnimConstraintAim` and `AnimConstraintOrient` are separate node types rather
than one node with a twist slider.

Note also the *order dependency* the first comment describes: the constraints
flip upside-down if the base position is not established first. So the
constraint list is **evaluated in order and the order is load-bearing** — the
same hazard as `source2_animation.md` §4.3, in a different subsystem. [inferred]

### 3.8 One apparent bug in production data

Flagging it because it is instructive, not because it matters to anyone's game
[S2-ASSET]. Every constraint in the file has its `AnimConstraintSlave`'s
`parent_bone` equal to the constraint's `constrained_bone` — except:

```
AnimConstraintParent  "Elbow_L_position"
    input:  attachment driver_elbow_L_position
    slave:  bone       arm_elbow_helper_R     ← R
    constrained_bone = "arm_elbow_helper_L"   ← L
```

The left elbow's slave names the **right** helper bone. Its mirror three
constraints earlier is consistent, as are both kneecaps. [inferred] A
copy-paste that the tool did not catch, in a rig used by every s&box player
character. Whether it has any visible effect depends on which of the two fields
the runtime actually reads, which I cannot determine from the asset — but
**that ambiguity is itself the finding**: two fields encoding the same fact is
one field too many, and a validator comparing them costs four lines.

---

## 4. Unreal 5.7 — the same problems, with the FIXMEs fixed

Epic ship `Engine/Source` with the binary install, so this is the real code.
**It is readable and it is not copyable — see §6.1.**

### 4.1 The menu

Three tiers, and knowing which is which saves a lot of confusion [UE-SRC]:

| Where | What |
|---|---|
| `Runtime/AnimationCore/` | The **algorithms**, engine-agnostic: `TwoBoneIK.cpp`, `FABRIK.cpp`, `CCDIK.cpp`, `SplineIK.cpp`, `SoftIK.cpp`, `Constraint.cpp`, `AngularLimit.cpp` |
| `Runtime/AnimGraphRuntime/BoneControllers/` | The **anim-graph nodes** that call them: `AnimNode_TwoBoneIK`, `_Fabrik`, `_CCDIK`, `_SplineIK`, `_LegIK`, `_HandIKRetargeting` |
| `Plugins/Animation/IKRig/` + `Plugins/Experimental/FullBodyIK/` | The **rig-level solvers**: `IKRigLimbSolver`, `IKRigPoleSolver`, `IKRigBodyMoverSolver`, `IKRigFullBodyIK`, and two whole full-body implementations (`JacobianSolver`, `PBIKSolver`) |

The tiering itself is the lesson: **the solve is a pure function, the node is
where blending and bone-index plumbing live, and the rig is where the solves are
ordered into a stack.** Source 2 has the same three tiers (solver enum, animgraph
node, IKChain) and Source 1 collapses the last two into `CIKContext`.

### 4.2 Soft IK — Source 1's `KNEEMAX_EPSILON` done properly

`SoftIK.cpp` is 60 lines and worth having in full [UE-SRC]:

```cpp
	// convert percentage to distance
	const float SoftDistance = TotalChainLength * (1.0f - FMath::Min(1.0f, SoftLengthPercent));
	const float HardLength   = TotalChainLength - SoftDistance;
	const float CurrentDelta = CurrentLength - HardLength;
	...
	// calculate the "softened" length of the effector
	const float PercentIntoSoftLength = CurrentDelta / SoftDistance;
	const float SoftenedLength = HardLength + SoftDistance * (1.0 - FMath::Exp(-PercentIntoSoftLength));
```

An **exponential approach to full extension**: inside the last few percent of
reach, the effector asymptotes instead of clamping. The header credits the
technique to a Softimage blog post via the Internet Archive, and documents the
default: *"typically set to 0.97"* [UE-SRC].

Why it matters more than it looks: a leg reaching for a target beyond its length
snaps to fully straight and **stays** there while the target keeps moving, then
snaps back — the knee pops. Source 1 has exactly this behaviour (§2.3's clamp to
`0.9998`); Source 2 has `soften_time` and `soften_percentage` in the old chain
data (§3.1); Unreal has it as a shared function called from `LegIK` and PBIK
alike; ozz has it as the `soften` input on its two-bone job (§6.2). **All four
independently concluded that clamping at full extension is unacceptable.** If
you implement one refinement beyond the raw solve, implement this one.

### 4.3 Extract the pole vector from the animation

`AnimNode_LegIK.cpp`, on how it decides which way the knee bends [UE-SRC]:

```cpp
	// If we have a HingeRotationAxis defined, we can cache 'BendDir'
	// and use it when we can't determine it. (When limb is straight without a bend).
	// We do this instead of using an explicit one, so we carry over the pole vector that animators use.
	// So they can animate it, and we try to extract it from the animation.
```

and when the limb is too straight to measure, it reorients the **cached** bend
direction against the hinge axis rather than failing.

This is Source 1's second FIXME — *"the ikKnee should be from the original
animation, not modifed"* — answered. And the reasoning in the comment is the
transferable part: **the animation already contains the pole vector, because the
animator posed the knee.** Adding an explicit pole-vector control creates a
second source of truth that will disagree with the first. Measure it from the
input pose; keep the last good measurement for the degenerate frames.

Compare the three responses to "the limb is straight, I can't find the plane":

| Engine | Response |
|---|---|
| Source 1 | `return false` — the whole solve fails [S1-SDK] |
| Source 2 | `hyperextension_release_dot_threshold` per chain — release the lock [S2-ASSET] |
| Unreal | cache the last valid bend direction and reuse it [UE-SRC] |

Unreal's is strictly better and costs one `FVector` of state.

### 4.4 Full-body IK, and why it is a different animal

`LegIK` also shows the honest fast path [UE-SRC]:

```cpp
	// Two Bones, we can figure out solution instantly
	...
	// Do iterative approach based on FABRIK
```

Analytic when the chain is two bones, FABRIK otherwise — in the same node.

For whole-skeleton problems Epic ship **PBIK** (`Plugins/Experimental/FullBodyIK`),
which is not a chain solver at all. It converts the skeleton into **rigid bodies
plus constraints and runs a position-based solver over the lot** [UE-SRC]:

```cpp
void FPBIKSolver::UpdateBodies(const FPBIKSolverSettings& Settings)
{
	// apply large scale gross movement to entire skeleton based on effector deltas
	// (this creates a better pose from which to start the constraint solving)
	ApplyRootPrePull(...);
	// pre-rotate bones towards preferred angles when effector limb is squashed
	ApplyPreferredAngles();
	// pull each effector's chain towards itself
	ApplyPullChainAlpha(...);
	// run ALL constraint iterations
	SolveConstraints(Settings);
}
```

Per-bone settings are physical rather than kinematic [UE-SRC]:
`RotationStiffness`, `PositionStiffness` (*"At higher values, the bone will
resist rotating (forcing other bones to compensate)"*), per-axis limits as
`Free` / `Limited` / `Locked` with min/max degrees, and **preferred angles** —
*"this bone will 'prefer' to rotate in the direction specified ... when the chain
it belongs to is compressed"*, which is the pole-vector problem generalised to
arbitrary joints.

Two details worth stealing regardless:

- **`StrengthAlpha` as a stabiliser** — *"At 0.0, the effector does not pull at
  all, but the bones between the effector and the root will still slightly
  resist motion from other effectors. This can thus act as a 'stabilizer' of
  sorts for parts of the body that you do not want to behave in a pure FK
  fashion."* [UE-SRC] A zero-strength effector is not a disabled effector; it is
  an anchor.
- **`PullChainAlpha`** — chains are pre-translated and rotated toward their
  goals *before* iteration, *"significantly improve[ing] convergence on dense
  bone chains"*, with the honest caveat *"may cause undesirable results in
  highly constrained bone chains (like robot arms)"* [UE-SRC]. A good initial
  guess is worth more than iterations, which is the general rule for every
  iterative solver.

And the meta-observation: **Unreal ships two full-body IK solvers** — the
Jacobian one in `Source/FullBodyIK/JacobianSolver.cpp` and PBIK — both under a
plugin still marked *Experimental* in 5.7 [UE-SRC]. [inferred] Full-body IK is
not a solved problem with a right answer; it is a family of trade-offs, and
Epic have not picked either. That is a strong argument for **not** building one
until a shipped feature demands it.

---

## 5. What all three agree on

The synthesis, and the part worth carrying forward independent of any engine:

| # | Agreement | Evidence |
|---|---|---|
| 1 | **Limbs get an analytic two-bone solve.** Iterative solvers are for chains longer than a limb. | `IKSOLVER_TwoBone` default on all four Citizen chains; UE's *"Two Bones, we can figure out solution instantly"*; ozz's job is two-bone only |
| 2 | **Soften at full extension; never clamp.** | §4.2, in all four codebases |
| 3 | **The pole vector comes from the animation.** | UE's cached `BendDir`; Perlin's `D` hint taken from the pre-IK knee position |
| 4 | **IK is a weighted layer, never absolute.** | Source 1's `flWeight` × `flRuleWeight`; ozz's `weight`; PBIK's four separate alphas; Source 2's bone masks `Only_L_Foot_IK`, `Exclude_ModelSpace_IK` |
| 5 | **Failure fades, it does not branch.** | `AutoIKRelease` synthesising a normal release rule; `hyperextension_release_dot_threshold` + `break_restoration_time` |
| 6 | **Contacts belong to the animation, not to gameplay code.** | `mstudioikrule_t` baked per clip with a compressed error track |
| 7 | **Twist and helper bones are constraints, evaluated after FK, authored once.** | 12 `AnimConstraintTiltTwist` on the Citizen; no equivalent animation curves anywhere |

And one they agree on by omission: **nobody solves the whole body by default.**
Source 2's full-body rig is disabled, Unreal's is experimental, Source 1 has
none. Per-limb IK plus a pelvis drop covers the shipped cases.

---

## 6. What you can actually ship — the licence answer

**This is a reading of the licences, not legal advice.** Where money is
involved, read the text yourself; all of it is short and linked in §9.

### 6.1 The two things in this note you may **not** copy

**Source SDK 2013 — non-commercial, Source-engine-only.** The licence permits
downloading and using the SDK *"to develop a modified Valve game running on the
Source engine"* and distributing it *"but only for free"*; commercial use
requires contacting Valve. Every line of §2 is therefore **reading material**.
Do not paste `CIKSolver` into your engine.

**Unreal Engine — EULA-licensed engine code.** Epic's engine source is licensed
for building products *with Unreal Engine*. Lifting `SoftIK.cpp` into a
non-Unreal C++ engine is outside that grant. §4 is likewise reading material.

**What you may take from both: the ideas.** An algorithm is not protected by
copyright, its expression is. "Soften the effector with an exponential falloff
starting at 97% of chain length" is a technique — and in this case one Epic's own
header attributes to a public blog post. "Cache the last valid bend direction"
is an idea. Write your own implementation from the description, do not
transcribe theirs, and do not keep their code in your tree "temporarily".

[inferred] The practical test: if you could explain the technique to someone in
two sentences and they could implement it without the original file open, you
are taking the idea. If you have the file open while typing, you are not.

### 6.2 What you can take, and what it gives you

| Library | Licence | Gives you | Verdict |
|---|---|---|---|
| **ozz-animation** | **MIT** | Skeleton, clip sampling, blending with **per-joint weights** (= bone masks), additive blending, `IKTwoBoneJob`, `IKAimJob`, SoA/SIMD layout, offline pipeline | **The base.** Already the chosen library |
| **Godot 4.6 IK modifiers** | **MIT** | `TwoBoneIK3D`, `SplineIK3D`, `FABRIK3D`, `CCDIK3D`, `JacobianIK3D`, `BoneConstraint3D` — a clean class hierarchy under `IKModifier3D` | **The reference you may actually copy.** MIT, so attribution is the only cost |
| **ufbx** | **MIT** | Single-file FBX import, no Autodesk SDK | Take it — see §6.3 |
| **cgltf** | **MIT** | glTF/GLB with skins and animations | Take it |
| **ACL** | **MIT** | Animation compression; UE 5.3+ default codec | Later, when clip memory is a real number |
| **Jolt** | **MIT** | Ragdolls, joint constraints, physical animation | Already chosen for physics |
| **Eigen** | MPL-2.0 | Linear algebra, if you ever build a Jacobian solver | File-level copyleft only; fine to link, but you almost certainly don't need it |
| **Orocos KDL** | LGPL-2.1 | Robotics-grade kinematics/dynamics | **Avoid.** LGPL + static linking is a problem, and it is shaped for manipulators, not characters |
| Blender rigging / iTaSC | **GPL** | — | **No.** Not usable in a closed-source game |
| UE `AnimationCore`, PBIK | Unreal EULA | — | Read only |
| Source SDK 2013 | Valve non-commercial | — | Read only |

### 6.3 Two things about ozz worth knowing before you rely on it

**It already has the two solvers you need, and they are complete.** `IKTwoBoneJob`
takes `target`, `mid_axis`, `pole_vector`, `twist_angle`, `soften` and `weight`,
and outputs two **local-space quaternion corrections** plus a `reached` flag
[OZZ]. That is §5's rows 1–4 already implemented — including `soften`, *"Allows
the chain to gradually fall behind the target position"*, which is §4.2. And
`IKAimJob` covers look-at with `forward`, `up`, `pole_vector`, `offset` and
`twist_angle` [OZZ], which is the `CLookAtAnimNode` of `source2_animation.md`
§7 — the *third* most common node in a shipped Valve graph.

**The FBX path drags in the Autodesk FBX SDK.** ozz's toolset converts *"gltf,
Fbx, Collada, Obj, 3ds, dxf"*, but the FBX importer builds against Autodesk's
proprietary SDK [OZZ]. Two clean ways out: use the **glTF** path, or import FBX
with **ufbx** (MIT) and feed ozz's offline structures yourself. [inferred] Given
that the offline side runs on your machine and never ships, this is a build
inconvenience rather than a licensing one — but it is the kind of thing that
turns into a three-day detour if discovered late, and ufbx removes it entirely.

### 6.4 What no open library gives you

Being explicit, because this is the gap that costs time:

- **No open-source library implements §2.4** — IK rules baked into clips with
  contact envelopes and error tracks. That is a pipeline feature and you would
  build it.
- **No open-source library implements §2.5** — latching, error detection and
  release. That is ~250 lines of *state machine*, not maths, and it is the part
  that makes foot planting look correct.
- **No open-source library implements §3.5–3.7** — the FK constraint layer.
  Godot's `BoneConstraint3D` is the closest thing and it is much smaller than
  Valve's vocabulary.

So the honest shape of the work: **ozz gives you the pose; Godot gives you the
solvers you can legally read *and* copy; the two layers around them are yours.**
Estimating from the sources here, the solver layer is a few hundred lines and
the *rules* layer around it is a couple of thousand.

---

## 7. What `cromwell` should take

There is no skeletal animation in the engine yet — this is a greenfield note,
so the value is in what gets decided before the first line, not after.

**Decide now, because retrofitting is expensive:**

1. **Fix the bone axis convention and assert it at load.** Both Valve engines
   require bones down +X and Source 2 made it an explicit per-chain flag (§2.2).
   A mismatch here produces limbs that solve to plausible-but-wrong poses, which
   is the worst failure mode to debug.
2. **Make an IK goal a *bone*, with a parameter as the alternative** (§3.2). The
   default path should let an animator author the goal with no code involved.
3. **Every IK application is weighted, and the weight is the only way to turn it
   off** (§5 row 4, §2.5). No boolean enable. A release is a weight ramp through
   the normal path.
4. **Clips must be able to carry contact data** (§2.4). Even if nothing reads it
   for a year, the *asset format* has to have somewhere to put it, because adding
   a per-clip side-channel later means reprocessing every animation.
5. **The constraint pass is a separate, ordered, data-driven stage between FK and
   IK** (§3.5). Uniform inputs → weighted slave; named types; evaluation order
   explicit and visible, since §3.7 shows order is load-bearing.

**Build in this order:**

| Stage | What | Why here |
|---|---|---|
| 1 | ozz sampling + blending + masks | Nothing else is testable without a pose |
| 2 | `IKAimJob` look-at | Cheapest visible win; §7 of the animation note says look-at is pervasive |
| 3 | Constraint pass: `TiltTwist`, `Parent`, `Orient`, `Aim` | Four node types cover the whole Citizen. Makes characters stop looking rubbery |
| 4 | `IKTwoBoneJob` with soften, driven by a bone goal | The limb case, done properly first time |
| 5 | Foot planting: trace, latch, error, release | The 250-line state machine. Only now, because it needs 1–4 |
| 6 | Pelvis drop (`abs_origin_drop_height`) | Foot IK alone cannot handle a step (§3.1) |
| — | FABRIK/CCD for long chains | Only when a spine or tail exists |
| — | Full-body IK | **Not until a shipped feature demands it** (§4.4) |

**Give it a profiler zone at stage 1** — CLAUDE.md's rule, and this is a
per-frame per-character system. One zone named `animation` until a measurement
splits it; per §5, expect the *rules* layer to outweigh the solve, so a naive
split into `ik solve` / `constraints` would be the wrong first cut.

**Multi-genre check** (`cromwell-engine-target-genres`): an RTS needs stage 1–3
and little else at unit scale; an FPS needs 1–4 plus hand-to-weapon IK; a
third-person game is the only one that needs 5–6. That ordering is not
accidental — it is increasing in both cost and how close the camera gets.

---

## 8. What I could not verify

- **Runtime semantics of Source 2's constraints.** Class names, fields and
  authored values are Valve's; how the runtime evaluates them is not observable
  from the asset. Everything in §3.5–3.7 about *mechanism* is marked [inferred].
- **Which field the runtime reads in §3.8**, and therefore whether the bug has
  any visible effect.
- **What `IKSOLVER_TwoBone` does differently from `IKSOLVER_Perlin`.** The
  existence of both is data; the difference is my inference.
- **`AnimConstraintTwist` vs `AnimConstraintTiltTwist`**, and `AnimConstraintPoint`
  / `Morph` / `PoseSpaceMorph` — in the binary's string table, unused by the
  Citizen, so I have no example of their fields.
- **PBIK's convergence behaviour and cost.** I read its structure, not its
  performance; no measurement here is mine.
- **Whether Godot 4.6's solvers are good.** I read the announcement, not the
  code. The licence claim (MIT, engine-wide) is solid; a quality claim would
  need a read of `IKModifier3D` and its subclasses.

---

## 9. Sources

**[S1-SDK]** — Source SDK 2013, local tree at
`E:/Game Development/Tools/source-sdk-2013-master`:

| Path | Used for |
|---|---|
| `src/public/bone_setup.cpp` | `CIKSolver` (2562), `Studio_SolveIK` ×3 (2690–2869), `Studio_AlignIKMatrix` (2752), `Studio_IKRuleWeight` (2875), `CIKContext::UpdateTargets` (3713), `AutoIKRelease` (3949) |
| `src/public/bone_setup.h` | `CIKTarget`, `ikcontextikrule_t`, `ikchainresult_t` |
| `src/public/studio.h` | `mstudioikrule_t` (557), the six `IK_*` type constants (522–527) |
| `LICENSE` | §6.1 |

**[S2-ASSET]** — s&box, `E:/SteamLibrary/steamapps/common/sbox/`:

| Path | Used for |
|---|---|
| `addons/citizen/Assets/models/citizen/prefabs/citizen_ikdata.vmdl_prefab` | §3.1, §3.2 — both IK systems in full |
| `.../prefabs/citizen_animconstraintlist.vmdl_prefab` | §3.5–3.8 — the constraint layer, 24 KB, all quotes are its `note` fields |
| `.../prefabs/citizen_attachmentlist.vmdl_prefab` | §3.6 — the 12 `driver_*` attachments and their influences |
| `.../citizen/citizen.vmdl` | The node census; the ScaleAndMirror note |
| `.../citizen/citizen.vanmgrph` | `CTwoBoneIKAnimNode` instances (14, named e.g. *"Left Arm IKrule"*) |
| `bin/win64/tools/modeldoc_editor.dll`, `hammer.dll` | §3.3's solver enum and §3.5's class vocabulary, read as strings |

Format: `modeldoc30` / `modeldoc29`, KV3 text — same provenance as
[`source2_animation.md`](source2_animation.md).

**[UE-SRC]** — Unreal Engine 5.7, `C:/Program Files/Epic Games/UE_5.7/Engine/`:

| Path | Used for |
|---|---|
| `Source/Runtime/AnimationCore/{Public,Private}/SoftIK.{h,cpp}` | §4.2, quoted in full |
| `Source/Runtime/AnimationCore/` | §4.1's algorithm list |
| `Source/Runtime/AnimGraphRuntime/.../AnimNode_LegIK.{h,cpp}` | §4.3, §4.4's fast path |
| `Plugins/Experimental/FullBodyIK/Source/PBIK/` | §4.4 — `PBIKSolver.{h,cpp}`, `PBIK_Shared.h` |
| `Plugins/Animation/IKRig/Source/IKRig/.../Solvers/` | §4.1's rig-level solver list |

**[OZZ]** — `ik_two_bone_job.h`, `ik_aim_job.h`, `README.md`,
[github.com/guillaumeblanc/ozz-animation](https://github.com/guillaumeblanc/ozz-animation) (MIT).

**[GODOT]** — [*Inverse Kinematics Returns to Godot 4.6*](https://godotengine.org/article/inverse-kinematics-returns-to-godot-4-6/).

**Licences referenced in §6:**
[Source SDK 2013 LICENSE](https://github.com/ValveSoftware/source-sdk-2013/blob/master/LICENSE) ·
[ufbx](https://github.com/ufbx/ufbx) ·
[ACL](https://github.com/nfrechette/acl).

**Related notes:** [`source2_animation.md`](source2_animation.md) for the layer
above (graph, states, masks, damping) — §7 and §12 there are the direct
continuation of this note; [`networked_animation_physics.md`](networked_animation_physics.md)
§2.3 for why the pose must be reconstructible at an arbitrary past time, which
constrains everything here — **note that §2.5's latching and §3.1's springs are
per-frame state, exactly like the damping fork flagged in `source2_animation.md`
§12.4**; [`source_fps_viewmodel.md`](source_fps_viewmodel.md) §8 for Source 1's
animation layer around `bone_setup.cpp`.
