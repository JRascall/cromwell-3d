# Nuclear Option — sensors, guidance and AI flight

The companion to [`nuclear_option.md`](nuclear_option.md), which covers the
vehicle physics. This one covers the **combat systems**: how radar detection
actually resolves, why notching works and what it is in code, how missiles fly
and steer, how IR seekers decide a flare is more attractive than an engine, how
guns lead a target, and — the part with the most transferable ideas — **how the
AI flies an aircraft that has a turning circle and has to manage its energy.**

Same source and the same caveats: read from the decompiled `Assembly-CSharp.dll`
of the retail Mono build. **No developer talk, blog or paper exists.** Tags:
**[CODE]** read from the assembly, **[PATCH]** the developer's release notes,
**[inferred]** my reading. Nothing here comes from running the game.

The single organising observation, which is different from the physics note's:

> **Every countermeasure and every evasive manoeuvre in this game attacks a
> specific term in a specific equation, and you can point at both.** Notching
> attacks the doppler multiplier in `RadarParams.GetSignalStrength`; flying low
> attacks the clutter subtraction in the same expression; throttling back to
> defeat an IR missile attacks `IRSource.intensity` in the seeker's
> signal-versus-dazzle comparison; chaff and flares attack the angular
> separation term. **The tactics are not scripted — they are the sensor model,
> read backwards.** §7.4 is where the AI does exactly that, and it is the most
> satisfying thing in the codebase.

**The third note closes this one's gaps.**
[`nuclear_option_command.md`](nuclear_option_command.md) covers the shared world
model every AI reads, the target-scoring library they all share, SAM and ship
batteries, gun mounts in full, the remaining seekers, the helicopter AI, ground
vehicles, ships and the strategic layer.

Related: [`nuclear_option.md`](nuclear_option.md),
[`nuclear_option_command.md`](nuclear_option_command.md),
[`nuclear_option_control.md`](nuclear_option_control.md) (the player input path
and the AI's flight procedures),
[`nuclear_option_audio.md`](nuclear_option_audio.md) (sound travel delay and the
Mach cone),
[`spatial_queries.md`](spatial_queries.md) (§1.3's batched raycasts and the
cheap-cull-before-expensive-test rule), [`ai_state_machines.md`](ai_state_machines.md)
(§7 is a shipped instance of the patterns there),
[`valve_networking.md`](valve_networking.md).

---

## 1. Radar detection

### 1.1 The pipeline

**[CODE]** `Radar : TargetDetector`. Detection is a four-stage funnel, and each
stage is deliberately cheaper than the next.

```
RepeatSearch()          UniTask loop, period = checkInterval, or alertCheckInterval
   ↓                    if anything was detected last pass. Start jittered by a
   ↓                    random fraction of the period so radars do not sync up.
RadarCheck()            for each enemy faction, for each unit in its
   ↓                    factionRadarReturn list: range reject at 2× maxRange,
   ↓                    then a cone reject if radarCone > 0
RequestRadarCheck()     radar HORIZON test (§1.2) and CLUTTER computation (§1.4).
   ↓                    Survivors are queued as a DetectionRequest.
RaycastCommand batch    line of sight, batched, Burst, batch size 16
   ↓
ProcessResult()         → CanSeeRadarReturn → GetSignalStrength ≥ minSignal
                        → DetectTarget → RpcUpdateTrackingInfo to the faction
```

**[inferred] Three structural things worth copying.**

- **The candidate list is maintained, not searched for.** `RadarCheck` iterates
  `FactionHQ.factionRadarReturn` — a per-faction roster of units that have a
  radar cross-section — rather than doing a spatial query. For a few hundred
  units that is cheaper than any tree, and it is the same argument CLAUDE.md
  makes for the occupancy index: **the roster already exists, so reading it beats
  rebuilding a structure over it.**
- **The scan period doubles as an alert state.** `alertCheckInterval` is used
  instead of `checkInterval` whenever the last sweep found anything, so a radar
  that is tracking something updates faster than one that is searching. One
  field, and it removes the need for a separate "alerted" state.
- **The initial delay is randomised** (`await UniTask.Delay(random * checkInterval)`).
  Without it, every radar spawned in the same frame scans on the same frame
  forever, and the frame cost spikes periodically. This is a two-line fix for a
  problem that is otherwise diagnosed as a mysterious periodic hitch.

Line of sight is `RaycastCommand.ScheduleBatch(commands, results, 16, 1)` — every
pending detection in the whole game becomes one batched, Burst-scheduled raycast
job per fixed step. **[PATCH]** 0.32: *"Implemented CPU multithreading for
line-of-sight calculations."* The ray is aimed at
`target.position + 0.4 * definition.height * up` (40% up the target's box, not
its origin) and a hit only blocks if it lands **more than `target.maxRadius * 1.5`
away from the target** — so clipping the target's own collider does not count as
an occlusion.

### 1.2 The radar horizon is a closed-form geometric test

```csharp
public const float EARTH_RADIUS = 6371000f;
float groundDist    = |targetPos - radarPos| with y zeroed;
float radarHorizon  = Mathf.Sqrt(12742000f * radarPos.y);      // sqrt(2·R·h)
float targetHorizon = Mathf.Sqrt(12742000f * targetPos.y);
if (radarHorizon + targetHorizon < groundDist) return;         // below the horizon: never queued
```

`sqrt(2Rh)` is the standard horizon-distance approximation, and the test is the
standard "can two objects see each other over a sphere" one: the sum of their
horizon distances must exceed the ground distance between them. **[PATCH]** 0.30:
*"Radar horizon implemented: aircraft below the horizon will not be detected."*

**[inferred]** Note where it sits — in `RequestRadarCheck`, **before** the
raycast is queued. It is a two-square-root rejection that removes a raycast, and
it is the cheap-filter-before-expensive-test rule from
[`spatial_queries.md`](spatial_queries.md) §3.6 applied exactly. It is also the
only place in the game that admits the Earth is round.

### 1.3 The signal equation, in full

**[CODE]** `RadarParams.GetSignalStrength` is the whole detection model and it is
17 lines:

```csharp
public float GetSignalStrength(Vector3 direction, float dist, Rigidbody rb,
                               float RCS, float clutter, float ecm) {
    float signal = maxRange / dist * Mathf.Pow(RCS, 0.25f);       // range and cross-section
    signal = Mathf.Min(signal, maxSignal);                        // saturation
    signal -= clutter * clutterFactor;                            // ground return
    if (signal > minSignal) {
        float boosted = Mathf.Lerp(signal, maxSignal, 0.5f);      // halfway to saturation
        if (rb != null) {
            float doppler = Mathf.Min(Mathf.Abs(Vector3.Dot(direction, rb.velocity)), 150f)
                          * dopplerFactor;
            boosted *= 1f + doppler;                              // ← THE DOPPLER TERM
        }
        return boosted - ecm;
    }
    return signal;                                                // ← no doppler boost down here
}
```

Five parameters per radar (`maxRange`, `maxSignal`, `minSignal`, `clutterFactor`,
`dopplerFactor`) and six inputs. Reading it term by term:

| Term | What it models |
|---|---|
| `maxRange / dist` | inverse-range falloff, normalised so signal = 1 at max range |
| `RCS^0.25` | cross-section, **heavily compressed** — 16× the RCS is only 2× the signal |
| `min(…, maxSignal)` | receiver saturation; a close target cannot return unbounded signal |
| `− clutter · clutterFactor` | ground return, **subtracted**, so it is an absolute floor not a ratio |
| `lerp(signal, maxSignal, 0.5)` | processing gain once above the detection floor |
| `× (1 + doppler)` | **closure rate along the line of sight**, capped at 150 m/s |
| `− ecm` | jamming, also subtractive |

**[inferred] `RCS^0.25` is the most interesting authored choice.** A real
monostatic radar equation goes as `RCS / R⁴`; this goes as `RCS^0.25 / R`. Both
of those are deliberate flattenings. The fourth root means stealth is *valuable
but not decisive* — a 16:1 RCS advantage buys 2:1 in signal, i.e. roughly 2:1 in
detection range, which is a big tactical edge and not an invisibility cloak. The
1/R instead of 1/R⁴ means detection range is not violently sensitive to the range
parameter, so `maxRange` means what a designer expects it to mean. **Neither is
physically right, and both make the design space controllable.** That is the
trade to notice: the equation was chosen for the *shape of its response to
authored parameters*, not for fidelity.

### 1.4 Clutter — why flying low hides you

**[CODE]** Clutter is computed per detection request, in `DetectorManager`, and
it is two independent terms:

```csharp
float clutter = 0f;

// (1) look-down geometry: only when the target is below the radar and inside its horizon
if (groundDist < radarHorizon && targetPos.y < radarPos.y * (1f - groundDist / radarHorizon)) {
    float grazing = slantDist * target.radarAlt / (radarPos.y - targetPos.y);
    clutter += Mathf.Min(slantDist, 1000f) / grazing;
}

// (2) proximity to the ground relative to the target's own size
clutter += target.maxRadius * target.maxRadius * 2f / (target.radarAlt * target.radarAlt);
```

**[inferred] Term (2) is the one that matters and it is beautifully simple.**
It is the ratio of the target's own size to its height above the ground, squared.
An aircraft with a 10 m radius at 500 m AGL contributes `200/250000 ≈ 0.0008` —
nothing. The same aircraft at 20 m AGL contributes `200/400 = 0.5`, which against
a `maxSignal` of order 1 is decisive. **The clutter penalty is inverse-square in
radar altitude**, so it does almost nothing until you are genuinely in the weeds
and then it dominates.

Term (1) adds a look-down penalty that grows as the geometry flattens — a radar
looking down a shallow slant angle at a low target sees more ground behind it.

Two details worth noting. First, **clutter is subtracted before the `minSignal`
comparison**, so it can push a target below the detection floor entirely — and
once below the floor, the doppler branch never runs. Second, `radarAlt` is the
target's *radar altitude* (height above terrain, from `Unit.CheckRadarAlt`'s
downward linecast), not its altitude above sea level, so flying low over a
mountain works and flying at the same absolute altitude over a valley does not.

### 1.5 Notching, exactly

The doppler term again:

```csharp
float doppler = Mathf.Min(Mathf.Abs(Vector3.Dot(direction, rb.velocity)), 150f) * dopplerFactor;
boosted *= 1f + doppler;
```

`direction` is the unit vector from radar to target; `rb.velocity` is the target's
world velocity. The dot product is **the target's speed component along the line
of sight** — its closure (or opening) rate. Capped at 150 m/s, multiplied by
`dopplerFactor`, and applied as a *multiplier* on the processed signal.

**So: fly perpendicular to the radar and the dot product goes to zero, the
multiplier goes to 1, and you lose the entire doppler processing gain.** That is
the notch, and it is one dot product.

**[inferred] What this model gets right, and what it deliberately does not.**

Right: the *direction* of the effect, the fact that it depends on your velocity
vector relative to the radar's line of sight rather than on your heading, the
fact that it works identically against an aircraft radar and a missile seeker
(both call the same function), and — crucially — the fact that it **stacks with
clutter**. The real technique is to beam the radar *and* get low; here those are
two independent terms in the same expression, one subtractive and one
multiplicative, so doing both is much stronger than doing either. A player who
discovers that has discovered something true.

Not right, and knowingly: there is no notch *width*. A real pulse-doppler radar
has a rejection filter of a few tens of m/s around zero closure, so you are
either in the notch or out of it, sharply. Here the response is linear and
continuous in closure rate — 30° off perpendicular gives you half the boost. That
is smoother and much more forgiving to fly, and it means the skill expression is
"point roughly abeam" rather than "hold ±3° or die". **[inferred]** For a game
that describes itself as accessible, that is the right call, and it is worth
naming as a deliberate simplification rather than an oversight, because the
capped `Min(…, 150f)` shows someone thought about the shape of this curve.

One more consequence worth flagging: **there is no doppler penalty for being
notched, only the absence of a bonus.** `boosted *= 1 + doppler` with doppler = 0
leaves `lerp(signal, maxSignal, 0.5)`, which is still stronger than the raw
signal. So notching alone never makes you invisible — it reduces your effective
detection range by a factor of `(1 + doppler)`. To actually disappear you need
the clutter term as well, which is why the AI's radar evasion (§7.4) does both.

### 1.6 What the player is told

**[CODE]** `Aircraft.EstimateDetection` is the HUD's "am I being seen" estimate,
and it is a *different, simpler* function from the real one:

```csharp
float clutter = maxRadius * maxRadius * 2f / (radarAlt * radarAlt);
returnSignal  = Mathf.Min(maxRange / dist * Mathf.Pow(RCS, 0.25f), maxSignal);
returnSignal -= clutter * 0.15f;                    // fixed 0.15, not the radar's clutterFactor
returnSignal  = Mathf.Max(returnSignal, 0f);
return returnSignal > minSignal;
```

**[inferred]** No doppler term, no look-down term, a hardcoded clutter factor.
The estimate deliberately does **not** model the notch. So the display tells you
roughly whether you are inside the envelope, and whether your specific geometry
defeats the radar is left as something the player has to understand rather than
read off a gauge. That is a design decision about where the skill lives, made by
writing a second, worse copy of the equation — which is a maintenance liability
(two formulas that must stay roughly in step) accepted on purpose.

### 1.7 Jamming and the rest of the EW model

**[CODE]**

- **`Radar.jamAccumulation`** rises on each `onJam` event by
  `jamAmount / max(jamTolerance, 0.1)` and decays at
  `−max(accumulation, 0.2) · dt` per second — i.e. an accumulator with a floor on
  its decay rate, so it always drains. `IsJammed()` is `accumulation > jamTolerance`,
  and a jammed radar returns false from `CanSeeRadarReturn` regardless of signal.
- **`ecmIntensity`** is a plain subtractive term in the signal equation,
  accumulated on the target by `Aircraft.AddECMIntensity` from every jammer
  covering it.
- **Radar warning** is *event-driven, not scan-driven*: `GetRadarReturn` calls
  `RpcGetRadarWarning(emitter)` on the target whenever it is illuminated, so the
  RWR is exactly as accurate as the radar. The target knows it is being looked at
  even if the return is too weak to detect it — which is right, since the RWR is
  a receiver and has a much easier job than the radar.
- **RCS is dynamic.** `Unit.RCS` starts at `definition.radarSize` and
  `ModifyRCS` is called when stores are expended: **[PATCH]** 0.30.9, *"Aircraft
  RCS now decreases when external weapons are expended"* — and drag decreases with
  it, via the same `dragArea` field from the physics note's §3.1. One weapon
  release changes both your signature and your performance.

---

## 2. How a missile flies

**[CODE]** `Missile : Unit`. A missile is a rigidbody with an aerodynamic model,
an autopilot, one or more motors, and a seeker. Those four are cleanly separated,
and the separation is the design.

### 2.1 Airframe

`ApplyAero` is the missile's whole flight model, and it is *not* the per-part
model the aircraft uses — it is a single lifting body:

```csharp
Vector3 v     = rb.velocity - wind;
float   alpha = Mathf.Deg2Rad * Vector3.Angle(transform.forward, v);      // TOTAL angle of attack
Vector3 liftDir = Vector3.Cross(Vector3.Cross(transform.forward, v), v).normalized;

float Cl   = liftCurve.Evaluate(alpha);
float drag = dragCurve.Evaluate(alpha) * ρ * v² * 0.5f * currentFinArea;
float lift = Cl                        * ρ * v² * 0.5f * currentFinArea;

Vector3 dragForce = -v.normalized * drag;
// same cubic transonic bump as the aircraft, then a permanent supersonic penalty:
if (speed > 1.1 * a)      dragForce *= 1f + supersonicDrag;
else if (speed > 0.9 * a) dragForce *= 1f + t³ * (supersonicDrag + 0.15f);

rb.AddForce(liftDir * lift + dragForce);
```

**[inferred] Three things.** The angle of attack is the *total* incidence angle,
not resolved into pitch and yaw — so the missile is treated as axisymmetric, which
is true of most missiles and saves half the work. The lift direction is
`Cross(Cross(fwd, v), v).normalized`, the component of the body axis perpendicular
to velocity, which is the correct lift direction for a body at incidence. And
**`currentFinArea` is a variable**, because fins deploy after launch
(`DeployFins`, `guidanceDelay`) — before that the missile is a ballistic tube with
almost no lift or drag, which is what gives the ejection phase its correct
character.

`dragCurve` rising with alpha is the whole of **[PATCH]** 0.30.9's *"Increased
maneuvering drag for most missiles, allowing kinetic evasion tactics"* — a
missile that pulls hard bleeds energy, so forcing it to manoeuvre is a defence.
That is a gameplay mechanic that exists entirely because drag is a function of
incidence rather than a constant.

### 2.2 The turn rate limiter is the same `ω = g/V` as the aircraft

```csharp
Vector3 torque = inputs * torqueGain;
if (maxTurnRate > 0f || gLimit > 0f) {
    float limit = Mathf.Min(maxTurnRate * Mathf.Deg2Rad, 9.81f * gLimit / Mathf.Max(speed, 1f));
    Vector3 predicted = localAngularVel + torque * dt;              // where this torque would put us
    float excessX = Mathf.Max(Mathf.Abs(predicted.x) - limit, 0f);
    float excessY = Mathf.Max(Mathf.Abs(predicted.y) - limit, 0f);
    torque -= new Vector3(Mathf.Sign(torque.x) * excessX / dt,
                          Mathf.Sign(torque.y) * excessY / dt, 0f);
}
rb.AddRelativeTorque(torque, ForceMode.Acceleration);
```

**[inferred]** Two limits, whichever binds first: an absolute body rate
(`maxTurnRate`, the fin authority) and a g limit expressed as a rate
(`9.81·g/V`, the structural/aerodynamic limit). Identical in form to the
aircraft's fly-by-wire pitch law. And the limiter is **predictive** — it computes
where this tick's torque would put the angular velocity and removes exactly the
excess, rather than clamping the rate after the fact. That means no overshoot and
no ringing at the limit.

The consequence players feel: `9.81 · g / V` means a **fast missile turns
worse**. A 30 g missile at 900 m/s can only pull 0.33 rad/s ≈ 19°/s; the same
missile at 300 m/s can pull 57°/s but has no energy left to reach you. That
tension — fast and unmanoeuvrable versus slow and agile — is not authored
anywhere. It is one division.

`ForceMode.Acceleration` is deliberate: the missile's mass drops as the motor
burns (`rb.mass -= burnRate * dt`), and acceleration mode makes control authority
independent of that.

### 2.3 The motor

```csharp
fuelMass    -= burnRate * dt;
rb.mass     -= burnRate * dt;                          // the missile gets lighter
if (localSim && missile.speed < topSpeed)
    rb.AddForce(thrust * throttle * transform.forward);
if (thrustVectoring > 0f)
    exhaustParticles.localEulerAngles = new Vector3(inputs.x * thrustVectoring,
                                                    180f - inputs.y * thrustVectoring, 0f);
```

Multiple `Motor`s per missile with individual `delayTimer`s gives boost-sustain
staging for free. `GetRemainingDeltaV` computes an actual rocket-equation-flavoured
Δv (`thrust · burnTimeRemaining / meanMass`), which the AI and the HUD use to
answer "can this missile still reach". `TerminalBoost` is a separate component
that re-lights a motor in the endgame.

**[inferred]** The `speed < topSpeed` guard is a soft speed cap that is *not* drag
— above `topSpeed` thrust simply stops. Cheap, and it stops a long-burn motor
from producing absurd speeds when the drag curve is optimistic.

### 2.4 The autopilot — a PID, and it is **not** proportional navigation

This is the answer to "is it PID controlled", and the answer has an important
qualifier.

```csharp
private void Steering() {
    Vector3 losDir = NormalizedDirection(missilePos, aimPoint);
    Vector3 refAxis = reachedOnTarget ? (forward * 0.5f + velocity.normalized * 0.5f) : forward;

    if (!reachedOnTarget && timeSinceSpawn > 2f && Vector3.Angle(losDir, refAxis) < 10f)
        reachedOnTarget = true;                                  // switch reference frame once settled

    if (Vector3.Dot(losDir, refAxis) < 0.71f)                    // >45° off
        losDir = Vector3.RotateTowards(refAxis, losDir, Mathf.PI / 4f, 1f);   // clamp the demand

    Vector3 err = Matrix4x4.TRS(zero, Quaternion.LookRotation(refAxis, transform.up), one)
                           .inverse.MultiplyVector(losDir) * 30f;
    err.z = 0f;

    if (uprightPreference > 0f) { /* roll the missile upright, biased into the turn */ }

    Vector2 out = pid.GetOutput(new Vector2(-err.y, err.x), dt);
    inputs = new Vector3(Clamp(out.x, -1, 1), Clamp(out.y, -1, 1),
                         Clamp(-localAngularVel.z * 5f + err.z * 0.3f, -1, 1));
}
```

So the autopilot is a **2-axis PID on the angular error between the missile's
reference axis and the aimpoint**, plus a roll channel that is a rate damper
(`−ω_z · 5`) with a proportional term.

**[inferred] Three design decisions here, and all three are worth stealing.**

1. **The reference axis switches from the nose to a nose/velocity blend once the
   missile has settled.** Immediately after launch the missile is not flying
   along its nose — it has been ejected sideways from a rail and has to point
   itself first, so the error is measured against the *nose*. Once it has been
   within 10° of the aimpoint for a moment (`reachedOnTarget`), the reference
   becomes `0.5·forward + 0.5·velocity`, which is the right frame for steering a
   lifting body, because what you actually control is the velocity vector via
   incidence. Two regimes, one flag, no blending logic.
2. **The demand is clamped to 45° from the current reference.** Commanding a
   180° turn produces an error the PID cannot act on sensibly and an integrator
   that winds up. `RotateTowards(refAxis, losDir, π/4, 1)` says "steer 45° toward
   it and re-evaluate next tick", which converges cleanly. Combined with the rate
   limiter in §2.2, the missile never asks for something it cannot do.
3. **The error is scaled by 30 and then PID'd.** The `× 30` puts the error in
   units where the authored PID gains are conveniently sized. Cosmetic, but it is
   why the gain values in the ScriptableObject are readable numbers.

**Now the qualifier, which is the important part.** A PID on *pointing error*
alone is pure pursuit, and pure pursuit against a crossing target produces a
tail chase and a miss. This missile does not fly pure pursuit, because
**the `aimPoint` it is steering at is not the target — it is a predicted
intercept point computed by the seeker** (§3.1). The lead is in the *guidance*,
the PID is only the *autopilot*.

**[inferred] That separation is the architecture, and it is cleaner than
proportional navigation for a game.** PN computes a commanded lateral
acceleration from the line-of-sight rate (`a = N · V_c · λ̇`) and is optimal
against a non-manoeuvring target, but it couples the seeker and the autopilot
into one law, it needs a clean LOS-rate estimate (noisy in a game where target
velocity is a networked value), and it is hard to reason about when you want a
missile to *also* loft, jink, or fly a datalink midcourse. Splitting it into
"the seeker says where to point, the autopilot points there" means:

- every seeker type (§3) shares one autopilot;
- loft, terminal jink and datalink midcourse are all just *modifications to the
  aimpoint*, added in the seeker with no autopilot changes;
- the same `SetAimpoint(pos, vel)` interface serves a radar missile, an IR
  missile, a laser-guided bomb and a cruise missile.

The cost is that the missile is not flying a true proportional-navigation
trajectory, so it does not achieve PN's constant-bearing intercept and will
generally pull harder late than a real missile would. Given the drag-vs-incidence
model in §2.1, that cost is real and it is visible as terminal energy loss —
which is, conveniently, also a gameplay feature.

### 2.5 Self-destruct conditions

```csharp
if ((timeSinceSpawn > 10f || (!EngineOn() && timeSinceSpawn > 2f))
 && (missile.LosingGround() || missile.MissedTarget() || missile.speed < selfDestructAtSpeed
     || targetUnit == null))
    missile.Detonate(...);

public bool LosingGround() => Vector3.Dot(rb.velocity, rb.velocity - targetVel) < 0f;
public bool MissedTarget() => Vector3.Dot(aimPoint - GlobalPosition(), rb.velocity) < 0f;
```

**[inferred]** `MissedTarget` is the classic "the target is now behind me" test.
`LosingGround` is subtler and rather good: the dot of your velocity with your
*closure* velocity — if you are no longer gaining on the target in the direction
you are travelling, you have lost the race. Both are one dot product, run on a 1 Hz
slow update, and between them they clean up every missile that is not going to
hit without needing a lifetime timer.

---

## 3. Seekers

**[CODE]** `MissileSeeker` is a four-method base class — `Seek()`, `Initialize()`,
`GetEvasionPoint()`, `GetSeekerThreat()` — and there are eight implementations.
All of them do the same job: **maintain `knownPos`, `knownVel`, `knownAccel` with
appropriate errors and failure modes, compute a lead point, and call
`missile.SetAimpoint`.**

### 3.1 The lead solver everything shares

```csharp
public static Vector3 GetLeadVectorWithAccel(GlobalPosition targetPos, GlobalPosition platformPos,
        Vector3 targetVel, Vector3 platformVel, Vector3 targetAccel, float maxLead) {
    targetAccel = Vector3.ClampMagnitude(targetAccel, 100f) + 9.81f * Vector3.up;
    float closing = Vector3.Dot((targetPos - platformPos).normalized, platformVel - targetVel);
    float t = Mathf.Clamp(Distance(platformPos, targetPos) / Mathf.Max(closing, 10f), 0f, maxLead);
    return t * targetVel + Mathf.Min(t * t, 1f) * 0.5f * targetAccel;
}
```

A first-order time-to-intercept (`range / closing rate`, floored at 10 m/s so it
cannot divide by zero or explode on a receding target), clamped to `maxLead`
seconds, then a constant-velocity lead plus a **half-a-t-squared acceleration
correction**.

**[inferred] The `Mathf.Min(t*t, 1f)` on the acceleration term is the detail.**
It caps the acceleration contribution at `0.5 · a` regardless of how long the
flight time is. Target acceleration is estimated by finite differences of a
networked velocity, so it is noisy; extrapolating a noisy acceleration over a
10-second flight would produce a wildly wrong aimpoint. Capping the term
deliberately *under-leads* on acceleration, which costs a little terminal miss
distance and buys a lot of stability. Gravity is added to the target's
acceleration unconditionally, which is why the solver leads correctly on ballistic
targets.

Every seeker calls this. What differs between them is **how `knownPos`,
`knownVel` and `knownAccel` are obtained, and how they degrade.**

### 3.2 ARH — active radar homing, with a real midcourse

`ARHSeeker` is the most complete, and it has four states:

```
passive → (guidanceDelay, fins deploy) → DatalinkMode → radarLockEstablished → TerminalMode
                                            ↑                                      ↓
                                            └──── lock lost > lockPerseverance ────┘
```

**Datalink midcourse.** Before its own radar acquires, the missile flies on the
launching faction's tracking database:

```csharp
positionalErrorVector = Random.insideUnitSphere * datalinkPositionalError;   // fixed at launch
...
if (HQ.IsTargetPositionAccurate(targetUnit, 2000f))
    knownPos = HQ.GetKnownPosition(targetUnit).Value + positionalErrorVector;
if (HQ.IsTargetBeingTracked(targetUnit))
    knownVel = targetUnit.rb.velocity;
knownPos += knownVel * dt;                                     // dead reckon between updates
if (Vector3.Angle(forward, Direction(missilePos, knownPos)) > maxDatalinkAngle)
    knownPos = missilePos + forward * 10000f;                  // demand outside the datalink cone: fly on
if (!InRange(knownPos, targetUnit.GlobalPosition(), 2000f))
    { missile.SetTarget(null); targetUnit = null; }             // the track has drifted too far: give up
```

**[inferred] `positionalErrorVector` is generated once at launch and never
changes.** That is a per-missile *bias*, not per-frame noise. It matters: noise
averages out over a flight and a controller ignores it; a fixed bias means this
particular missile is aimed slightly wrong for its whole midcourse and has to
correct when its own seeker acquires. Every seeker in this game does the same
thing (`IRSeeker.errorOffset = Random.insideUnitSphere`), and it is the right way
to model sensor error for something that flies for thirty seconds.

**Terminal.** The seeker runs its own radar — the *same* `RadarParams` struct, the
*same* horizon test, the *same* clutter formula, the *same* `GetSignalStrength` —
so **notching a missile and notching an aircraft are literally the same code
path**. Additional gates the aircraft radar does not have:

- `maxTrackingAngle` — a gimbal limit; a target outside the seeker's cone is not
  seen, and if the demand goes outside it the missile flies straight ahead.
- `minReacquireRange` — once the return has dropped below `minSignal`, it will not
  re-acquire inside 2 km. So breaking lock late is permanent.
- `homingLockDelay` — the seeker must hold a return for a continuous period before
  it trusts it enough to slew `knownPos` onto the true position.
- `lockPerseverance` (default 2 s) — memory. `timeWithoutReturn` accumulates while
  the return is lost and the missile coasts on `knownPos += knownVel · dt`; past
  the threshold it drops the target.
- `returnStrength == -1f` is a distinguished value meaning *hard* loss (below the
  horizon, or line of sight blocked), which drops the target immediately rather
  than starting the perseverance timer.

**Loft:**

```csharp
float loft = Mathf.Min(timeToTarget * timeToTarget * 4.905f * loftAmount, targetDist * loftAmount);
aimPoint += loft * Vector3.up;
timeToTarget -= dt;
```

`½gt²` scaled by `loftAmount`, capped at a fraction of the range. The missile aims
above the target by exactly the amount it will fall on the way — so it flies a
lofted trajectory into thin air where drag is lower and arrives with more energy,
and the loft naturally decays to zero as `timeToTarget` runs down.

**Terminal jink.** The best find in the file:

```csharp
if (jinkEvasion.amount > 0f && multipleInbound && targetDist > terminalRange)
    aimPoint += rampIn * jinkEvasion.ApplyJink(missilePos, knownPos, missile.speed, targetDist);
```

where `multipleInbound` is set from the faction tracking data
(`trackingData.missileAttacks > 1`). **[inferred] When more than one missile is
inbound on the same target, the missiles weave** — so a salvo does not fly a
single line that one manoeuvre or one countermeasure defeats. The jink is faded in
over `timeSinceSpawn` 3→10 s and switched off inside terminal range. This is a
*missile* using an evasion routine, and it is the kind of detail that only exists
because someone played the game and noticed salvos were too easy to beat.

**Home-on-jam.** If accumulated jamming exceeds `jamTolerance` and `homeOnJam` is
set, the missile retargets the *jammer*. Which means switching on a powerful
jammer is not free.

### 3.3 IR — aspect, sun, and how the flare decision is made

`IRSeeker` tracks an `IRSource` — a transform with an `intensity`, registered on
the target by its engines (`TurbineEngine` sets
`intensity = Lerp(IRMin, IRMax, currentPower / maxPower)`) and by flares.

**Lock is lost when a flare wins the following comparison:**

```csharp
private void IRSeeker_OnTargetFlare(IRSource flare) {
    float rangeCoef  = rangeFactor.Evaluate(dist / maxRange);           // authored curve
    Vector3 losToTgt = NormalizedDirection(seekerPos, target.position);
    Vector3 flareSep = NormalizedDirection(flare.position, target.position);
    float   angSep   = Clamp01(1f - Mathf.Abs(Vector3.Dot(losToTgt, flareSep)));   // 0 = co-located
    float   aspect   = AspectCoef(losToTgt);
    float   sunGlare = Clamp01(BackgroundBrightness(losToTgt)) * 2f;

    float signal = IRTarget.intensity * (1f + aspect) / (rangeCoef + sunGlare);
    dazzleAmount += (1f + angSep) / flareRejection;

    if (dazzleAmount > signal) { LoseLock(); IRTarget = flare; }        // ← the seeker switches to the flare
}

private float AspectCoef(Vector3 los) {
    float head = Clamp01(Vector3.Dot(-target.forward, los));            // looking at the nose
    float tail = Clamp01(Vector3.Dot( target.forward, los));            // looking up the tailpipe
    return head * 0.5f + tail * 2f;                                     // ← 4:1 rear-aspect advantage
}

private float BackgroundBrightness(Vector3 los) {
    float cloud = LevelInfo.GetCloudOcclusion(seekerPos);
    return Clamp01(Vector3.Dot(los, -sun.transform.forward)) * (1f - cloud) * sun.color.b;
}
```

**[inferred] Every real IR countermeasure technique is a term in that
comparison.** Read it as a list of what a pilot can do:

| Do this | Term it attacks |
|---|---|
| Throttle back / shut down an engine | `IRTarget.intensity` — directly |
| Turn nose-on to the missile | `aspect` — 2.0 becomes 0.5, a **4× signal drop** |
| Break turn so the flare separates | `angSep` — dazzle rises toward 2/`flareRejection` |
| Fly toward the sun | `sunGlare` — divides the signal |
| Fly in cloud | `cloud` — but this *removes* sun glare, so it cuts both ways |
| Dive into terrain | line-of-sight linecast in `IRLockCheck` — hard loss |

And the `sun.color.b` factor is a lovely touch: the sun's blue channel is high at
midday and low at sunset, so **the sun blinds an IR seeker at noon and not at
dusk**, entirely as a side effect of the time-of-day lighting system already
computing a sun colour.

**Flares themselves are ballistic objects** (`IRFlare`) — position integrated with
quadratic drag and gravity, a burn timer, a linecast for ground impact. High drag
means they decelerate away from the aircraft quickly, which is what drives
`angSep` up over the first second after release. So **flare effectiveness as a
function of time since release is emergent from flare drag**, not a curve.

When the seeker has no lock it does not simply fly straight — it accumulates a
random walk:

```csharp
driftError += Random.insideUnitSphere * (driftRate * Time.deltaTime / 2f);
```

so a spoofed IR missile wanders off progressively rather than continuing on its
last bearing.

### 3.4 The others

- **`SARHSeeker`** — semi-active. Depends on the *launching* radar continuing to
  illuminate; the classic "support the shot" constraint, and the reason the
  launching aircraft cannot immediately turn away.
- **`ARMSeeker`** — anti-radiation. Homes on emitters; goes ballistic to the last
  known position when the radar shuts down.
- **`OpticalSeeker`**, `OpticalSeekerBomb`, `OpticalSeekerHighDrag`,
  `OpticalSeekerShell`, `OpticalSeekerCruiseMissile` — camera/contrast seekers
  with different flight profiles for different munitions.
- **`LaserSeeker`** — tracks a `LaserDesignator` spot.
- **`InertialSeekerShell`** and `BallisticMissileGuidance` — no seeker; fly a
  computed trajectory to a coordinate.

**[inferred]** Eight seekers, one autopilot, one lead solver, one
`SetAimpoint(pos, vel)` interface. That ratio is the point of §2.4's separation,
and it is the number I would quote to justify the design.

---

## 4. Countermeasures

**[CODE]** `CountermeasureManager` holds `countermeasureStations`, each declaring
a list of `threatTypes` (the strings `"IR"`, `"ARH"`, `"SARH"`, … returned by
`MissileSeeker.GetSeekerType()`):

```csharp
public string ChooseCountermeasure(Missile threat) {
    string seekerType = threat.GetSeekerType();
    activeIndex = byte.MaxValue;
    for (byte b = 0; b < countermeasureStations.Count; b++)
        if (countermeasureStations[b].threatTypes.Contains(seekerType)) { activeIndex = b; break; }
    ...
}
```

**[inferred]** Dispatch by string is not what I would write, but the shape is
right: **the countermeasure is selected by the threat's declared seeker type, and
the mapping is authored data on the aircraft rather than code.** An aircraft with
a mixed dispenser, or a new seeker type, needs no code change. The string is the
weak point — a typo is a silent failure — and an enum would cost nothing.

Chaff (`RadarChaff`) and flares (`IRFlare`) share their structure: a ballistic
object with quadratic drag, a lifetime, and registration as a decoy source on the
aircraft that ejected it (`onAddRadarChaff` / `onAddIRSource`) so the seeker's
event handler fires. Chaff's effect on an ARH seeker is the same
angular-separation model as flares:

```csharp
float rangeCoef = Clamp01(1f - targetDistance / radarParameters.maxRange);
float angSep    = Clamp01(1f - Mathf.Abs(Vector3.Dot(losToTarget, chaffToTarget)));
jamAccumulation += rangeCoef * angSep / (1f + jamTolerance);
```

**[inferred] Note `rangeCoef`: chaff is most effective when the missile is *far
away*** (`1 - d/maxRange` → 1 at long range, 0 at the merge). That is correct —
at long range the resolution cell is huge and the chaff cloud sits inside it; up
close the seeker can resolve the two. It also gives the right gameplay: chaff
defeats a long shot and does nothing to a missile that is already on top of you.

---

## 5. Guns and ballistics

### 5.1 Bullets are simulated, not raycast

**[CODE]** `BulletSim` owns a pool of `Bullet` structs, stepped every frame:

```csharp
velocity.y -= 9.81f * dt * info.gravMult;
velocity   -= velocity.magnitude * velocity * (info.dragCoef * dt / info.muzzleVelocity);
GlobalPosition next = position + velocity * dt;
if (Physics.Linecast(prev, prev + velocity * 1.5f * dt, out hit, ~ExclusionZonesMask)) { ... }
```

Quadratic drag normalised by muzzle velocity, gravity with a per-weapon
multiplier, and a **linecast over 1.5× the frame's travel** for collision — the
1.5 factor being overlap insurance against a variable frame time. Damage scales
with the *square* of the current speed over the muzzle speed:

```csharp
float pierce = info.pierceDamage * velocity.sqrMagnitude / (info.muzzleVelocity * info.muzzleVelocity);
```

i.e. **damage is proportional to kinetic energy**, so range degrades penetration
automatically. Each bullet also carries a `reliability = Random.value` rolled at
creation — a per-round random used for dud/penetration checks, which means the
outcome is decided at fire time rather than at impact time and is therefore
consistent for that round.

Tracers are a separate `TracerView` with a **static material cache keyed by
colour**, so ten thousand tracers share a handful of materials.

### 5.2 The lead solver — two of them

**[CODE]** There are two distinct gun lead calculations, for two different jobs.

**The cheap one**, `TargetCalc.TargetLeadTime`, is a fixed-point iteration on the
closed-form solution for exponential drag:

```csharp
for (int i = 0; i < iterations; i++) {                     // AI uses 2 iterations
    float d = Vector3.Distance(target.position + targetVel * t, gun.position);
    t = (Mathf.Pow(2.71828f, dragCoef * d / v0) - 1f) / dragCoef;
    if (!float.IsFinite(t) || t > 120f) return 120f;
}
```

`t = (e^{kd/v} − 1)/k` is the exact time of flight for a projectile with drag
proportional to velocity, and iterating it twice with the target's predicted
position converges fast. Muzzle velocity includes the platform's own velocity
component along the barrel.

**The expensive one**, `Kinematics.TrajectorySim`, is a full forward integration
used for the player's CCIP/aim assist, run **at most every 0.1 s** and
`SmoothDamp`ed in between:

```csharp
float timeStep = Mathf.Lerp(0.02f, 0.1f, targetDist * 0.0003f);   // coarser steps for longer shots
while (!done) {
    timeStep += 0.02f;                                            // and progressively coarser
    ...
    accel = 9.81f * gravMult * up + v² * dragCoef / muzzleVelocity * v̂;
    v -= accel * 0.3f;  pos += v * timeStep;  v -= accel * 0.7f;  // velocity Verlet-ish split
    if (Dot(v, targetPos - projPos) <= 0f) { /* interpolate back to closest approach */ }
}
missVector = Vector3.ProjectOnPlane(projPos - targetPos, v);
```

and the caller then *corrects the aimpoint by the miss vector* and re-simulates —
a shooting-method solve rather than an analytic one.

**[inferred] Three things are right about this pair.** The AI gets the cheap
solver because it fires from a cone and does not need millimetre accuracy. The
player gets the expensive one because a CCIP pipper that is visibly wrong is
unacceptable — but it runs at 10 Hz and is smoothed, so the cost is bounded. And
the step size **grows with range and grows again each iteration**, so a 4 km shot
does not cost 200× a 20 m shot. That last trick — an integrator whose step
coarsens as the answer gets less sensitive — is worth remembering.

### 5.3 When the AI actually pulls the trigger

```csharp
float aimAngle = Vector3.Angle(gunForward, leadPoint - aircraftPos);
float angleRate = Mathf.Clamp((aimAngle - lastAimAngle) / dt, -20f, 0f);   // negative = closing
float cone = Mathf.Clamp(50f * currentTarget.maxRadius / targetDist, 0.5f, 3f);   // degrees
if (targetDist < effectiveRange && aimAngle + Mathf.Min(angleRate * 0.25f, 0f) < cone)
    pilot.Fire();
```

**[inferred] The firing cone is angular size, not a constant** — `50 · radius /
distance` clamped to 0.5°–3°, so the AI opens fire when the pipper is within the
target's apparent size, which is the correct criterion and scales automatically
from a helicopter at 300 m to a bomber at 2 km. And the `angleRate` term means it
fires **slightly early when the pipper is sweeping onto the target**, leading the
tracking error itself. Only negative rates count (`Clamp(..., -20, 0)`), so
sweeping *off* the target does not delay the shot — it just does not help.

---

## 6. Threat awareness

**[CODE]** `ThreatTracker`, `ThreatList`, `ThreatVector`, `OpportunityThreat`,
`MissileWarning`, `RadarWarning`, `TerrainWarningSystem`.

`MissileWarning` is what feeds the AI's evasion (§7.4) and the player's RWR. The
important structural fact is that missile *launch* events are broadcast to the
target through `AICombat_OnRegisterMissile` / `OnMissileAlert`, so the AI's
knowledge of an inbound missile is event-driven and shares the same source as the
player's warning tone. `missileReactTime` gates the response — the AI does not
react instantly, and the delay is a skill parameter.

`TerrainWarningSystem` produces an `urgency` scalar and a
`GetFollowTerrainWaypoint` used by the autopilot (§7.2), which is how terrain
avoidance gets folded into ordinary steering rather than being a separate
override behaviour.

---

## 7. AI flight control

The most transferable part of the codebase, and the direct answer to *"how do the
AI handle movement, given drag and a turning circle?"*

### 7.1 The AI is not privileged

**[CODE]** The single most important fact:

```csharp
// AutopilotPlane.AutoAim, last two lines of the control path:
forwardFlightController.ApplyInputs(controlInputs, airspeed, new Vector3(pitchErr, yawErr, rollErr));
aircraft.FilterInputs();                          // ← the same fly-by-wire the player's stick goes through
```

**The AI writes to `ControlInputs` — the same struct the human stick writes to —
and its output then passes through the same `ControlsFilter`, the same control
surfaces, and the same aerodynamics.** There is no AI flight model, no
"AI can turn at X°/s" parameter, no teleport-to-waypoint. An AI aircraft is
subject to exactly the drag, stall, g limit and control lag the player is.

**[inferred] This is the decision that makes everything else in this section
necessary, and it is why the code is worth reading.** Most games give AI aircraft
a kinematic controller because a physical one is hard. Doing it properly means
every problem the player has — energy management, turn radius, not stalling on
the approach — becomes a problem the AI must actually solve, in code, and those
solutions are §§7.2–7.5. It is also why **[PATCH]** *"Improved maneuvering and
terrain avoidance skills for AI aircraft"* ships in the same update as *"New
aerodynamics and Fly-by-wire control filters"* — they are the same change.

The layering:

```
AIPilotCombatModes   (1,269 lines)  what to do: attack mode, target, weapon, evasion
       ↓ writes `destination`, `throttle`, `aimEffort`, `bankAllowed`, flags
AutopilotPlane.AutoAim (150 lines)  how to fly there: heading rate limit, bank, terrain
       ↓ writes pitch/yaw/roll error, runs AeroPID
ControlsFilter.Filter               the fly-by-wire (nuclear_option.md §6)
       ↓
ControlSurface job → transforms → AeroJob → forces
```

### 7.2 The turning circle is a rate limit on the *demand*

This is the answer to the turning-circle question and it is **one line**:

```csharp
float speedFactor = (effort > 1f || radarAlt < 1f) ? 1f
                  : Mathf.Clamp01(airspeed / aircraftParameters.cornerSpeed);

waypointDelta = Vector3.RotateTowards(
        new Vector3(currentHeading.x, 0f, currentHeading.z),     // where I am pointing, flattened
        NormalizedDirection(aircraftPos, destination),            // where I want to point
        0.9f * speedFactor * speedFactor,                         // ← max radians of turn demanded
        0f);
```

**The commanded direction is not the direction to the target.** It is the current
heading rotated toward the target by at most `0.9 · (V/V_corner)²` radians.

**[inferred] Why this works, and why it is better than computing a radius.**

- At or above corner speed, `speedFactor` is 1 and the demand can swing up to
  0.9 rad (52°) from the current heading in one tick — effectively unlimited,
  because the fly-by-wire's g limiter is now the binding constraint and it will do
  the right thing.
- At half corner speed, the factor is 0.25 and the demand can only swing 0.225 rad
  (13°). The AI therefore *asks for a gentler turn when it is slow*, which is
  exactly the behaviour a turn radius produces, without anyone computing a radius,
  a turn centre, or an arc.
- The **square** is what makes it feel right: turn rate for a given g goes as
  `g/V` and available g goes as `V²`, so sustainable turn rate rises roughly
  linearly with speed while *radius* rises as `V²`. Squaring the speed ratio
  matches the radius relationship, so the AI's willingness to demand heading
  change tracks what the airframe can deliver.
- It composes with everything else. Terrain avoidance, exclusion zones and
  evasion all work by *moving the destination*, and they inherit the rate limit
  for free.

**[inferred] The general pattern is worth naming: rate-limit the *demand*, not
the response.** A controller given an achievable demand behaves well with simple
gains. A controller given an impossible demand saturates, winds up its integrator,
and then overshoots on the way back — and no amount of tuning fixes it. This is
the same idea as the missile's 45° demand clamp (§2.4) and the FBW's own
`targetPitchAngVel` (physics note §6.2), appearing for the third time in the same
codebase.

`turningRadius` *does* exist as an `AircraftParameters` field — but it is used for
**route geometry**, not for control: the landing state uses
`turningRadius * 3f` to place the glideslope intercept point. **Two different
mechanisms for two different jobs**: a radius to plan where to go, a rate limit to
fly there.

### 7.3 Bank, pitch and the rest of `AutoAim`

The aircraft is flown **bank-to-turn**, and the roll error is computed as the
angle between the aircraft's up vector and the desired lift direction, measured
about the velocity vector:

```csharp
float currentBank = TargetCalc.GetAngleOnAxis(cockpit.up, Vector3.up, -velocity);
float wantedBank  = TargetCalc.GetAngleOnAxis(desiredLift, Vector3.up, -velocity);
wantedBank        = Mathf.Clamp(wantedBank, -bankAllowed, bankAllowed);
rollError         = currentBank - wantedBank;

float pitchError  = TargetCalc.GetAngleOnAxis(velocity, waypointDir, cockpit.right);
pitchError       *= Mathf.Lerp(1f, 0.1f, Mathf.Abs(rollError) * 0.015f);    // ← don't pull while rolling
```

**[inferred] The `pitchError` attenuation is a small line with a large effect.**
Pulling g while still rolling into the turn drags the lift vector through the
wrong plane and produces exactly the "AI aircraft wallowing through a turn" look.
Scaling pitch demand down to 10% while the bank error is large means the aircraft
**rolls first, then pulls** — which is what a pilot does.

`bankAllowed` is then scaled by four independent factors, each guarding a
different failure:

```csharp
if (preventInvertedFlight) bankAllowed = Mathf.Min(bankAllowed, 135f);
bankAllowed *= Mathf.Clamp(radarAlt * 0.003f - 1f, 0.6f, 1.2f);      // less bank near the ground
bankAllowed *= Mathf.Clamp(verticalError * 0.05f + 1f, 1.2f, 0.5f);  // less bank when it needs to climb
bankAllowed *= Mathf.Max(speedFactor * speedFactor, 0.45f);          // less bank when slow
```

Yaw is used **only** for runway alignment and below 0.5 m radar altitude, and is
explicitly zeroed when the target is more than 20° off:

```csharp
if (targetAngle > 20f && !runwayAlign && radarAlt > 0.5f) yawError = 0f;
```

**[inferred] That is the AI being told "do not try to yaw the nose around" —**
the classic novice mistake in a flight controller, which produces sideslip, drag,
and a nose that never quite arrives. Rudder is for crosswind and the flare, and
nothing else.

Two low-speed guards worth copying:

```csharp
if (airspeed < landingSpeed) desiredLift += Vector3.up * 0.5f;        // raise the nose, do not stall
if (targetAngle > 60f && targetDist < 2000f && heightDiff < 1000f)
    waypointDelta += 2000f * Mathf.Clamp01(airspeed / cornerSpeed - 1f) * Vector3.up;
```

The second is an **energy-aware manoeuvre choice**: when the target is well off
the nose and close, go *up* — but only in proportion to how far above corner speed
you are. Below corner speed the term is zero and the AI turns in the horizontal
instead. That is a one-line vertical-versus-horizontal decision, made on energy
state.

### 7.4 Energy and throttle: the AI manages speed explicitly

Throttle is set by the *combat mode*, not by the autopilot, and the guns mode is
the interesting one:

```csharp
// UseFixedGuns
controlInputs.throttle =
    (aircraft.speed > cornerSpeed && aircraft.speed > currentTarget.speed * 1.5f) ? 0f : 1f;

if (currentTarget.speed > 30f) {
    if (targetDist < 500f && targetAngle < 45f && aircraft.speed - currentTarget.speed > 60f)
        controlInputs.throttle = 0f;                                  // about to overshoot: idle
    ignoreCollision = true;
}
```

**[inferred] Two rules, both energy management.** *Idle when you are above corner
speed and much faster than the target* — because above corner speed extra speed
buys nothing in turn rate and costs turn radius. *Idle when closing fast inside
500 m* — because you are about to overshoot into a guns defence. Both are
expressed against `cornerSpeed`, which is the single per-aircraft number that
makes the whole system portable across airframes.

The rest of the modes are simpler and honest about it: `NoTarget` flies at
`aircraftParameters.cruiseThrottle`; almost everything else commands full
throttle; IR evasion commands **zero** (§7.5).

The landing state is where speed control gets a real controller:

```csharp
adjustedLandingSpeed = Mathf.Sqrt(mass / maxWeight) * aircraftParameters.landingSpeed;
...
float targetSpeed = cornerSpeed + Distance(aircraft, glideslopeAimpoint) * 0.02f;
float speedError  = aircraft.speed - targetSpeed;
controlInputs.throttle = Mathf.Clamp(0.5f - speedError * 0.1f, 0f, cruiseThrottle);
```

**[inferred] `sqrt(mass / maxWeight) · landingSpeed` is exactly right** — stall
speed goes as `√(W/S)`, so an aircraft returning heavy flies a faster approach,
with no table and no per-loadout tuning. And the target speed itself tapers with
distance to the aimpoint (`cornerSpeed + 0.02 · d`), so the aircraft flies fast
when far and decelerates continuously onto the approach speed rather than
stepping. Throttle is then a plain proportional controller on speed error biased
around 0.5.

The glideslope gets an **integrator**:

```csharp
glideslopeCorrection += new Vector3(Clamp(err.x, -4, 4) * 0.2f,
                                    Clamp(err.y, -1, 1) * 0.2f,
                                    Clamp(err.z, -4, 4) * 0.2f) * dt;
```

clamped per-axis and reset outside 10 s to touchdown — trim for the approach, so a
consistent offset (wind, weight, a damaged wing) is flown out rather than fought.

### 7.5 Evasion — the AI reads the sensor model backwards

**[CODE]** `EvadeModeRadar` is my favourite thing in the codebase:

```csharp
targetHeight = 10f;                                          // ← get low: attack the CLUTTER term

Vector3 toMissile = missile.position - aircraft.position;
float   closing   = Mathf.Max(Dot(-toMissile.normalized, missile.velocity - aircraft.velocity), 1f);
missileImpactTime = toMissile.magnitude / closing;

Vector3 tmp        = Cross(missile.GetEvasionPoint() - aircraftPos, aircraft.velocity);
missileEvadeVector = Cross((aircraftPos - missile.GetEvasionPoint()).normalized, tmp);
if (Dot(missileEvadeVector, aircraft.forward) < 0f) missileEvadeVector *= -1f;   // pick the near side

bool inTheNotch = missileImpactTime < 8f
               && Vector3.Angle(missileEvadeVector, aircraft.velocity) < 20f;
if (!aircraft.countermeasureTrigger && inTheNotch)                                 // ← chaff ONLY when established
    aircraft.Countermeasures(active: true, activeIndex);

float urgency = (missileImpactTime > 7f) ? 0.3f : 1f;
evadeDestination = Vector3.Lerp(destination,
        aircraftPos + missileEvadeVector * 1000f + randomErrorVector * 100f / max(skill, 0.01f),
        urgency);

if (missileImpactTime < 2f) { destination += Vector3.up * 1000f; followTerrain = false; }  // last-ditch
controlInputs.throttle = 1f;
```

**[inferred] Unpack the double cross product.** `(me→missile) × ((missile→me) ×
myVelocity)` is the component of the aircraft's velocity **perpendicular to the
missile's line of sight**. Steering onto it drives
`Dot(losDirection, velocity)` — the exact expression in
`RadarParams.GetSignalStrength` — toward zero.

**So the AI notches by computing the perpendicular of the LOS and flying it, and
it simultaneously descends to 10 m to maximise the clutter subtraction. Those are
the two independent terms in the signal equation, and the AI attacks both.** No
behaviour tree node called "notch". The evasion is the sensor model, solved for
the aircraft's velocity vector.

Three further details:

- **Chaff is only released once the aircraft is within 20° of the notch vector and
  inside 8 s to impact.** That is the real technique — chaff dropped while the
  doppler gain still has you is wasted, because the seeker can discriminate. It
  also stops the AI emptying its dispensers at long range.
- **`urgency` blends the evade destination in gradually** (30% weight beyond 7 s,
  100% inside), so the aircraft does not abandon its attack for a missile that is
  still 20 seconds out.
- **`randomErrorVector * 100f / skill`** — a *skill-scaled positional error on the
  evasion point*. A low-skill AI notches badly. One line, and it is a far better
  difficulty knob than scaling damage or reaction time, because it degrades the
  *quality of the manoeuvre* rather than cheating.

IR evasion is two lines and just as pointed:

```csharp
private void EvadeModeIR() {
    if (missileReactTime > 0f) {
        controlInputs.throttle = 0f;                                       // ← attack IRSource.intensity
        if (!aircraft.countermeasureTrigger) aircraft.Countermeasures(true, activeIndex);
    }
}
```

Throttling to idle drops `TurbineEngine.heatSource.intensity` toward `IRMin`,
which lowers `signal` in the seeker's `dazzleAmount > signal` comparison (§3.3),
which makes the flares win. **Again: the counter-tactic is the sensor equation
read backwards.**

### 7.6 The mode machine

`AIPilotCombatModes` runs **two orthogonal state machines**: an `AttackMode`
(NoTarget, FlyingToTarget, BreakOffAttack, RetreatStandoff, UsingMissiles,
UsingFixedGuns, UsingLaserGuided, Bombing, GlideBombing, Jammer, EnergyWeapon) and
an `EvadeMode` (None, IR, Radar). Evasion writes a *destination* and the attack
mode writes a *destination*, and `RunEvadeMode` blends between them by urgency —
so the two machines compose rather than one pre-empting the other.

Mode selection runs on **staggered intervals** — `On1sInterval`, `On2sInterval`,
`On5sInterval`, `On10sInterval` — with the expensive work (`AssessHQTargets`) on
the slowest tick. Each mode function takes a `checkMode` flag and does its own
exit test at the top, so **the transition condition lives next to the behaviour it
guards** rather than in a central table.

**[inferred]** Compare [`ai_state_machines.md`](ai_state_machines.md): this is the
"each state owns its exit conditions" pattern, plus the tiered-update pattern from
[`space_engineers.md`](space_engineers.md) §6. Arrived at independently, which is
mild evidence both are forced by the problem.

The mode functions also set a small set of *steering flags* consumed by the
autopilot — `aimEffort`, `bankAllowed`, `ignoreCollision`, `followTerrain`,
`aimVelocity`, `climbing`, `targetHeight`. **[inferred] That interface is the
thing to copy**: the tactical layer does not steer, it sets a destination and
half a dozen constraint flags, and the flight layer owns everything about how the
aircraft gets there. It is the reason the same combat AI drives a bomber and a
fighter.

### 7.7 Altitude as an integrator

```csharp
private void ManageAltitude() {
    if (targetHeight < aircraftParameters.minimumRadarAlt)
        targetHeight = Mathf.Lerp(targetHeight, aircraftParameters.minimumRadarAlt, 0.5f);
    if (targetHeight < aircraft.radarAlt + 200f) targetHeight += 5f;
    ...
    bool wantHigher = (targetObscured && inRange)
                   || (target.radarAlt > 10f && target.y > aircraft.y)
                   || (targetDist < 5000f && aircraft.y < targetKnownPosition.y);
    targetHeight += wantHigher ? 20f : -10f;
}
```

**[inferred] `targetHeight` is not a setpoint, it is an accumulator** — it drifts
up at 20 m per call when the AI wants to be higher and down at 10 m when it does
not, with an asymmetric rate so climbing is decided twice as fast as descending.
The AI therefore never commands a step change in altitude, and the hysteresis
falls out of the asymmetry instead of needing two thresholds. The same trick
appears as `climbFactor` in the gun attack (rising at 100/s while `climbing`,
falling at 500/s otherwise — a fast-attack, slow-decay ramp that keeps the AI from
porpoising during a strafing run).

---

## 8. What is worth taking

1. **Make counter-tactics fall out of the sensor equation rather than scripting
   them.** (§1.5, §3.3, §7.4.) If detection is a formula with named terms, then
   "how do I hide" has an answer a player can derive and an AI can compute. The
   double-cross-product notch vector is the canonical example: it is the
   *analytic solution* for "minimise this dot product", and it is two lines.

2. **Rate-limit the demand, not the response.** (§7.2, §2.4.)
   `RotateTowards(current, wanted, maxRate)` where `maxRate` is a function of the
   vehicle's current capability. A controller given an achievable demand behaves
   well with simple gains; one given an impossible demand saturates and
   overshoots, and no tuning fixes it. This appears three times in this codebase,
   which is what a load-bearing pattern looks like.

3. **Separate "where to point" from "how to point there".** (§2.4.) Eight seekers,
   one autopilot, one `SetAimpoint(pos, vel)` interface. Loft, jink and datalink
   midcourse are all aimpoint modifications with no autopilot changes. Worth the
   loss of true proportional navigation.

4. **Sensor error should be a fixed per-instance bias, not per-frame noise.**
   (§3.2.) `Random.insideUnitSphere * error` generated once at launch. Noise
   averages out and gets ignored by a controller; a bias produces a consistent
   wrong answer that has to be corrected, which is what a real sensor does.

5. **Cheap geometric rejection before the expensive test.** (§1.2.) Two square
   roots for the radar horizon, removing a raycast. Straight out of
   [`spatial_queries.md`](spatial_queries.md) §3.6.

6. **Batch the expensive test.** (§1.1.) Every pending line-of-sight check in the
   game becomes one `RaycastCommand.ScheduleBatch`, Burst-scheduled, batch 16.

7. **Randomise the phase of periodic work.** (§1.1.) One line
   (`await Delay(random * period)`) prevents every detector spawned in the same
   frame from scanning on the same frame forever. This is a real periodic frame
   spike that is otherwise very hard to diagnose.

8. **Let the update period carry the alert state.** (§1.1.) `alertCheckInterval`
   when something was found, `checkInterval` otherwise. One field instead of a
   state.

9. **Scale the firing/acceptance cone by apparent angular size.** (§5.3.)
   `clamp(k · radius / distance, min, max)` automatically does the right thing
   across target sizes and ranges, and adding the *rate of change* of aim error
   fires early when the pipper is sweeping on.

10. **Coarsen an integrator's step as the answer becomes less sensitive.**
    (§5.2.) `timeStep = lerp(0.02, 0.1, dist·k)` and then `timeStep += 0.02` each
    iteration, so a 4 km shot does not cost 200× a 20 m shot.

11. **Two solvers for two consumers.** (§5.2.) The AI gets a 2-iteration
    closed-form estimate; the player's pipper gets a full forward simulation at
    10 Hz, smoothed. Accuracy where it is seen, cheapness where it is not.

12. **Use an accumulator with asymmetric rates instead of a setpoint with
    hysteresis.** (§7.7.) `targetHeight += wantHigher ? 20 : -10` gives smooth
    commands and hysteresis from one expression.

13. **Difficulty as manoeuvre quality, not as cheating.** (§7.4.)
    `randomErrorVector * 100f / skill` degrades the *evasion geometry*. A weak AI
    notches badly, which is legible to the player as bad flying rather than as bad
    numbers.

14. **Give the tactical layer a destination and constraint flags, never the
    stick.** (§7.6.) `destination`, `throttle`, `aimEffort`, `bankAllowed`,
    `ignoreCollision`, `followTerrain`, `aimVelocity` — and the flight layer owns
    everything else. It is why one combat AI flies every airframe.

And two anti-patterns:

15. **Do not dispatch on strings.** (§4.) `threatTypes.Contains(seekerType)`
    where `seekerType` is `"IR"` from an overridden method. The data-driven shape
    is right; the string is a silent-failure waiting to happen and an enum costs
    nothing.

16. **Do not ship a second, worse copy of a core equation.** (§1.6.)
    `EstimateDetection` reimplements the signal calculation with a hardcoded
    clutter factor and no doppler. The *intent* is defensible — the HUD should
    not reveal the notch. But it should have been the same function with terms
    disabled, not a second one to keep in step.

---

## 9. What is *not* established

- **Nothing was run or profiled.** No measurement of detection cost, missile
  counts, or AI think time.
- **No authored values.** Every `RadarParams`, seeker tuning value, PID gain,
  `flareRejection`, `dopplerFactor` and `cornerSpeed` lives in ScriptableObjects
  and prefabs whose type trees are stripped from this build — see
  [`nuclear_option.md`](nuclear_option.md) §19. **So I can describe every equation
  and none of its constants.** Where a number appears above it is a hardcoded
  literal in the C#, not a tuning value.
- **Most of the gaps below are now closed by
  [`nuclear_option_command.md`](nuclear_option_command.md)** — SARH and ARM
  seekers, the optical/laser/inertial family, `TopAttack` and `JinkEvasion`, the
  full gun mount (heat, dispersion, sub-frame timing, recoil), `FireControl`'s
  battery logic, the shared `CombatAI` scoring library, the helicopter AI, ground
  vehicles, ships and the strategic layer. What follows is what remains unread
  after all three notes.
- **`Turret`, `WeaponStation`, `WeaponManager` and `AimSolver`** were read only
  where they intersect the paths described; the turret traverse/elevation loop
  specifically was not read.
- **`AIPilotCombatModes` is 1,269 lines and I read perhaps half of it** — the
  gun, missile, altitude and evasion paths. The bombing, glide-bombing, jammer,
  laser-guided and energy-weapon modes were skimmed.
- **`OpticalSeekerCruiseMissile`** (terrain-following cruise guidance) and the AI
  takeoff/taxi/short-landing states were sampled, not read.
- **No developer account exists.** Every "because" above is **[inferred]** — my
  reconstruction of a reason, not a reported one.

---

## 10. Where things are

| System | Files |
|---|---|
| Radar & detection | `Radar.cs`, `RadarParams.cs`, `TargetDetector.cs`, `NuclearOption.Jobs/DetectorManager.cs`, `DetectionRequest.cs`, `RadarLocator.cs` |
| EW | `RadarJammer.cs`, `JammingPod.cs`, `RadarWarning.cs`, `MissileWarning.cs`, `IRadarReturn.cs`, `IRSource.cs` |
| Missile airframe & autopilot | `Missile.cs` (`Steering`, `ApplyAero`, `Motor`, `ProxyFuse`), `PID2D.cs`, `PID3D.cs`, `TerminalBoost.cs`, `TopAttack.cs` |
| Seekers | `MissileSeeker.cs`, `ARHSeeker.cs`, `IRSeeker.cs`, `SARHSeeker.cs`, `ARMSeeker.cs`, `OpticalSeeker*.cs`, `LaserSeeker.cs`, `InertialSeekerShell.cs`, `BallisticMissileGuidance.cs` |
| Lead & ballistics | `TargetCalc.cs`, `Kinematics.cs`, `AimSolver.cs`, `Parabola.cs`, `ArtilleryCalc.cs` |
| Countermeasures | `CountermeasureManager.cs`, `Countermeasure.cs`, `FlareEjector.cs`, `ChaffEjector.cs`, `IRFlare.cs`, `RadarChaff.cs`, `SpecialFlare.cs` |
| Guns | `Gun.cs`, `BulletSim.cs`, `FireControl.cs`, `FiringCone.cs`, `Turret.cs`, `WeaponStation.cs` |
| AI flight | `Autopilot.cs`, `AutopilotPlane.cs`, `AutopilotHelo.cs`, `AutopilotTiltwing.cs`, `AeroPID.cs` |
| AI tactics | `AIPilotCombatModes.cs`, `CombatAI.cs`, `JinkEvasion.cs`, `ThreatTracker.cs`, `ThreatList.cs`, `OpportunityThreat.cs`, `TerrainWarningSystem.cs` |
| AI procedures | `AIPilotTakeoffState.cs`, `AIPilotLandingState.cs`, `AIPilotShortLandingState.cs`, `AIPilotTaxiState.cs`, `AIHelo*.cs` |

---

## Sources

- **The retail install**, `E:\SteamLibrary\steamapps\common\Nuclear Option`,
  `Assembly-CSharp.dll` decompiled with ILSpy 8.2.
- [Nuclear Option — Development (release notes)](https://nuclearoption.wiki.gg/wiki/Development) — the only first-party technical source; the radar horizon, transonic drag, missile manoeuvring drag, RCS-with-stores and multithreading entries are quoted above.
- [Nuclear Option on Steam](https://store.steampowered.com/app/2168680/Nuclear_Option/).
- **No engineering talk, blog or paper was found**, and the developer interviews that exist cover roadmap rather than implementation.
