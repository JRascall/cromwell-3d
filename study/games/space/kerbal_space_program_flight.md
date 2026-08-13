# Kerbal Space Program 1 — the flight model

The implementation half of [`kerbal_space_program.md`](kerbal_space_program.md).

That note answers *why KSP is shaped the way it is* — two universes, one seam,
and every famous bug living on it. It is an architecture note, and it will not
let you build one. **This one is the build note.** It covers the path from a key
press to a force on a rigidbody: the control interface, SAS, engines, gimbals,
control surfaces, lifting surfaces, RCS, reaction wheels, the atmosphere as a
physical model and the atmosphere as an image — with the actual equations and,
where they could be recovered, the actual constants.

The governing finding, and it is the same one
[`nuclear_option_control.md`](../flight/nuclear_option/nuclear_option_control.md)
§1 reaches about a completely different game:

> **There is exactly one control interface, it is six floats, and everything
> writes to it.** A keyboard, a joystick, SAS, a manoeuvre-node autopilot, kOS,
> MechJeb and a replicated remote player all fill in the same
> `FlightCtrlState`, and it then goes through the same gimbals, the same control
> surfaces, the same RCS and the same reaction wheels. **No consumer of that
> struct knows or can know where it came from.**

Everything in §1–§6 is downstream of that, and §10 is what to build in what
order if you are actually remaking this.

> **Read alongside:**
> [`kerbal_space_program.md`](kerbal_space_program.md) — the architecture half.
> §1 there (the on-rails/off-rails split) is a precondition for this note: none
> of what follows runs unless the vessel is off rails.
> [`nuclear_option_control.md`](../flight/nuclear_option/nuclear_option_control.md)
> and [`nuclear_option.md`](../flight/nuclear_option/nuclear_option.md) — the
> closest comparable, and the one to steal from where KSP is thin. §4.5 and §6.4
> read them against each other directly.
> [`dcs_world.md`](../flight/dcs/dcs_world.md) §1–2 for the third answer to the
> same question (tabulated whole-airframe coefficients rather than per-part).

---

## 0. Sourcing — what is new here

The tags carry over from the parent note (§0 there). Four sources do most of the
work in this one and they are worth naming individually, because three of them
are unusually strong for a closed-source game:

| tag | source | why it is strong |
|---|---|---|
| **[CODE]** | **`AtmosphereAutopilot/SyncModuleControlSurface.cs`** (Boris-Barboris) — a class that **`extends ModuleControlSurface` and overrides `CtrlSurfaceUpdate`** to reproduce stock behaviour in one phase. | It is a *subclass*, so every field it touches (`ctrlSurfaceRange`, `authorityLimiter`, `deflection`, `neutral`, `baseTransform`, `ctrlSurface`, `ignorePitch`, `useExponentialSpeed`, `actuatorSpeed`, `mirrorDeploy`) is stock, and its arithmetic had to match stock closely enough that aircraft still fly. **§4 is essentially source.** |
| **[CODE]** | **`Kopernicus/AtmosphereFromGroundLoader.cs`** — the loader for KSP's own `AtmosphereFromGround` component (which `CelestialBody.afg` points at). | It sets `Kr`, `Km`, `ESun`, `g`, `waveLength`, `scaleDepth`, `samples` and derives `Kr4PI`, `KmESun`, `scaleOverScaleDepth`. That is Sean O'Neil's uniform set field-for-field, **with KSP's shipped default values**. §8. |
| **[COMMUNITY]** | **`Ren0k/Project-Atmospheric-Drag`** — a step-by-step re-derivation of stock aerodynamics from `Physics.cfg` and `PartDatabase.cfg`, checked against in-game readouts. | §5's lift, profile-drag and induced-drag equations are quoted from it, including the global multipliers. |
| **[API]** | The KSP API doxygen. | `ModuleEngines`, `FlightCtrlState`, `CelestialBody` field lists are real. Prose comments hedge and are treated accordingly. |

**What is still missing is stated in §11 and it is not small.** In particular
**§9 (PQS quad construction) is mostly a list of what could not be established**,
and the SAS section is thinner than it should be.

---

## 1. `FlightCtrlState` — the one interface

### 1.1 Six floats and three trims

**[API]** *"A `FlightCtrlState` is a snapshot of the state of all control inputs
to a vessel at a given instant in time."*

```csharp
float pitch, yaw, roll;          // −1 … +1
float pitchTrim, yawTrim, rollTrim;
float mainThrottle;              // 0 … 1
float X, Y, Z;                   // RCS translation, −1 … +1
bool  killRot;                   // "Whether SAS is turned on"
```

**[inferred]** That is the entire vocabulary for commanding a spacecraft in KSP.
Three rotational axes, three translational, one throttle. Everything else in the
game — staging, gear, lights, action groups — travels on a different channel
(events), which is the same split
[`broken_arrow_animation.md`](../flight/broken_arrow/broken_arrow_animation.md)
§2 finds: **continuous state on one channel, discrete events on another, each
carrying what it is good at.**

Compare Nuclear Option's `ControlInputs`, also six floats, also written by a
stick and by an AI alike. Two space/flight games, no shared code, identical
answer. **The interface is forced by the problem: a rigid body has six degrees
of freedom, and a throttle.**

### 1.2 The chain, and where things insert

**[API]** `FlightInputHandler.state` holds the player's raw input;
`Vessel.ctrlState` holds the vessel's current one; `Vessel.OnFlyByWire` is a
callback list that mods hook.

**[COMMUNITY]** The documented behaviour of `OnFlyByWire` is that the state
*"may arrive with SAS or WASD controls pre-applied"*.

**[inferred]** So the order per physics step is:

    raw input  →  trim added  →  SAS writes/overrides  →  OnFlyByWire callbacks
                                                       →  Vessel.ctrlState
                                                       →  every PartModule reads it

and the important property is the last arrow: **the parts pull from the vessel's
state rather than being pushed to.** `SyncModuleControlSurface` reads
`vessel.ctrlState.pitch` directly, in its own update, per part. That makes each
part module independent, order-free and trivially addable — a new control
surface type is a module that reads the same three floats — at the cost that
nothing can see the *set* of surfaces and coordinate them. §4.2 is where that
bill comes due.

### 1.3 The reference transform is the control point

**[CODE]** All of the control-surface maths is expressed against
`vessel.ReferenceTransform`, and the axes used are:

| axis | used for |
|---|---|
| `ReferenceTransform.up` | **the nose direction** — and the roll axis |
| `ReferenceTransform.right` | the pitch axis |
| `ReferenceTransform.forward` | the yaw axis |

**[inferred]** Worth stating loudly because it catches everyone: **in KSP a
vessel's "forward" is its transform's `up`.** A command pod points along +Y. That
falls out of rockets being built vertically in the VAB and it propagates into
every piece of control code in the game.

The reference transform is the **currently controlling part** — the command pod,
or a docking port if you "control from here". Two consequences that are real
gameplay:

- Changing the control point **re-labels the axes**, which is why you control
  from a docking port when berthing: the port's forward becomes the nose, and
  translation controls line up with what you are looking at.
- SAS measures angular velocity **at that transform** (§2.1), which is why
  where you put it changes the flight characteristics of an identical rocket.

---

## 2. SAS

### 2.1 A PD controller on angular velocity, sampled at the control point

**[COMMUNITY]** The documented mechanism:

> "The SAS modules use a PID system that is applied to the vessel's **angular
> velocity, not its heading**. The SAS functionality found within command modules
> and standard SAS units use only **PD, with no I**, and thus do not lock the
> heading of the vessel. The proportional component means the SAS module applies
> a turning-force that is proportional to the speed of rotation."

**[inferred]** So base stability assist is a **rate damper**, not an attitude
hold. It drives ω → 0 and does not care where you end up pointing. That is why
early-game SAS stops a tumble but drifts off heading, and it is the correct
cheap primitive: damping a rate needs no target and cannot fight the pilot.

**[COMMUNITY]** And the consequence that connects straight back to the parent
note's §4:

> "The PID control is applied to the vessel's rotational velocity **at the
> command point**. This means that large rockets that tend to wobble out of
> control during flight do so because **the SAS sees the tip (where the command
> module usually is) rotating and assumes the entire vessel is spinning this
> fast**. The wobble can be largely eliminated by controlling the vessel as close
> to the CG as possible."

**[inferred]** That is an independent confirmation of
[`kerbal_space_program.md`](kerbal_space_program.md) §4.5, arrived at from the
player side rather than the solver side. The rate gyro is mounted at the end of
a flexible beam and is therefore measuring **beam tip motion plus rigid-body
rotation**, and the controller cannot tell them apart. Real launch vehicles have
exactly this problem, solve it with notch filters at the known bending
frequencies, and *also* care a great deal where the IMU is bolted.

**Two things to steal**: a rate damper is the cheap correct default, and **where
you sample the state is part of the controller design, not an implementation
detail.**

### 2.2 The modes are target directions, not different controllers

**[COMMUNITY]** The mode set:

    STABILITYASSIST  PROGRADE  RETROGRADE  NORMAL  ANTINORMAL
    RADIALOUT  RADIALIN  TARGET  ANTITARGET  MANEUVER

**[inferred]** Nine of those ten are **the same controller with a different
setpoint**, and every setpoint is a unit vector the game already has for the
navball ([`kerbal_space_program.md`](kerbal_space_program.md) §10.1). Prograde is
the velocity direction; normal is the orbit normal; radial is the position
direction; manoeuvre is `ManeuverNode.GetBurnVector()`. **The autopilot and the
instrument consume the identical vectors** — which is why the marker on the ball
and the direction the ship swings to are guaranteed to agree, with no second
code path to keep in step.

Stability assist is the odd one out precisely because it has *no* setpoint, and
that is the correct way to model "hold what I've got".

**[inferred]** Note also that SAS mode is stated to reset to stability assist
whenever SAS is toggled on. Cheap, and the right default: the mode that cannot
be wrong.

### 2.3 SAS writes the same six floats

**[inferred]** The critical structural point, and the reason this section is in
this note rather than a UI one: **SAS output is `pitch`/`yaw`/`roll` in
`FlightCtrlState`.** It does not apply torque. It does not talk to reaction
wheels or gimbals or control surfaces. It writes the struct a keyboard writes,
and the actuators downstream do what they would have done for a human.

Three things fall out for free, and each is a bug that does not happen:

1. **SAS cannot cheat.** It is limited to exactly the authority the vessel has.
   A craft with no control surfaces, no RCS and no reaction wheels flies exactly
   as badly under SAS as under a pilot.
2. **Every actuator improvement improves SAS**, at no cost, forever.
3. **The player can fight it**, and the result is the sum of two inputs to one
   struct rather than a mode conflict.

Nuclear Option's note reaches this conclusion and calls it the single most
important structural decision in that codebase. KSP made the same one.

---

## 3. Engines

### 3.1 Thrust is applied at transforms, so torque is emergent

**[API]** `ModuleEngines` — *"All stock propulsion systems except for RCS are
implemented through `ModuleEngines`"* — carries

```csharp
List<Transform> thrustTransforms;   // "the locations and directions at which the
                                    //  thrust this engine generates is applied"
float maxThrust, minThrust;         // kN
FloatCurve atmosphereCurve;         // Isp as a function of atmospheric pressure
FloatCurve velocityCurve;           // thrust vs airspeed
bool  useEngineResponseTime;
float engineAccelerationSpeed, engineDecelerationSpeed;
float currentThrottle, requestedThrottle, finalThrust, realIsp;
bool  throttleLocked;               // solid boosters
bool  exhaustDamage;
```

**[inferred]** `thrustTransforms` being a **list of transforms** rather than a
vector is the whole design. Thrust is applied as a force **at a point, in that
point's direction**, split across the transforms — so:

- **Off-axis engines produce torque with no torque code.** A radial booster
  yaws the stack because the force is applied where the nozzle is.
- **A gimbal is a rotation of the transform** and nothing else (§3.5).
- **CoM shift as tanks drain automatically changes the moment arm**, so a
  rocket's handling degrades or improves through a burn for free.
- **A multi-nozzle engine is N transforms sharing `maxThrust`**, so a nozzle
  arrangement matters visually and physically at once.

This is the same principle Nuclear Option applies to aerodynamics —
[`nuclear_option.md`](../flight/nuclear_option/nuclear_option.md) §2, *the
vehicle's geometry is its physics model* — applied here to propulsion. **Nothing
computes "pitching moment"; it exists because a force was applied somewhere that
is not the centre of mass.**

**[API]** `exhaustDamage` is a nice small one: *"a ray is cast back along the
engine's thrust vector and if that ray hits a part then that part gets heated
up, and may explode"*. One raycast per engine per step buys the entire "don't
mount an engine facing your own fuel tank" rule.

### 3.2 Isp, and the fuel-flow equation

**[API]** `atmosphereCurve` is *"the specific impulse (Isp) of the engine as a
function of altitude"* — in practice a `FloatCurve` keyed on **atmospheric
pressure in atmospheres**, e.g.

```
atmosphereCurve { key = 0 320    // vacuum Isp, seconds
                  key = 1 250 }  // sea-level Isp
```

**[inferred]** Then, with `g₀ = 9.80665 m/s²` (which is what `ModuleEngines.G` is
almost certainly for):

    Isp    = atmosphereCurve.Evaluate(staticPressure_atm)
    thrust = maxThrust · currentThrottle · (thrustPercentage/100)      [kN]
    ṁ      = thrust / (Isp · g₀)                                       [t/s]

and the propellant is drawn from the `propellants` list in its authored ratios.

That single curve is doing a lot of design work. It is why vacuum engines are
useless at sea level and sea-level engines are inefficient in vacuum, it is why
the game's whole staging logic exists, and it is **one authored curve per engine
rather than a nozzle-flow model** — which is exactly right, because the shape of
Isp-vs-pressure is well known, cheap to author and the physics behind it is
irrelevant to the player's decision.

**[inferred]** Note the direction of the dependency: **KSP tabulates the
performance and derives the flow.** A more "physical" engine model would compute
thrust from chamber pressure, expansion ratio and ambient pressure and let Isp
fall out. KSP does the opposite, and it is the better call for a game, because
the tabulated quantity is the one the player is choosing between.

### 3.3 Spool-up is one lerp, and it matters more than it looks

**[API]** `useEngineResponseTime` with `engineAccelerationSpeed` /
`engineDecelerationSpeed`, driving `currentThrottle` toward `requestedThrottle`.
The doc is explicit that `currentThrottle` *"may be different from the current
throttle set by the player"*.

**[inferred]** Separate accel and decel rates, which is physically right — a
turbopump spins up slowly and a valve closes fast — and the reason it matters is
**stability**: an instant-response engine inside the SAS loop of §2.1 has no
phase lag, and adding one changes the closed-loop behaviour of every craft in the
game. A jet with a slow spool is genuinely harder to fly, and none of that is
coded anywhere; it is two floats.

### 3.4 Jets are the same module with two extra curves

**[COMMUNITY]** Air-breathing engines use `velCurve` (thrust vs Mach) and
`atmCurve` (thrust vs intake air / atmospheric density) on top of the same
`atmosphereCurve` Isp.

**[inferred]** So a turbojet, a ramjet-ish high-speed engine and a rocket are
**one code path and three curve sets**. The flame-out at altitude is the atm
curve going to zero and the propellant request failing —
`getFlameoutState` is documented as *"the engine is not producing thrust because
it can't get enough resources"*, and intake air is just a resource. **The
distinguishing behaviour of an entire engine class is a resource that runs out.**

### 3.5 Gimbal

**[COMMUNITY]** `ModuleGimbal` — `gimbalRange` (degrees), `gimbalResponseSpeed`,
`enablePitch`/`enableYaw`/`enableRoll` — rotates the gimbal transform, which the
thrust transform is parented under.

**[inferred]** So the gimbal writes a rotation and **the force follows from
§3.1**. It never computes a moment. The demand comes from the same
`ctrlState.pitch/yaw/roll` the control surfaces read, mapped through the same
kind of geometric test as §4.2 — which is why a gimbal on a centre engine gives
you pitch and yaw but no roll (its thrust line passes through the roll axis, so
the cross product is zero), and why you need either a second off-axis engine or
vernier RCS for roll authority. **Nobody wrote that rule.**

---

## 4. Control surfaces

**This is the section that is closest to being source.** `ModuleControlSurface`
extends `ModuleLiftingSurface`, and its actuation logic is reproduced by
`SyncModuleControlSurface`, which subclasses it.

### 4.1 One module, five surface types, zero authoring

**[CODE]** The full set of authored fields is small:

```
ctrlSurfaceRange     // max deflection, degrees (15 is the stock default)
ctrlSurfaceArea      // fraction of the part's lift area that actuates, 0…1
deflectionLiftCoeff  // the part's lift "area" (inherited from ModuleLiftingSurface)
authorityLimiter     // percent, player-tweakable in the editor
actuatorSpeed        // with actuatorSpeedNormScale, if useExponentialSpeed
ignorePitch / ignoreRoll / ignoreYaw   // player-tweakable
deploy, deployAngle, deployInvert, partDeployInvert, usesMirrorDeploy
```

**[inferred]** There is **no field saying "this is an elevator"**. Nothing
declares a surface to be an aileron, a rudder, an elevon or a canard. Whether a
given surface responds to pitch, to roll, to yaw or to all three — **and in which
direction** — is computed every physics step from where it is and which way it
faces. That is §4.2, and it is the best idea in KSP's flight model.

### 4.2 The actual maths

**[CODE]** From `SyncModuleControlSurface.CtrlSurfaceUpdate`, annotated. `vel` is
the airflow, `baseTransform` is the surface's own transform, `world_com` is
`vessel.CoM`:

```csharp
float pitch_input = ignorePitch ? 0f : vessel.ctrlState.pitch;
float roll_input  = ignoreRoll  ? 0f : vessel.ctrlState.roll;
float yaw_input   = ignoreYaw   ? 0f : vessel.ctrlState.yaw;

if (vessel.atmDensity == 0.0)                       // (a) vacuum: no authority
    pitch_input = roll_input = yaw_input = 0f;

float fwd = Mathf.Sign(Vector3.Dot(vessel.ReferenceTransform.up,
                                   vessel.srf_velocity) + 0.1f);   // (b)

// ---- pitch ----------------------------------------------------------
float axis_factor  = Vector3.Dot(vessel.ReferenceTransform.right,
                                 baseTransform.right) * fwd;       // (c)
float pitch_factor = axis_factor *
    Math.Sign(Vector3.Dot(world_com - baseTransform.position,
                          vessel.ReferenceTransform.up));          // (d)

// ---- roll -----------------------------------------------------------
axis_factor = Vector3.Dot(vessel.ReferenceTransform.up,
                          baseTransform.up) * fwd;
float roll_factor = axis_factor *
    Math.Sign(Vector3.Dot(vessel.ReferenceTransform.up,
        Vector3.Cross(world_com - baseTransform.position,
                      baseTransform.forward)));                    // (e)

// ---- yaw ------------------------------------------------------------
axis_factor = Vector3.Dot(vessel.ReferenceTransform.forward,
                          baseTransform.right) * fwd;
float yaw_factor = axis_factor *
    Math.Sign(Vector3.Dot(world_com - baseTransform.position,
                          vessel.ReferenceTransform.up));

// ---- sum, clamp, scale ----------------------------------------------
deflection = ctrlSurfaceRange * authorityLimiter * 0.01f *
             Clamp(pitchN + rollN + yawN, 1f);                     // (f)
ctrlSurface.localRotation = Quaternion.AngleAxis(deflection, Vector3.right)
                          * neutral;                               // (g)
```

Reading it line by line:

**(c) — how much of this axis does this surface see?** The dot product of the
vessel's pitch axis with the surface's own right axis. A horizontal tailplane has
its hinge parallel to the vessel's pitch axis, so the dot is ±1 and it is a full-
authority elevator. A vertical fin has its hinge perpendicular, so the pitch dot
is 0 and the **yaw** dot is ±1 — it is a rudder. A wing at 45° gets 0.7 of both
and is an elevon. **The surface's authority on each axis is the cosine of its
misalignment with that axis, and that is the entire classification system.**

**(d) — which way should it deflect?** The sign of the lever arm: is the surface
ahead of or behind the centre of mass, along the nose axis? A tailplane is behind
the CoM and deflects one way; **a canard is ahead of it and deflects the other**,
automatically, because the sign flips. Nobody authored "canards work backwards".

**(e) — roll needs a cross product, not a dot.** Roll authority is about the
moment arm *around* the nose axis, so it is `sign(dot(nose, cross(r, chord)))`.
That is what makes the left and right ailerons deflect oppositely with no mirror
logic in the control path: their `r` vectors point opposite ways, so the cross
products have opposite sign.

**(b) — flying backwards reverses everything.** `fwd` is the sign of the airflow
along the nose, with a `+0.1` bias so it does not chatter at zero. Multiply every
axis factor by it and **a craft moving tail-first has all its controls inverted
— correctly**, because so does a real one.

**(f) — one surface can be three surfaces at once.** The three normalised
deflections are **summed and clamped to 1**, so an elevon commanded to pitch up
and roll left does both until it saturates. Saturation is exactly what a real
surface does, and it is one `Clamp`.

**(a) — vacuum is a hard gate.** `atmDensity == 0` zeroes the input. **[inferred]**
This is the one place where KSP is *less* elegant than Nuclear Option, whose
control authority falls off with dynamic pressure automatically because the
surface is just a wing and there is no gate at all
([`nuclear_option.md`](../flight/nuclear_option/nuclear_option.md) §4). KSP's
version is a discrete test that stops the actuator moving rather than letting the
force go to zero on its own. It costs nothing in play — the force *would* be zero
— but it means the visible surface freezes in vacuum instead of flapping
uselessly, which is a presentation decision wearing a physics test's clothes.

### 4.3 Actuator dynamics

**[CODE]** Two modes. Linear:

```csharp
prev = prev + Clampf(target - prev, spd_factor * Mathf.Abs(axis_factor));
// spd_factor = TimeWarp.fixedDeltaTime * CSURF_SPD   (2.0 in this reimplementation)
```

or exponential, if `useExponentialSpeed`:

```csharp
exp_spd_factor = actuatorSpeed / actuatorSpeedNormScale * TimeWarp.fixedDeltaTime;
prev = Mathf.Lerp(prev, target, exp_spd_factor);
```

**[inferred]** The rate limit is scaled by `|axis_factor|` in the linear path,
so a surface with weak authority on an axis also *slews* slowly on that axis —
which keeps the summed deflection consistent when the axes saturate together.

The exponential path is a first-order lag, i.e. a real actuator, and it
introduces phase lag into the loop the SAS of §2.1 is closing. Both of those
knobs are per-part and player-visible.

### 4.4 The deflection produces no force — it rotates a transform

**[CODE]** The last line of the whole routine:

```csharp
ctrlSurface.localRotation = Quaternion.AngleAxis(deflection, Vector3.right) * neutral;
```

**[inferred] This is the most important line in the flight model and it is worth
stopping on.** `CtrlSurfaceUpdate` never applies a force, never computes a
moment, and never touches a rigidbody. It **rotates a child transform**, and then
`ModuleLiftingSurface` (§5) does what it always does — resolve the airflow
against that transform's orientation and produce a lift force at the part's
position.

So:

- **Control authority falling off at low dynamic pressure is emergent**, because
  lift is `q · A · Cl` and `q` is in there.
- **Control-surface *stall* is emergent**, because past the peak of the lift
  curve, more deflection gives less force.
- **Pitching moment is emergent**, because the force is applied at the part.
- **Adverse yaw from ailerons is emergent**, because the up-going and down-going
  surfaces have different induced drag (§5.3).

Not one of those is implemented. This is Nuclear Option's *"control surfaces
produce a quaternion and nothing else"* finding
([`nuclear_option.md`](../flight/nuclear_option/nuclear_option.md) §4) reached
independently, by a different studio, four years earlier. **Two games, and both
concluded the correct output of a control-surface module is a rotation.**

### 4.5 Where KSP is thinner than Nuclear Option

**[inferred]** Being fair about the comparison, since §4.4 is the flattering
half:

| | **KSP** | **Nuclear Option** |
|---|---|---|
| authority classification | from geometry, per step | from geometry, per step |
| output of the surface module | a quaternion | a quaternion |
| force model behind it | one `Cl(sin α)` curve, four named curve sets shared by every part in the game | per-part airfoil, resolved in each part's own frame |
| sideslip | needs no term (a fin is a rotated surface) | needs no term (same reason) |
| vacuum | **explicit gate** | no gate needed; `q` handles it |
| actuator | rate limit or first-order lag | rate limit |

The structural ideas are the same. **The difference is entirely in the fidelity
of the force model behind the transform**, which is the right place for two games
with different subjects to differ — and it means KSP's control-surface code
transfers to a more serious flight model unchanged.

---

## 5. Lifting surfaces and the force model

### 5.1 A wing is `deflectionLiftCoeff` and a curve set

**[COMMUNITY]** *"You will find that wings have a `dragModelType` property of
`none`, i.e. they do not use drag cubes. […] All wings have a module called
`ModuleLiftingSurface`. […] We are actually only interested in a single property,
called `deflectionLiftCoeff`. `deflectionLiftCoeff` defines the Area of the wing,
but also not really. It is just a modifier that we use in the place of 'A' in the
drag equation."*

**[CFG]** The curves come from `Physics.cfg`'s `LIFTING_SURFACE_CURVES`, of which
there are exactly four named sets — `Default`, `BodyLift`, `CapsuleBottom`,
`SpeedBrake` — each with `lift`, `liftMach`, `drag`, `dragMach`.

**[inferred] So the entire aerodynamic character of every wing in the game is
four curves plus one number per part.** That is an extremely aggressive
simplification and it is defensible for the same reason drag cubes are
([`kerbal_space_program.md`](kerbal_space_program.md) §8.2): the differences
between wing sections are small compared with the difference between having a
wing and not.

### 5.2 The three forces

**[COMMUNITY]** With `q = ρv²/2` and `α` the angle of attack, and noting that
the curves are **indexed by sin α, not α**:

    Cl      = lift.Evaluate(sin α) · liftMach.Evaluate(M)
    Cd      = drag.Evaluate(sin α) · dragMach.Evaluate(M)

    Lift          = q · deflectionLiftCoeff · Cl · 36      [N]   (liftMultiplier 0.036)
    Profile drag  = q · deflectionLiftCoeff · Cd · 15      [N]   (liftDragMultiplier 0.015)
    Induced drag  = sin α · Lift                            [N]

**[inferred]** Three observations that matter if you are building this.

**Induced drag is literally `sin(α) × lift`**, and the source is explicit about
why: *"When a lifting surface produces lift, its lift vector tilts in the
opposite direction of motion."* KSP does not compute induced drag as a separate
phenomenon — **the lift vector is not perpendicular to the airflow**, it tilts
back as AoA rises, and its rearward component *is* the induced drag. That is one
of the cheapest correct-feeling things in the whole model: you get the
lift-drag polar, the reason high-AoA flight bleeds energy, and adverse yaw, from
tilting one vector.

**Indexing on `sin α` removes an `asin` from the per-part inner loop.** The dot
product you already have is the lookup key.

**Profile drag and lift use different global multipliers** (0.015 vs 0.036) on
the same area, so the L/D ratio of every wing in the game is set by two numbers
in a config file.

### 5.3 Body lift is the same code with a different curve set

**[COMMUNITY]** *"You will also find parts that both use drag cubes and have a
`ModuleLiftingSurface`. […] These parts do not make use of wing profile drag, but
lift and induced drag are valid properties."*

**[CFG]** `bodyLiftMultiplier = 18` — 500× the `liftMultiplier`, applied to a
`deflectionLiftCoeff` that *"is usually quite a high value"*.

**[inferred]** So a fuselage generates lift at angle of attack, from the same
curve machinery, with a separate global scalar so it can be tuned without
touching wings. That is what makes spaceplane fuselages and re-entering capsules
fly rather than just fall, and `CapsuleBottom` existing as its own curve set
(with a **constant** `liftMach` of 0.0625) is the game admitting that a blunt
heat shield is a different aerodynamic object than a wing and giving it two
lines rather than a system.

### 5.4 Where the forces are applied

**[inferred]** Each of these is applied **at the part**, not at the vessel — same
as thrust (§3.1). So the vessel's aerodynamic centre, its static margin, its
pitch stability and its tendency to weathervane are all **emergent from where the
parts are**, and moving a wing 30 cm aft in the editor genuinely changes the
handling. There is no stability derivative anywhere in the game.

This is the payoff of the per-part model and it is why KSP can let players build
arbitrary aircraft at all: **a model that computes forces per part needs no
knowledge of the configuration, and therefore has no configurations it cannot
handle.** A whole-airframe coefficient table — DCS's approach,
[`dcs_world.md`](../flight/dcs/dcs_world.md) §2 — is far more accurate for the
airframes it has and cannot represent one it does not.

---

## 6. RCS, reaction wheels, and what "acts like a spaceship" is made of

### 6.1 RCS

**[COMMUNITY/API]** `ModuleRCS` carries `thrusterPower` (kN), a **list** of
`thrusterTransforms`, an `atmosphereCurve` for Isp exactly like an engine, and
per-axis enables — `enablePitch`, `enableYaw`, `enableRoll`, `enableX`,
`enableY`, `enableZ`.

**[inferred]** The dispatch has to be, for each thruster with position `r`
relative to CoM and thrust direction `d`:

    torque_contribution      = cross(r, d)          → dot with (pitch,yaw,roll) demand
    translation_contribution = d                    → dot with (X,Y,Z) demand
    throttle = clamp(sum of the two dots, 0, 1)     // a nozzle can only push

and each thruster then fires at its own throttle. **[COMMUNITY]** The stock
implementation is confirmed naive in exactly the way that predicts: *"the stock
implementation would generate thrust for both forces independently, potentially
causing opposite thrusters on the same RCS block to fire simultaneously against
each other"*, and *"requested thrust for a rotational input uses the RCS thruster
position, not the RCS part position"*.

**[inferred]** Two real consequences that players experience as the game's
personality:

- **Unbalanced RCS translates when you rotate**, because nothing solves for the
  set — each nozzle independently decides it is helping. Placing RCS blocks
  symmetrically about the CoM is a *player-side* least-squares solve, done by
  eye, and it is one of the game's genuine skills.
- **It gets worse as fuel drains**, because the CoM moves and the arms change.

The alternative — a proper constrained solve over all thrusters for the demanded
wrench — is what `RCSBuildAid` and Throttle Controlled Avionics exist to
approximate. **KSP left it naive, and the naivety is the gameplay.** Worth
noting as a design choice rather than an oversight: a solver would make bad
designs fly correctly and delete the reason to place thrusters carefully.

### 6.2 Reaction wheels

**[COMMUNITY]** `ModuleReactionWheel` — `PitchTorque`, `YawTorque`, `RollTorque`
in kN·m, plus a resource cost — applies torque to the vessel directly.

**[inferred]** And **no momentum budget**. Stock reaction wheels never saturate,
so they are a free infinite attitude authority limited only by electric charge.
This is the single largest departure from reality in KSP's flight model and it is
deliberate: real reaction wheels saturate and need RCS or magnetorquers to
desaturate, which is a maintenance mechanic nobody wants in a game about getting
to Duna.

Read it against §6.1 and there is a clean design statement: **KSP made the
realistic system (RCS) the one you have to be clever about, and the unrealistic
system (wheels) the one that just works.** The player is taught by the honest
system and rescued by the forgiving one.

### 6.3 What makes it feel like space

**[inferred]** Assembling §3 and §6, "acting like a spaceship" turns out to be
about four properties, none of which is a feature:

1. **Nothing damps rotation.** In vacuum there is no aerodynamic force, so
   angular momentum is conserved and a spin persists forever. This is the whole
   feeling, and it comes from *not* adding drag rather than from adding physics.
   (Unity's rigidbody `angularDrag` must be zero, or it silently ruins it.)
2. **Every force is applied at a point**, so translation and rotation are
   coupled through geometry rather than through code.
3. **The mass distribution changes continuously** as tanks drain, so the CoM
   moves through the burn and the moment arms with it.
4. **Control authority is limited and directional**, so pointing costs something.

A remake that gets those four right will feel like KSP with a much simpler force
model. A remake that gets them wrong will not feel like KSP no matter how good
the aerodynamics are.

---

## 7. The atmosphere as a physical model

**[API]** `CelestialBody` carries:

```csharp
bool       atmosphere;
double     atmosphereDepth;                       // where it ends
FloatCurve atmospherePressureCurve;               // pressure vs altitude
FloatCurve atmosphereTemperatureCurve;            // temperature vs altitude
bool       atmosphereUsePressureCurve, atmosphereUseTemperatureCurve;
bool       atmospherePressureCurveIsNormalized, atmosphereTemperatureCurveIsNormalized;
double     atmospherePressureSeaLevel, atmosphereTemperatureSeaLevel;
double     atmosphereTemperatureLapseRate, atmosphereGasMassLapseRate;
double     atmosphereMolarMass;                   // kg/mol
double     atmosphereAdiabaticIndex;              // γ
bool       atmosphereContainsOxygen;              // "whether jet engines will work"
FloatCurve atmosphereTemperatureSunMultCurve;
FloatCurve latitudeTemperatureBiasCurve;
FloatCurve axialTemperatureSunMultCurve;
double     atmDensityASL;
```

**[inferred]** Three readings.

**Pressure is a curve, with an exponential fallback.** Both a curve *and* a sea-
level value plus a lapse rate ship, with `atmosphereUsePressureCurve` selecting.
The exponential model (`P = P₀·e^(−h/H)`) is the pre-1.0 one and is still the
fallback; the curve is the 1.0 replacement. **[inferred]** Keeping both is the
same add-beside-and-leave-the-old-one-loaded migration
[`dcs_clouds.md`](../flight/dcs/dcs_clouds.md) §8 finds in DCS's mission format
and [`dcs_world.md`](../flight/dcs/dcs_world.md) §5 finds in its damage models.
It is apparently what shipped engines always do.

**Everything downstream is the ideal gas law.** With `R = 8.31446`:

    ρ = P · M / (R · T)                    density
    a = sqrt(γ · R · T / M)                speed of sound
    M_number = v / a

so density and Mach — the two inputs the whole of §5 and
[`kerbal_space_program.md`](kerbal_space_program.md) §8 run on — come from two
curves and two constants per body. **The aerodynamic model needs exactly four
numbers from the atmosphere and the atmosphere model exists to produce them.**

**Temperature varies with latitude, sun angle and axial tilt** — three separate
curves for it. **[inferred]** This is more effort than the model needs for
plausibility and it exists because temperature feeds the thermal system
([`kerbal_space_program.md`](kerbal_space_program.md) §9): re-entry heating,
radiator performance and solar panel output all read it, so a constant would have
made the day/night cycle mean nothing.

---

## 8. The atmosphere as an image

### 8.1 The chain of evidence is complete, which is rare here

**[HARVESTER]**, in 2011, on the 0.4 terrain:

> "It even had an atmosphere to it. This was done with 'atmospheric scattering'
> shaders, which were made accessible to the world by a guy called **Sean
> O'Neil**, who wrote an article about that on a developer publication called
> **GPU Gems**."

**[API]** `CelestialBody.afg` is of type **`AtmosphereFromGround`** — and
`GroundFromAtmosphere` / `SkyFromAtmosphere` / `GroundFromSpace` /
`SkyFromSpace` are the names of the four shader variants in O'Neil's GPU Gems 2
chapter 16 sample.

**[CODE]** And Kopernicus's loader for that component sets and derives exactly
O'Neil's uniform set:

```csharp
afg.g2                 = afg.g * afg.g;
afg.KrESun             = afg.Kr * afg.ESun;
afg.KmESun             = afg.Km * afg.ESun;
afg.Kr4PI              = afg.Kr * 4f * Mathf.PI;
afg.Km4PI              = afg.Km * 4f * Mathf.PI;
afg.outerRadius2       = afg.outerRadius * afg.outerRadius;
afg.innerRadius2       = afg.innerRadius * afg.innerRadius;
afg.scale              = 1f / (afg.outerRadius - afg.innerRadius);
afg.scaleOverScaleDepth= afg.scale / afg.scaleDepth;
```

**Claim, mechanism and constants, all three.** That is the best-evidenced single
system in either of these two notes.

### 8.2 KSP's shipped values

**[CODE]** From `SetDefaultValues()`:

| uniform | KSP | O'Neil's sample | reading |
|---|---|---|---|
| `ESun` | **30** | 20 | brighter sun |
| `Kr` (Rayleigh) | **0.00125** | 0.0025 | half |
| `Km` (Mie) | **0.00015** | 0.0010 | **a sixth** |
| `g` (Mie asymmetry) | **−0.85** | −0.990 | much less forward-scattering |
| `waveLength` | **(0.65, 0.57, 0.475)** | (0.650, 0.570, 0.475) | identical — the RGB wavelengths in µm |
| `samples` | **4** | 2–4 | raymarch steps |
| `scaleDepth` | **−0.25** | 0.25 | sign convention |
| `outerRadius` | `planetRadius × 1.025` | — | **a 2.5% shell** |
| `innerRadius` | `outerRadius × 0.975` | — | |

and `invWaveLength = 1/λ⁴` per channel — Rayleigh's inverse-fourth-power law,
which is *why the sky is blue*, computed once at load rather than in the shader.

**[inferred]** The interesting deltas are `Km` and `g`. Cutting Mie scattering to
a sixth and softening its phase function from −0.99 to −0.85 removes the tight,
bright forward haze around the sun and leaves mostly Rayleigh — a cleaner, more
"cartoon planet from orbit" look, and a sky that reads well from *inside* the
atmosphere at any sun angle rather than only at sunset. **They tuned away from
photographic and toward legible**, which is consistent with everything else about
KSP's art.

`samples = 4` is the honest number: four raymarch steps through the shell, on
2011 hardware, on a vertex-heavy shader.

### 8.3 The shell, and what it costs

**[inferred]** The atmosphere is a **sphere 2.5% larger than the planet**, drawn
with a shader that integrates optical depth between the camera and each vertex.
`doScale` setting `transform.localScale = 1.025` is that shell being sized.

The four shader variants exist because the integration has different limits
depending on whether the camera is inside or outside the shell, and whether you
are shading the sky shell or the ground under it. **Camera position selects the
shader**, which is a 2005 solution and is why crossing the atmosphere boundary in
KSP can pop.

Three things this model cannot do, and all three are visible in the shipped game:

- **No aerial perspective on the terrain at close range.** Ground haze is
  computed per *vertex* on the PQS quads, so it is only as good as the tessellation.
- **No cloud interaction, no volumetrics, no light shafts.** There is nothing in
  the shell but analytic density.
- **No multiple scattering.** O'Neil's is a single-scattering approximation with
  a fitted phase function, which is why the twilight band is thinner and less
  colourful than a real one.

**[COMMUNITY]** The `scatterer` mod replaces the whole thing with a
Bruneton-style precomputed multiple-scattering model, which is the direct
lineage from O'Neil and the thing to build if you are starting today.

### 8.4 Why this is the right decision to copy anyway

**[inferred]** O'Neil's model is **analytic, needs no precomputation, needs no
lookup textures and runs on anything**. For a game whose atmosphere must work at
600 km altitude, at sea level, on five different bodies with different colours,
and whose author was one person in 2011, it is the correct choice by a distance —
and the fact that it lasted the entire lifetime of the game with only a mod
replacing it says the fidelity ceiling was never the binding constraint.

Compare [`dcs_clouds.md`](../flight/dcs/dcs_clouds.md) §1, where ED shipped a
raymarched cloud system, **withdrew it**, and waited seven years for hardware.
KSP shipped the cheap analytic model and never had to.

---

## 9. PQS quad construction — what is established and what is not

**[CODE]** The stock PQS's own tunables, from Kopernicus's loader:

```
minLevel  maxLevel                  // subdivision depth (commonly 2 and 10)
minDetailDistance
maxQuadLengthsPerFrame              // a build budget
fadeStart  fadeEnd  deactivateAltitude
mapMaxHeight
materialType  Material  FallbackMaterial
PhysicsMaterial { bounciness = 0.0  staticFriction = 0.8  dynamicFriction = 0.6 }
Mods { ... }
```

**[COMMUNITY]** *"Planets in KSP are patchworks of 2D grids that have their
vertices displaced to the surface of a sphere. The closer a tile is to the
camera, the more detailed it is."*

**[inferred]** What can be said with confidence, from the parent note's §7.4–7.5
plus the above:

- A quad is a **fixed-resolution grid** displaced by the modifier stack; it
  subdivides into four children rather than adding vertices, so vertex count per
  quad is constant and only quad *count* varies.
- Subdivision is **distance-driven**, and the conservative shell of §7.5 is what
  makes the test cheap.
- `maxQuadLengthsPerFrame` is a **build budget** — quads are generated
  incrementally, which is the mechanism behind HarvesteR's *"you will never be
  able to outrun the terrain"*.
- Colliders exist only at deep levels, and `PhysicsMaterial` is per-body.

**What is NOT established, and this is the biggest hole in either note:**

- **The vertex count per quad.** Community lore says 32×32; no source here
  confirms it.
- **The subdivision criterion.** Distance-based, but the exact metric —
  screen-space error, a per-level radius, quad angular size — is unknown.
- **Crack handling between adjacent quads at different levels.** Skirts, stitching
  and vertex snapping are all plausible; none is evidenced.
- **Normal generation**, and whether normals are computed from the mesh or
  sampled analytically from the height function.
- **Whether any of it is threaded.** The modifier stack is embarrassingly
  parallel by construction (§7.4 of the parent note), which makes it striking if
  it is not.
- **How the ScaledSpace mesh is baked** from the PQS, and at what level.

§11 says what would fix this.

---

## 10. If you were remaking this — a build order

**[inferred]** Ordered so that each stage is playable and each unlocks the next.
The estimates are of *difficulty*, not hours.

| # | build | why here | difficulty |
|---|---|---|---|
| 1 | **`Orbit` in double precision** — elements, Kepler solve, `getPositionAtUT`, `UpdateFromStateVectors` | Everything else references it. Get this wrong and nothing above it can be right. | Medium. Well-documented classical mechanics; the only trap is the eccentric-anomaly iteration near e→1. |
| 2 | **One gravity source + SOI switching** | Makes orbiting real. | Easy. |
| 3 | **On-rails / off-rails split, and time warp** | The architecture (parent §1). Doing this *now* costs little; retrofitting it costs everything. | Medium. |
| 4 | **A vessel as a tree of rigidbodies with compliant joints** | Now it is KSP and not an orbit toy. | Easy to get working, hard to get *good* — parent §4. |
| 5 | **`FlightCtrlState` and the actuator chain** | Six floats. Do it before any actuator exists so every actuator is written against it. | **Easy, and the highest ratio of value to effort in the list.** |
| 6 | **Engines: thrust at transforms, Isp curve, spool** | §3. Force-at-a-point gives you torque for free. | Easy. |
| 7 | **Reaction wheels, then RCS** | §6. Wheels are three floats; RCS is the per-thruster dispatch. | Easy / medium. |
| 8 | **A rate-damper SAS on the control point** | §2.1. PD on ω, no I. | Easy — and it will immediately show you how floppy your joints are. |
| 9 | **Floating origin + velocity rebase** | Parent §5. Needed once you go fast, not before. | Medium. The velocity half is the part people forget. |
| 10 | **PQS-style quad sphere with a modifier stack** | §9, parent §7. | **Hard.** The subdivision, cracks and collider policy are the work; the modifier stack itself is easy. |
| 11 | **Atmosphere as a physical model** (two curves + ideal gas) | §7. Two curves per body. | Easy. |
| 12 | **Drag cubes + lifting surfaces + control surfaces** | Parent §8, and §4–5 here. | Medium. The **control-surface geometry test (§4.2) is ~20 lines and is the single best-value thing in the flight model.** |
| 13 | **O'Neil scattering shell** | §8, with KSP's own constants in §8.2. | Medium. Analytic, no precompute. |
| 14 | **Manoeuvre nodes and the patched-conic solver** | Parent §2.3, §10.3. | **Hard** — the SOI root find is the hardest single algorithm in the game. But note it is *last*: the game is fully playable without it. |
| 15 | **Navball** | Parent §10. | Easy once §1's frames exist. It is a basis viewer. |

**Two notes on the ordering.** Physics easing (parent §6.5) should go in with
step 3, not later — it is five lines and it will save you days of "why does my
vessel explode on load". And **step 5 before step 6** is the load-bearing one:
if the first engine you write reads the keyboard directly, you will be
retrofitting the interface into every actuator for the rest of the project.

**What to skip.** Body lift, `CapsuleBottom` curves, the buoyancy model, jets'
`atmCurve`, the thermal system and axial temperature curves are all refinements
that can be added at any time and change nothing structural.

---

## 11. What I could not establish

Adding to the parent note's §12, which still stands in full.

- **PQS quad internals** (§9) — vertex count, subdivision metric, crack handling,
  normals, threading, and how ScaledSpace is baked. **This is the largest gap in
  either note.** The fix is a decompile: `PQS`, `PQSQuad` and `PQSCache` are real
  classes in `Assembly-CSharp.dll` and KSP ships Mono.
- **SAS's post-0.90 implementation.** The PD-on-angular-velocity description is
  well-sourced but describes the *classic* behaviour. The overhaul that added the
  nine directional modes plausibly added an attitude loop on top, and its
  structure is unknown. `VesselAutopilot` / `VesselSAS` are the class names to
  look at.
- **The RCS dispatch is reconstructed, not read** (§6.1). The per-thruster
  contribution formula is what the behaviour implies and what the mods that fix
  it assume; no source states it.
- **`ModuleGimbal`'s axis mapping** (§3.5) is inferred by analogy with §4.2 and
  not verified.
- **How drag cubes are baked.** The one clue is that fairings generate theirs at
  runtime with *"a camera tool in Unity"* — strongly implying the offline bake is
  **six orthographic renders measuring silhouette area** — but this is an
  inference from one sentence about the runtime case.
- **`ModuleLiftingSurface`'s lift *direction*.** §5.2 has the magnitudes from a
  checked reconstruction; the exact construction of the lift vector (which plane
  it lies in, how it is built from the surface normal and the airflow) is not
  established, and it is the difference between a wing that works and one that
  does not.
- **Nothing about the actual shader source.** KSP ships compiled shaders. §8 is
  reconstructed from the uniform names and O'Neil's published chapter, which is
  strong for *which algorithm* and says nothing about what Squad changed inside
  it.
- **Still no performance numbers of any kind**, for anything, anywhere.
