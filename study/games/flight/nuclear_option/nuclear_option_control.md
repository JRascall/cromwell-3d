# Nuclear Option — control and movement, end to end

The fourth and last note on Nuclear Option, written to close the question *"have
you read all the movement and control code?"* The honest answer when it was asked
was no — three areas were unread, and one of them was the player's own input path,
which is the most basic thing in a flight game. This note covers them: **every
path from an input to a force**, human or AI, in the air, on the ground, on the
water and on foot, plus the wind field that perturbs all of them.

Same source and caveats: read from the decompiled `Assembly-CSharp.dll` of the
retail Mono build. **No developer talk, blog or paper exists.** Tags: **[CODE]**
read from the assembly, **[PATCH]** the release notes, **[inferred]** my reading.
Nothing here comes from running the game. §11 is a final audit of what is *still*
unread across all four notes.

Two organising observations, and they are the reason this note is worth having
separately from the other three:

> **1. There is exactly one control interface, and everything writes to it.**
> `ControlInputs` is six floats. A human stick writes it, a mouse writes it, an AI
> combat state writes it, an autopilot writes it, a taxi procedure writes it. Then
> it goes through the same fly-by-wire and the same aerodynamics. There is no
> "AI control path" anywhere in this codebase, and §2 is what that buys.
>
> **2. There is one contact idiom, reused four times.** *Cast a ray down, build a
> spring-damper along the hit normal, add a righting torque toward the surface,
> and subtract the surface's own velocity.* That is the landing gear, the ground
> vehicle suspension, the hovercraft skirt, **and the pilot's legs.** §4 is that
> idiom and why it is worth recognising as one thing.

Related: [`nuclear_option.md`](nuclear_option.md) (vehicle physics),
[`nuclear_option_combat.md`](nuclear_option_combat.md) (sensors, guidance, air AI),
[`nuclear_option_command.md`](nuclear_option_command.md) (world model, batteries,
surface units), [`moving_frame_navigation.md`](../../../topics/agents/moving_frame_navigation.md) (§4's
surface-velocity subtraction is the physics-layer version of that note's problem),
[`nuclear_option_audio.md`](nuclear_option_audio.md) (the camera-state coupling
in §3.2 is the other half of this note's input/presentation story),
[`ai_state_machines.md`](../../../topics/agents/ai_state_machines.md).

---

## 1. The player's input path

**[CODE]** Input goes through **Rewired** (`Rewired_Core`, plus native DirectInput
and Windows.Gaming.Input plugins), which handles device enumeration, binding,
deadzones and curves outside the game's code. What the game writes is
`PilotPlayerState`, 281 lines, and it does three interesting things.

### 1.1 The throttle heuristic

This is the best thing in the file and it is eight lines:

```csharp
float raw     = Mathf.Clamp(player.GetAxisRaw("Throttle"), -1f, 1f);
float rawPrev = Mathf.Clamp(player.GetAxisRawPrev("Throttle"), -1f, 1f);
...
float delta = Mathf.Abs(raw - rawPrev);
if (delta > 0f && delta < 0.5f)
    simulatedThrottle = raw;                                            // ABSOLUTE
else if (Mathf.Abs(raw) > 0.5f)
    simulatedThrottle += Mathf.Clamp(raw - simulatedThrottle, -Time.deltaTime, Time.deltaTime);  // RELATIVE
```

**[inferred] It detects what kind of hardware you have from how the axis moves.**
A physical throttle lever moves in small increments frame to frame, so `delta` is
small and the game snaps the commanded throttle to the lever position — absolute
control, which is what a HOTAS user expects. A keyboard or a button jumps from 0
to 1 in one frame, so `delta ≥ 0.5`, and the game instead treats the axis as a
*direction* and ramps the simulated throttle at one unit per second — relative
control, which is what a keyboard user expects.

**One heuristic, no setting, both hardware classes correct.** There *is* a
`PlayerSettings.throttleUseRelative` override that quantises the axis to
`sign(raw)` and forces the relative branch, for the case where the detection is
wrong (a very jittery analogue axis, or a detented lever). But the default needs
no configuration, and getting this wrong is one of the most common complaints
about flight games on mixed hardware.

Two more axis details in the same function. `PlayerSettings.throttleUseNegative`
remaps a centred axis (`−1..1`) to `0..1` for a lever that idles at centre, and
`invertCollective` flips it for helicopter players who want collective-down at the
top. And:

```csharp
if (player.GetButton("Axis Modifier")) { customAxis1 += raw; raw = 0f; }
```

**a modifier button that redirects the throttle axis onto `customAxis1`** — flaps,
tilt, swing-wing sweep — so a two-axis stick can fly a tiltrotor. That is a real
accommodation for limited hardware, in three lines.

### 1.2 The virtual joystick, for mouse flight

```csharp
Vector3 pos = flightHud.virtualJoystickPos.transform.localPosition;
pos += virtualJoystickSensitivity * Mathf.Min(Time.unscaledDeltaTime, 0.1f) * 30f
     * new Vector3(input.GetAxis("Pan View"), -invert * input.GetAxis("Tilt View"), 0f);
pos  = Vector3.ClampMagnitude(pos, 150f);
pos  = Vector3.Lerp(pos, Vector3.zero, virtualJoystickCentering * 2f * Time.deltaTime);
flightHud.SetVirtualJoystick(pos);

pitchInput = -pos.y / 150f;
rollInput  =  pos.x / 150f;
if (aircraft.radarAlt < spawnOffset.y + 1f) yawInput = pos.x / 150f;   // nosewheel steering on the ground
```

**[inferred] The mouse moves a *stick*, not an aim point.** The cursor's motion
accumulates into a virtual stick deflection clamped to a 150-pixel circle, which
**self-centres** by lerping toward zero at a configurable rate. That is the
correct scheme for mouse flight — it preserves the "hold the stick over to keep
turning" model of a real aircraft, unlike the mouse-aim scheme where the aircraft
chases the cursor and the pilot never learns what a control input is. The
centering rate is the single knob that makes it feel like a spring-loaded stick
versus a friction-held one.

Two details: `Time.unscaledDeltaTime` clamped to 0.1 s, so a frame hitch cannot
throw the stick to the stop; and on the ground the same X deflection also drives
yaw, so the mouse steers the nosewheel without a separate binding.

Mouse and hardware axes are **added, not switched**:

```csharp
pitchInput += player.GetAxis("Pitch");
rollInput  += player.GetAxis("Roll");
yawInput   += player.GetAxis("Yaw");
controlInputs.pitch = Mathf.Clamp(pitchInput, -1f, 1f);
```

so a stick and a mouse can both be live, and a rudder pedal works alongside a
mouse-flown aircraft.

### 1.3 G-LOC takes control away

```csharp
pilotStrength = gloc.SimulateGLOC(pilot.gForce);       // returns bloodPressure, 0..1
...
if (pilotStrength < 0.2) { controlInputs.pitch = controlInputs.roll = controlInputs.yaw = 0f; return; }
```

and in `PlayerControls`, the same threshold gates **every button** — gear,
countermeasures, weapons, ejection. **[inferred] Losing consciousness does not
grey the screen and let you keep flying; it removes your inputs.** The physical
model in [`nuclear_option.md`](nuclear_option.md) §15 (a blood-pressure integrator
against a stamina pool) therefore has a hard behavioural consequence, and the
aircraft continues under whatever trim the fly-by-wire's integrating output
happened to be holding — which is why a G-LOC in this game is a spiral rather than
a freeze.

### 1.4 Where the split lives

```csharp
public override void UpdateState(Pilot pilot)      => PlayerControls();        // buttons, per FRAME
public override void FixedUpdateState(Pilot pilot) { pilotStrength = gloc.SimulateGLOC(...);
                                                     PlayerAxisControls();      // axes, per FIXED STEP
                                                     pilot.aircraft.FilterInputs(); }
```

**[inferred]** Buttons are polled on the render frame (so a press is never missed
between fixed steps at low tick rates), axes are sampled on the fixed step (so the
control law sees exactly one sample per integration step). Getting this backwards
produces either missed button presses or aliased stick input, and both are the
kind of bug that is blamed on "input lag".

---

## 2. `ControlInputs` — six floats, and everything writes them

**[CODE]** The entire control surface of every aircraft in the game:

```csharp
public class ControlInputs {
    public float pitch, roll, yaw;
    public float throttle;
    public float brake;
    public float customAxis1;      // flaps / tilt / sweep / collective-secondary, per airframe
}
```

Writers, all of them: `PilotPlayerState` (§1), `AIPilotCombatModes`,
`AIPilotTakeoffState`, `AIPilotTaxiState`, `AIPilotLandingState`,
`AIPilotShortLandingState`, the four helicopter states, `Autopilot` and its three
subclasses, `ControlsFilter`'s auto-hover, `AircraftNetworkTransform` (replaying a
remote player's inputs for visual control-surface animation), and
`SwingWingController` / `TiltWingController` / `DuctedThrustSystem` writing
`customAxis1` on the pilot's behalf.

Readers: `ControlsFilter` (which rewrites them in place), then `ControlSurface`,
`RotorShaft`, `Turbofan`, `Turbojet`, `DuctedFan`, `ConstantSpeedProp`,
`LandingGear`, `Airbrake`, `MagicTorqueController`, `ReactionControlSystem`.

**[inferred] The value of this is not elegance, it is that the AI cannot cheat.**
Because there is one struct and one downstream path, an AI aircraft is subject to
the identical servo rate limits, fly-by-wire gain scheduling, stall behaviour,
g-limits and thrust lag as a human. Any improvement to the flight model improves
the AI's flying for free, and any bug in the flight model is a bug the AI has to
cope with too — which is a very effective forcing function for the model's
quality. It also means **a replayed remote player's inputs animate their control
surfaces correctly on your machine** with no separate animation path (see
[`nuclear_option.md`](nuclear_option.md) §13).

The cost is that `customAxis1` is a semantic hole — its meaning is per-airframe and
nothing in the type says so — and that `ControlsFilter.Filter` mutates the struct
in place rather than returning a new one, so the order of the assist layers is
load-bearing and invisible.

---

## 3. The AI's flight procedures

The tactical AI ([`nuclear_option_combat.md`](nuclear_option_combat.md) §7) is
only one of the pilot's states. The others are procedures, and they are where the
awkward parts of flying live.

### 3.1 Takeoff

**[CODE]** `AIPilotTakeoffState`, 192 lines.

```csharp
float airspeed = (aircraft.rb.velocity - LevelInfo.i.GetWind(position)).magnitude;   // ← TRUE airspeed
controlInputs.customAxis1 = (airspeed > takeoffSpeed * 0.7f) ? 0.5f : 1f;            // flap schedule
```

**[inferred] The rotation decision is made on airspeed relative to the wind
field**, so a headwind means the aircraft unsticks at a lower groundspeed and a
tailwind means a longer roll — a real consequence of the Perlin wind field (§5)
falling out of one subtraction.

The aim point is the runway centreline projected ahead by `50 + speed * 3` metres —
a lookahead that **grows with speed**, which is what keeps the steering stable as
the roll accelerates. Before the run begins, the aircraft lines up *slowly*:

```csharp
if (Vector3.Dot(forward, runwayDirection) > 0.95f) startedTakeoffRun = true;
if (startedTakeoffRun) { throttle = 1f; brake = 0f; }
else                   { throttle = Mathf.Clamp01(1f - speed * 0.1f);
                         brake    = Mathf.Clamp01(speed * 0.1f); }
```

— throttle falls and brake rises with speed until the nose is within 18° of the
runway heading, at which point it commits. **[inferred] A latch, not a threshold**:
`startedTakeoffRun` never resets, so a gust that swings the nose after the run
begins does not abort it.

There is a ski-jump branch (aim along the velocity vector with the vertical
component clamped to 50–150 m) and a twelve-second stuck timer that ejects the
pilot and parks the aircraft — a garbage-collection path for an aircraft that has
been blocked in, which matters because a jammed runway would otherwise stall the
whole faction's sortie generation (`Runway.RegisterStartTakeoff` /
`RegisterTakeoffLeftRunway` track occupancy).

### 3.2 Taxi — the 524-line one

**[CODE]** The most procedurally detailed state in the game, and the one I would
point at if asked what "polish" looks like in AI code.

**Speed target:**

```csharp
float headingPenalty = 1f + Vector3.Angle(forward, steerVector) * 0.02f;   // how far off am I pointing
float cornerPenalty  = 1f + steerInfo.nextWaypointAngle * 0.02f;           // how sharp is the next turn
float target = (15f / cornerPenalty + Mathf.Min(Mathf.Sqrt(steerDistance), 15f)) / headingPenalty;
if (takeoffQueued) target *= 0.5f;
target = Mathf.Max(target, 4f);

controlInputs.throttle = Mathf.Clamp(0.5f + (target - aircraft.speed) * 0.3f, 0f, 0.5f);
controlInputs.brake    = (aircraft.speed > target) ? 1f : 0f;
controlInputs.throttle -= brakeUrgency;
controlInputs.brake    += brakeUrgency;
```

**[inferred]** A 15 m/s base divided by corner sharpness, *plus* a term growing as
the square root of the distance to the steer point (so it accelerates down long
straights), all divided by how far off heading it is, floored at 4 m/s, and halved
when queueing. Throttle is a proportional controller on the speed error capped at
half power. Legible, and every term is a sentence you could say out loud.

**Yielding.** Ground collision avoidance between taxiing aircraft, checked at 1 Hz:

```csharp
foreach (Obstacle o in obstacles) {
    Vector3 toObs = o.Transform.position - aircraft.position;
    float facing      = Dot(toObs.normalized,  aircraft.forward);       // is it ahead of me
    float theirFacing = Dot(-toObs.normalized, o.Transform.forward);    // are they pointing at me
    if (facing < 0f) continue;                                          // behind: ignore
    float gap = toObs.magnitude - o.Radius - aircraft.maxRadius;

    if (gap < 50f && theirFacing > 0f) {                                // head-on conflict
        if (!yielding && pilot.flightInfo.HasTakenOff) {
            yielding = true;
            Vector3 escape = Vector3.RotateTowards(forward, -toObs.normalized, Mathf.PI/2f, 0f);
            yieldPosition = aircraft.GlobalPosition() + escape * 50f;   // pull 90° off and stop
            break;
        }
        if (!(facing > theirFacing)) break;                             // ← the right-of-way rule
    }
    brakeUrgency += (aircraft.speed > Mathf.Sqrt(Mathf.Max(gap - 10f, 0f))) ? 1f : 0f;
}
```

**[inferred] Three things.** The conflict test is mutual — *it is ahead of me* AND
*it is pointing at me* — so an aircraft crossing behind is ignored. The
right-of-way tiebreak is `facing > theirFacing`: whoever has the other more
squarely ahead is the one that must act, which is a symmetric rule that resolves
deterministically without communication. And **only an aircraft that has already
flown yields** (`HasTakenOff`) — a returning aircraft gives way to one departing,
which is both correct airfield etiquette and a deadlock-breaker, because the two
classes cannot both defer.

`brakeUrgency` is a **braking-distance test**: brake if your speed exceeds
`sqrt(gap − 10)`. That is the same shape as the ground vehicle corner rule in
[`nuclear_option_command.md`](nuclear_option_command.md) §7.2 — a speed compared
against the square root of available distance — and it appears independently in
three places in this codebase.

Plus: `WaitingForTakeoffClearance`, `WaitingAtRunwayCrossing` (hold short of an
active runway), a `Wait()` that brakes and resets both stuck timers, taxiing on a
`RoadNetwork` returned by `airbase.GetTaxiNetwork()` when one exists and a direct
moving-target follow when it does not, and **two independent stuck detectors** —
`CheckStuckSpeed` and `CheckStuckYaw`, because an aircraft can be pinned against
geometry while its nose still swings.

### 3.3 Landing

**[CODE]** Covered partly in
[`nuclear_option_combat.md`](nuclear_option_combat.md) §7.4; the parts I had not
read:

```csharp
adjustedLandingSpeed = Mathf.Sqrt(mass / maxWeight) * aircraftParameters.landingSpeed;
```

— approach speed scaling as `√(W/S)`, correct and table-free. The pattern is laid
out from `turningRadius`:

```csharp
Vector3 aim = runwayUsage.GetGlideslopeAimpoint(aircraft, aircraftParameters.turningRadius * 3f, 30f);
aim += Vector3.RotateTowards(-approachDir * offset, -toAim, Mathf.PI / 2f, 0f);
```

so the join point is placed three turn radii out — **the authored `turningRadius`
is used for *route geometry* while the actual turn is rate-limited by
`0.9·(V/V_corner)²`** (combat note §7.2). Two mechanisms, two jobs, and neither
tries to do the other's.

Speed is a proportional controller on an error whose *target tapers with distance*:

```csharp
float targetSpeed = aircraftParameters.cornerSpeed + Distance(aircraft, aimpoint) * 0.02f;
controlInputs.throttle = Mathf.Clamp(0.5f - speedError * 0.1f, 0f, cruiseThrottle);
```

and the glideslope error is fed to a **clamped per-axis integrator**
(`glideslopeCorrection`, ±4 lateral / ±1 vertical, reset outside 10 s to
touchdown) — trim for the approach, so a consistent offset from wind or weight is
flown out rather than fought. Moving decks are handled by leading the touchdown
point with the runway's own velocity:

```csharp
toTouchdown = touchdownPoint + runwayUsage.Runway.GetVelocity() * touchdownTime - aircraftPos;
```

### 3.4 Helicopter procedures

`AIHeloLandingState` re-checks `landingPoint.IsAvailable()` every step and bails
back to the combat state if the pad is taken — **the pad is a reservation, not a
coordinate**. `AIHeloTakeoffState` is 108 lines and mostly a vertical climb to a
transition height. `AIHeloTransportState` (716 lines) is the largest single AI
state in the game: it searches for a landing spot near an objective
(`SearchForLandingSpot`), defends itself with missiles while en route
(`DefendWithMissiles`), runs its own countermeasure logic, and deploys cargo or
troops (`DeployCargo`) — a full logistics behaviour rather than a movement one.

---

## 4. The contact idiom, four times

**[inferred]** Once you have read all of them, the same shape appears in four
unrelated systems, and recognising it as one idiom is worth more than any of the
individual implementations.

> Cast a ray downward. Build a spring-damper along the hit normal. Add a torque
> that rotates the body's up axis toward the surface normal. **Subtract the
> surface's own velocity** before computing anything. Clamp the tangential force
> to a friction circle proportional to the normal load.

| System | The ray | The spring | The righting torque | Surface velocity |
|---|---|---|---|---|
| `LandingGear` | strut linecast, `suspensionTravel` | `springRate · compression + dampingRate · v_n` | (none — the wheel is on a hinge) | `attachedRigidbody.GetPointVelocity` **and** `AnimatedPhysicsSurface.GetVelocity` |
| `GroundVehicleJob_Math2` | one ray per vehicle, amortised | same, into a cached `surfacePlane` | `1.5 · −Cross(planeNormal, up) − 0.1 · ω` | `sampleGroundResult.hitPointVelocity`, and the plane is *translated* between samples |
| `AirCushion` | plane raycast, `maxHeight` | `spring · (maxHeight − h) + damp · v_n` | `50 · −Cross(surfaceNormal, up) − 15 · ω` | (skirt rides the water plane) |
| `PilotDismounted` | 1.5 m linecast under the pilot | `(1.3 − hitDist − 0.2·v_y) · mass · 25` | `Cross(up, worldUp) · mass · 4` | `hit.collider.attachedRigidbody.GetPointVelocity` |

**[inferred] Two things follow from noticing this.** First, **the pilot on foot is
not a character controller** — it is a rigidbody standing on a leg spring, which
is why an ejected pilot tumbles convincingly, gets blown over by a shockwave, can
be picked up by a sling hook, and stands correctly on a moving carrier deck with
no special case. A `CharacterController` would have needed all four of those
written separately. Second, **the surface-velocity subtraction is not an
afterthought anywhere** — it appears in three of the four, and
`AnimatedPhysicsSurface` exists solely to let an *animated* collider (a lift, a
deck elevator, a ramp) report a velocity it does not have a rigidbody for. That is
the physics-layer twin of
[`moving_frame_navigation.md`](../../../topics/agents/moving_frame_navigation.md)'s problem, and the
answer is the same: **make the moving frame's velocity explicitly queryable.**

The ground-vehicle version has one extra trick worth stealing. Because its ray is
amortised across ticks, the surface plane would go stale on a moving surface — so
between samples it *translates the cached plane* by the relative velocity:

```csharp
if (fields.sampleGroundResult.hasHitRB) {
    velocity = fields.velocity - fields.sampleGroundResult.hitPointVelocity;
    fields.surfacePlane.Translate(velocity * shared.Ref().fixedDeltaTime);   // keep the stale plane honest
}
```

**A cached query result that dead-reckons itself between refreshes.** That pattern
generalises well beyond ground contact.

---

## 5. Ground vehicle locomotion, in full

**[CODE]** `GroundVehicleJob_Math2`, 115 lines, is the whole of it — no wheel
colliders, no per-wheel anything.

```csharp
// normal load from the single suspension probe
float spring = springRate * Mathf.Max(suspensionTravel - radarAlt, 0f);
float damp   = (spring > 0f) ? -dampingRate * Mathf.Min(Dot(surfacePlane.normal, velocity), 0f) : 0f;
Vector3 normalForce = surfacePlane.normal * (spring + damp);

// self-righting, and yaw-rate damping
Vector3 righting = 1.5f * -Cross(surfacePlane.normal, transform.Up()) - 0.1f * angularVelocity;

// lateral grip: resist velocity along the axis perpendicular to forward and the normal
Vector3 lateralAxis = Cross(transform.Forward(), surfacePlane.normal);
Vector3 resist      = new Vector3(-normalForce.x, 0f, -normalForce.z) - velocity * mass * 10f;
Vector3 lateral     = Vector3.Project(-velocity * 10f + resist, lateralAxis);
Vector3 braking     = (-velocity * 10f + resist) * inputs.brake;
Vector3 rolling     = new Vector3(Sign(-v.x) - v.x*0.05f, ..., ...) * (0.05f * (spring + damp));

// engine: force falls off with speed, capped by a per-surface top speed
float topSpeed = sampleGroundResult.onPaved ? topSpeedOnroad : topSpeedOffroad;
Vector3 drive = (Mathf.Abs(speed) < topSpeed * 0.2778f)
    ? transform.Forward() * mass * Mathf.Clamp(engineOutput * 40f / Mathf.Max(Mathf.Abs(speed), 1f),
                                               -acceleration * 5f, acceleration * 5f)
    : Vector3.zero;

// THE FRICTION CIRCLE
Vector3 tangential = Vector3.ClampMagnitude(lateral + braking + rolling, (spring + damp) * frictionCoef);
tangential += drive;

fields.AddForce(normalForce + tangential);
fields.AddTorque(inputs.steering * 0.2f * transform.Up() + righting);
```

**[inferred] Four properties this buys for the price of one raycast.**

**A real friction circle.** Lateral grip, braking and rolling resistance are summed
and then `ClampMagnitude`d to `normalLoad × frictionCoef` — so a vehicle braking
hard has less lateral grip, and one cornering hard cannot also brake hard. That is
the single most important behaviour in vehicle dynamics and it is one clamp. Note
the drive force is added *after* the clamp, deliberately: the engine is not
traction-limited, which keeps a tank from being unable to climb out of a ditch.

**A torque curve.** `engineOutput · 40 / max(|speed|, 1)` — force inversely
proportional to speed, i.e. constant power, clamped to ±5× the vehicle's
`acceleration`. Strong from rest, fading at speed, with a hard top speed that
**differs on and off paved surfaces** (`sampleGroundResult.onPaved`). So roads
matter to the physics, not just to the pathfinder.

**Self-righting.** `1.5 · −Cross(planeNormal, up)` is a torque proportional to the
sine of the tilt angle, damped by `0.1 · ω`. **[PATCH]** 0.34 lists *"Vehicles now
have better controlled self-righting behaviour"* — this is the term that was
tuned, and it is the reason a vehicle that clips a rock rocks back onto its tracks
instead of flipping.

**Steering is a raw torque** (`steering · 0.2 · up`), not a wheel angle — correct
for tracked vehicles, adequate for wheeled ones at this fidelity, and it composes
with the yaw damping in the righting term.

And the whole thing runs inside a Burst `IJobParallelFor`, with both the ground
raycast and the AI's input evaluation round-robined across ticks by
`ShouldRunSampleGround(i, tickOffset)` / `ShouldRunInputs(i, tickOffset)` — so a
few hundred vehicles cost a fraction of their nominal per-tick price.

---

## 6. Wind and weather

**[CODE]** The field itself is in [`nuclear_option.md`](nuclear_option.md) §3.5;
what I had not read is where it comes from and who consumes it.

### 6.1 The field

```csharp
public Vector3 GetWind(GlobalPosition p) {
    p += windOffset;                                          // scrolls with time
    Vector3 n = (-0.75f + Perlin(p.z*0.02f) + 0.5f*Perlin(p.z*0.1f)) * windZone.forward
              + 0.5f * (-0.75f + Perlin(p.x*0.02f) + 0.5f*Perlin(p.x*0.1f)) * windZone.right
              + 0.3f * (-0.75f + Perlin(p.y*0.02f) + 0.5f*Perlin(p.y*0.1f)) * Vector3.up;
    return windVelocity + windTurbulence * Mathf.Max(windSpeed, 10f) * n;
}
```

Two octaves per axis (wavelengths ~50 m and ~10 m), anisotropic — full strength
along the wind, half across it, 0.3 vertically — over a mean `windVelocity`. The
`Mathf.Max(windSpeed, 10f)` floor means **turbulence exists even in dead calm**,
which is right: still air over terrain is not smooth.

### 6.2 It changes over time

```csharp
private float windChangeDelay = 120f;
...
if (windRandomArc > 0f) {
    if (Time.realtimeSinceStartup > lastWindChange + windChangeDelay) {
        lastWindChange = Time.realtimeSinceStartup;
        UpdateWind();                                       // re-roll within windRandomArc of windMainHeading
    }
    float heading = GetWindHeading();
    heading = Mathf.LerpAngle(heading, windMainHeading, Time.deltaTime);   // ease toward it, ~1 s constant
    UpdateWindHeading(heading);
}
```

**[inferred] The wind re-rolls a target heading every two minutes within an arc
around a mission-authored prevailing direction, and the actual heading eases
toward it continuously.** So it wanders rather than switching, the mission
designer controls the prevailing direction and how much it varies
(`windRandomArc`), and over a long sortie the crosswind on the runway you took off
from will have changed by the time you come back. `windVelocity`, `windSpeed`,
`windTurbulence`, `conditions` and `cloudHeight` are all `[SyncVar]`s on
`LevelInfo`, so **every client has the same wind**, which matters because the
flight model is client-authoritative — two clients disagreeing about the wind would
show up as position rejections.

`WeatherSet` is a tiny ScriptableObject (`coverage`, a cloud `mask`, a
`particleSampler`, light `cookies`, a `lightning` flag) and `conditions` is a
scalar that also dims the ambient light (`GetAmbientLight` multiplies by
`1 − conditions * 0.5`), so weather is one authored asset plus one scalar.

### 6.3 Twenty-two consumers

`GetWind` is called from `Aircraft`, the aero job (inlined, because Burst cannot
call it — the duplication noted in [`nuclear_option.md`](nuclear_option.md) §3.5),
`RotorShaft`, `SoftBodyRotor`, `ConstantSpeedProp`, `PropFan`, `Missile`,
`InertialSeekerShell`, three `OpticalSeeker` variants, `IRFlare`, `SpecialFlare`,
`RadarChaff`, `Parachute`, `Container`, `MushroomCloud`, `Autopilot`,
`AIPilotTakeoffState`, `AIPilotLandingState`, `AoAFeedback` and
**`CameraStateManager`**.

**[inferred] The list is the point.** Wind is not an aircraft feature — it is a
property of the world that everything unsupported by the ground reads. Flares and
chaff drift downwind (which changes the angular-separation term in the seeker
models, so *wind affects countermeasure effectiveness*). Parachutes drift, so an
ejected pilot lands somewhere the wind decided. Artillery shells and bombs are
displaced. The AI computes true airspeed for rotation and approach. And the camera
reads it, presumably for shake or for the sense of movement in an external view.

**[inferred] That breadth is only affordable because the field is analytic.** Two
Perlin octaves per axis is a handful of cycles; there is no wind volume texture to
sample, nothing to stream, and no synchronisation problem beyond five `SyncVar`s.
A grid-based wind field would have made most of those twenty-two call sites too
expensive to justify.

---

## 7. Things that hang under aircraft

### 7.1 Parachutes are a two-body system

**[CODE]** `Parachute` does not use a joint. It has the payload's rigidbody and a
**manually integrated canopy point mass** joined by a spring line with slack:

```csharp
lineVector  = canopy.position - transform.position;
lineTension = Mathf.Max(lineVector.magnitude - lineSlackLength, 0f) * lineSpring;

Vector3 canopyAir = canopyVel - LevelInfo.i.GetWind(transform.GlobalPosition());
float   drag      = maxDrag * chuteDrag.Evaluate(openAmount) * airDensity;

canopyForce = -Vector3.up * canopyMass * 9.81f          // canopy weight
            - lineVector.normalized * lineTension        // pulled by the payload
            - canopyAir * drag;                          // its own drag, against the WIND

openAmount += Mathf.Min(canopyAir.magnitude * 0.03f, 1f) * Time.deltaTime;    // inflates with airspeed
lineTension = Mathf.Min(lineTension, rb.mass * 200f);                          // sanity clamp
rb.AddForceAtPosition(lineVector.normalized * lineTension, transform.position);
rb.AddTorque(Cross(-rb.transform.up, -lineVector.normalized * lineTension) * rb.mass * 0.001f * damping);
canopyVel      += canopyForce / canopyMass * Time.deltaTime;
canopy.position += canopyVel * Time.deltaTime;
```

**[inferred]** The canopy is integrated by hand rather than being a second
rigidbody, which avoids a joint and a solver interaction for something that only
ever pulls. The torque term swings the payload upright under the canopy. The chute
**inflates progressively** (`openAmount` grows with canopy airspeed, feeding a drag
curve), so deployment is a process rather than an event. Deployment conditions are
a five-way gate — altitude band, time-since-spawn band, and speed band — so a chute
opens neither too high, too low, too early nor too fast. And the shader gets
`_wrinkleDisplacment` from accumulated canopy travel and `_wrinkleStrength` from
`wrinkleStrength.Evaluate(openAmount) / clamp(lineTension·0.0005, 0.5, 2)`, so the
canopy visually tightens as the lines load.

### 7.2 Sling loads

`SlingloadHook` (515 lines) is a four-state machine — `Retracting`, `Deployed`,
`Connected`, `RescuePilot` — with a notable networking wrinkle:

```csharp
if (suspendedUnit != null && aircraft.LocalSim && !aircraft.IsServer
 && Time.timeSinceLevelLoad - lastTransformSent > 0.1f)
    aircraft.CmdSendSlungTransform(aircraft.transform.InverseTransformPoint(suspendedUnit.transform.position),
                                   suspendedUnit.transform.rotation);
```

**[inferred] The slung unit's transform is sent to the server *in the lifting
aircraft's local space*, at 10 Hz.** That is the right frame: the load's absolute
position is dominated by the helicopter's, which is already being replicated, so
sending the relative offset costs far less precision and stays correct through
the helicopter's own interpolation. `RescuePilot` is the same machinery picking up
a `PilotDismounted` — which works because §4 made the pilot a rigidbody.

### 7.3 The dismounted pilot

Beyond §4's leg spring:

```csharp
// tunnelling guard for a fast-moving ejected pilot
if (speed > 30f && Physics.Raycast(position, rb.velocity, out hit, speed * 1.1f * dt, Statics)) {
    transform.position = hit.point + Vector3.up * 1f;
    rb.velocity = Vector3.Reflect(rb.velocity, hit.normal) * 0.3f;      // bounce, 30% restitution
}

// water
if (inWater) {
    rb.AddForce(Vector3.up * rb.mass * 25f * Clamp01(Datum.LocalSeaY - position.y));   // buoyancy
    rb.AddTorque(Cross(transform.up, Vector3.up) * rb.mass * 8f);                       // float upright
    rb.drag = 20f; rb.angularDrag = 5f;
}

// g-death
private bool TooMuchForce() => FastMath.OutOfRange(velocityPrev.Value, rb.velocity, 500f * dt);
if (!disabled && TooMuchForce()) KillPilot();
```

**[inferred]** A pilot who ejects into the airstream at 300 m/s dies from the
velocity change, not from a scripted check — `TooMuchForce` is a 500 m/s² threshold
on the frame-to-frame velocity delta, which is the same quantity the server uses
to validate aircraft movement ([`nuclear_option.md`](nuclear_option.md) §13). And
a pilot down near a wrecked aircraft is gently pushed away from it
(`(position − cockpitPosition).normalized · mass · 8`), which is a small thing that
stops the corpse-in-the-cockpit clipping problem.

---

## 8. Turrets

**[CODE]** `Turret` (860 lines) has five acquisition modes matching
`FireControl`'s (`datalink`, `fireControl`, `assignedTargetDetectors`,
`parentUnitTargetDetector`, manual), rate-limited traverse and elevation
(`traverseRate`, `elevationRate`, `traverseRange`, `min/maxElevation`), a
`lockTime` before it will fire, `FiringCone[]` masking so a turret cannot shoot
through its own airframe, and:

```csharp
traverseRate *= Mathf.Max(a, 0.33f);      // scaled by condition/power, floored at a third
```

with `armorTierOptimism` letting a turret take shots slightly beyond its rated
penetration, `onlyDefensive` restricting some mounts to inbound threats, and
`newTargetSearchAfterFire` controlling whether it re-acquires between bursts.
Aiming goes through `AimSolver` and the same `TargetCalc.TargetLeadTime` as the
aircraft guns.

**[inferred]** The turret is the clearest case in the game of *the same component
serving player and AI*: `manual` swaps the target source for the player's
crosshair, and everything downstream — traverse rates, firing cones, lead solution,
lock time — is identical. A player-manned turret is not a different code path,
which is why gunner positions feel mechanically consistent with AI ones.

---

## 9. What is worth taking

1. **Detect absolute versus relative input hardware from the axis delta.** (§1.1.)
   `delta < 0.5` → snap to the value; otherwise treat as a direction and ramp. One
   heuristic, no setting, and it makes a HOTAS and a keyboard both correct.

2. **Mouse flight should move a stick, not an aim point.** (§1.2.) Accumulate
   motion into a clamped virtual deflection that self-centres at a configurable
   rate. It preserves the control model the aircraft actually has.

3. **Poll buttons per frame, sample axes per fixed step.** (§1.4.) Backwards gives
   you missed presses or aliased stick input, and both get blamed on input lag.

4. **One control struct, written by everything.** (§2.) The AI cannot cheat,
   improvements to the flight model improve the AI for free, and replicated inputs
   animate remote players' control surfaces with no separate path.

5. **Recognise the contact idiom and write it once.** (§4.) Ray, spring-damper on
   the normal, righting torque, **subtract the surface velocity**, clamp tangential
   force to a friction circle. It is the landing gear, the vehicle suspension, the
   hovercraft and the pilot's legs.

6. **Make a person a rigidbody on a leg spring, not a character controller.**
   (§4, §7.3.) Ragdolling, shockwaves, sling-load pickup and standing on a moving
   deck all then need no code.

7. **Dead-reckon a cached query result between refreshes.** (§4.)
   `surfacePlane.Translate(relativeVelocity * dt)` keeps an amortised ground sample
   honest on a moving surface — the pattern generalises to any expensive query
   sampled at less than tick rate.

8. **The friction circle is one `ClampMagnitude`.** (§5.) Sum lateral, braking and
   rolling resistance, clamp to `normalLoad × μ`, and add drive force *after* so a
   vehicle can still climb out of a hole.

9. **Compare speed against the square root of available distance.** (§3.2, §5,
   and [`nuclear_option_command.md`](nuclear_option_command.md) §7.2.) It is the
   braking-distance test, it appears three times independently in this codebase,
   and it is the cheapest correct answer to "should I be slowing down".

10. **Latch a commitment rather than re-testing a threshold.** (§3.1.)
    `startedTakeoffRun` never resets, so a gust cannot abort a roll that has begun.

11. **Right-of-way needs a symmetric, communication-free tiebreak.** (§3.2.)
    `facing > theirFacing` resolves deterministically from geometry alone, and
    restricting yielding to one class (`HasTakenOff`) guarantees the deadlock
    cannot form.

12. **Keep an environment field analytic if many systems must read it.** (§6.)
    Two Perlin octaves per axis is cheap enough for twenty-two consumers; a wind
    volume texture would have priced most of them out.

13. **Let the environment wander, not switch.** (§6.2.) Re-roll a target every two
    minutes within an authored arc and ease toward it. The mission designer keeps
    control of the prevailing conditions and the player experiences change.

14. **Send an attached object's transform in its carrier's local space.** (§7.2.)
    Cheaper, more precise, and correct through the carrier's own interpolation.

15. **Integrate a one-way constraint by hand instead of adding a joint.** (§7.1.)
    The parachute canopy is a point mass with its own drag, and it never pushes —
    so it needs no solver participation.

And one thing I would not copy:

16. **`customAxis1` is a semantic hole.** One float whose meaning — flaps, tilt,
    sweep, secondary collective — is per-airframe and recorded nowhere in the type.
    It is the price of the single control struct, and a small enum or a named
    accessor per airframe would have cost nothing.

---

## 10. Final audit — what is still unread

Across all four notes, after this pass. Everything here is a genuine gap, not a
summary.

- **Nothing was ever run or profiled**, and **no authored value was recovered** —
  the build ships with script type trees stripped
  ([`nuclear_option.md`](nuclear_option.md) §19). Every number in all four notes is
  a hardcoded literal in the C#.
- **`AIPilotCombatModes`** — roughly half read. The bombing, glide-bombing, jammer,
  laser-guided and energy-weapon attack modes were skimmed, not read.
- **`AIHeloTransportState` (716 lines)** — structure only. Its landing-spot search,
  cargo deployment and self-defence logic were not read.
- **`AIPilotShortLandingState` (405 lines)** — not read. Vertical and short landing
  procedures for the VTOLs.
- **`Airbase.cs` (~1,600 lines)** — read only through the interfaces others use.
  Runway assignment, parking, rearm/repair queues and the reserve system are
  substantially unread.
- **`MissionManager`, `MissionRunner`, `NuclearOption.SavedMission` and the
  objective node graph** — not read at all. The mission editor
  (`NuclearOption.MissionEditorScripts`, 120 types) likewise.
- **`OpticalSeekerCruiseMissile` (359 lines)** — terrain-following cruise guidance,
  not read.
- **The UI and HUD layer** — `CombatHUD`, `FlightHud`, the MFD apps, `DynamicMap`,
  `TacScreen` — not read, beyond noting that `EstimateDetection` feeds it.
- **The camera system** — `CameraStateManager` and its eleven states — not read.
- **All shaders and the render pipeline internals.** The rendering paragraph in
  [`nuclear_option.md`](nuclear_option.md) §15 is a list of class names, not a
  reading of what they draw.
- **Audio**, `SoundManager`, the mixer routing and Doppler handling — not read.
- **`NuclearOption.Workshop`, `.Social`, `.Chat`, `.DedicatedServer`,
  `.SceneLoading`, `.UIStyleSystem`, `.AddressableScripts`** — not read.
- **No developer account of any of this exists.** Every "because" across all four
  notes is **[inferred]** — my reconstruction of a reason, not a reported one.

What *has* been read, and read properly: the whole flight model (fixed wing,
rotary, VTOL, propellers, jets), the ship physics and flooding model, landing gear
and ground contact, the atmosphere and wind, the job/threading architecture, the
network authority model, the floating origin, radar and IR sensing, missile
airframes and autopilots, all eight seekers to at least their guidance law,
countermeasures, guns and ballistics, the shared world model and target scoring,
fire-control batteries, the aircraft and helicopter combat AI, ground vehicle
physics and navigation, ship AI, the strategic layer, and — as of this note — the
player input path, the AI flight procedures and the contact idiom.

---

## 11. Where things are

| System | Files |
|---|---|
| Player input | `PilotPlayerState.cs`, `Pilot.cs`, `PilotBaseState.cs`, `ControlInputs.cs`, `ControlsMenu.cs`, `PlayerSettings.cs`, `RewiredSaveDataMigrator.cs`, `HeadTrackerManager.cs`, `ExtraUiInput.cs` |
| Tuning UIs | `ControlsFilterTuner.cs`, `FlyByWireTuner.cs`, `PIDTuner.cs` |
| AI procedures | `AIPilotTakeoffState.cs`, `AIPilotTaxiState.cs`, `AIPilotLandingState.cs`, `AIPilotShortLandingState.cs`, `AIHeloTakeoffState.cs`, `AIHeloLandingState.cs`, `AIHeloTransportState.cs`, `PilotParkedState.cs` |
| Ground contact | `LandingGear.cs`, `GearPart.cs`, `AnimatedPhysicsSurface.cs`, `AirCushion.cs`, `NuclearOption.Jobs/GroundVehicleJob_Math2.cs`, `SampleGroundResult.cs` |
| Ground locomotion | `GroundVehicle.cs`, `NuclearOption.Jobs/GroundVehicleJob_Math1.cs`, `GroundVehicleFields.cs`, `GroundVehicleJobSettings.cs` |
| Wind & weather | `LevelInfo.cs` (`GetWind`, `UpdateWind`, `windRandomArc`), `WeatherSet.cs`, `CloudLayer.cs`, `Lightning.cs`, `MapSettings` |
| Suspended things | `Parachute.cs`, `SlingloadHook.cs`, `EjectionSeat.cs`, `EscapeCapsule.cs`, `Container.cs`, `MountedCargo.cs`, `CargoDeploymentSystem.cs`, `CargoRamp.cs` |
| On foot | `PilotDismounted.cs`, `NuclearOption.NetworkTransforms/PilotDismountedNetworkTransform.cs`, `MountedTroops.cs` |
| Turrets | `Turret.cs`, `AimSolver.cs`, `FiringCone.cs`, `FiringConeChecker.cs`, `HUDTurretState.cs`, `TurretAutoIndicator.cs` |

---

## Sources

- **The retail install**, `E:\SteamLibrary\steamapps\common\Nuclear Option`,
  `Assembly-CSharp.dll` decompiled with ILSpy 8.2.
- [Nuclear Option — Development (release notes)](https://nuclearoption.wiki.gg/wiki/Development) — the only first-party technical source; the vehicle self-righting entry is quoted above.
- [Nuclear Option on Steam](https://store.steampowered.com/app/2168680/Nuclear_Option/).
- **No engineering talk, blog or paper was found.**
