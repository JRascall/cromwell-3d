# Nuclear Option — a flight model where the geometry *is* the model

Deep dive on **Nuclear Option** (Shockfront Studios, Early Access since October
2023), read from the retail install on this machine. The interest here is the
**vehicle physics** — fixed wing, rotary, VTOL, ground handling and ships — and
Nuclear Option is an unusually good subject for two reasons.

First, it ships as a **Mono** Unity build, so `Assembly-CSharp.dll` decompiles to
near-original C# and the entire simulation can be read line by line. Nothing in
this note is inferred from identifier tables or file magics the way
[`broken_arrow.md`](broken_arrow.md) had to be; where an equation appears below,
it was read out of a method body.

Second, it is the **opposite architecture** to Broken Arrow. Broken Arrow is
~90 bought packages around six in-house assemblies. Nuclear Option's entire
third-party manifest is **26 assemblies, of which none render anything** — the
networking library, the input library, the async library, Steam, Discord, an eye
tracker, and a JSON parser. Everything that flies, floats, draws terrain,
streams or serialises was written. Two Unity games, the same engine, opposite
answers to buy-versus-build. §16 reads them against each other.

**§17 is a reconstruction guide** — the data model, the exact tick order, and
per-archetype recipes, written so this can be rebuilt rather than only admired.

**The combat systems are in two companion notes.**
[`nuclear_option_combat.md`](nuclear_option_combat.md) covers radar and how
notching works in code, missile guidance and seekers, countermeasures, gunnery,
and how the AI flies an aircraft that has a turning circle and has to manage its
energy. [`nuclear_option_command.md`](nuclear_option_command.md) covers the shared
world model, target scoring, SAM and ship batteries, gun mounts, the helicopter
AI, ground vehicles and their navigation, ship AI, and the strategic layer.
[`nuclear_option_control.md`](nuclear_option_control.md) covers the player's own
input path, the AI's takeoff/taxi/landing procedures, ground locomotion, the
wind field in full, and the ground-contact idiom shared by landing gear,
vehicles, hovercraft and the pilot's legs.
[`nuclear_option_audio.md`](nuclear_option_audio.md) covers Doppler at high
Mach, sound travel delay, the Mach cone, and engine sound layering.

> **Sources, and their limits.** There is **no GDC talk, no technical blog, no
> paper, and no engineering interview** for this game. I looked. The developer
> (**B25Mitch**) gives community interviews about roadmap and content, not
> architecture. So the primary source here is the decompiled assembly, and the
> only *first-party* corroboration is the release notes — which turn out to be
> unusually good, because they name the systems (§15).
>
> Tags: **[CODE]** read from the decompiled `Assembly-CSharp.dll`. **[BUILD]**
> read from the retail install. **[PATCH]** the developer's own release notes.
> **[STORE]** the Steam store page. **[inferred]** my reading.
>
> Nothing here comes from running or profiling the game. **Per-aircraft authored
> values were not recoverable** — §19 says why, and §17 gives physically-derived
> starting values instead.

Related: [`broken_arrow.md`](broken_arrow.md) (the buy-versus-build counterpart
in the same engine), [`space_engineers.md`](space_engineers.md) (the other
"simulate the assembly, not the object" game),
[`sea_of_thieves_water.md`](sea_of_thieves_water.md) (§11.6 is the contrast — a
game whose ocean *is* the physics), [`map_scale.md`](map_scale.md),
[`moving_frame_navigation.md`](moving_frame_navigation.md) (§9.4 is the same
moving-deck problem in the physics layer),
[`valve_networking.md`](valve_networking.md) (§13 reads the authority model
against Valve's).

---

## 1. What it is, technically

**[BUILD]**

| | |
|---|---|
| Engine | **Unity 2022.3.62f2**, **Mono** (not IL2CPP), Burst-compiled jobs (`lib_burst_generated.dll`) |
| Render pipeline | **URP** — `UnityEngine.Rendering.Universal` is referenced directly by gameplay code |
| Install size | **2.3 GB**, of which addressables are 100 MB (liveries and skins only) |
| Game code | **one assembly**, `Assembly-CSharp.dll`, 3.0 MB, ~1,200 types |
| Networking | **Mirage** (the Mirror fork) over a custom UDP socket layer (`nanosockets.dll`) |
| Third-party managed assemblies | **26 total**, listed below |
| Type trees | **stripped** for game scripts — see §19 |
| `boot.config` | `gfx-enable-gfx-jobs=1`, `gfx-enable-native-gfx-jobs=1`, `gc-max-time-slice=3` |
| Map extent | **100 km wide** **[STORE]**; world bounds ±90 km from datum **[CODE]** |

The complete non-Unity, non-BCL assembly list:

| Package | What it answers |
|---|---|
| `Mirage`, `Mirage.SocketLayer`, `Mirage.Sockets.Udp`, `Mirage.SteamworksSocket`, `Mirage.Authenticators`, `Mirage.Components` | networking, transport, Steam relay, auth |
| `UniTask` (+ 4 integrations) | allocation-free async over the Unity player loop |
| `Rewired_Core`, `Rewired_Windows` (+ 2 native) | input binding — a flight sim needs HOTAS, pedals, hat switches |
| `com.rlabrecque.steamworks.net`, `Discord.Sdk` | platform |
| `Newtonsoft.Json` | mission and save serialisation |
| `JamesFrowen.ScriptableVariables`, `JamesFrowen.Graphy`, `JamesFrowen.Build.Runtime` | utility + an on-screen perf overlay, by Mirage's maintainer |
| `RuntimeTransformHandle`, `StandaloneFileBrowser`, `Ookii.Dialogs` | mission-editor gizmos and file pickers |
| `ProfanityDetector`, `Interop.SpeechLib` | chat filter, TTS |
| Native: `tobii_gameintegration_x64.dll` | eye tracking |

**[inferred] Read that list for what is missing.** No terrain package, no
vegetation package, no instancing package, no impostor package, no upscaler, no
ECS framework, no serialisation framework, no DI container, no Lua, **no water
or buoyancy package**. The foliage renderer (`NuclearOption.Effects.DetailRenderer`,
`GrassRenderer`, `TreeRenderer`, `ComputeFrustumCulling`) is in-house and
compute-shader driven. The mission scripting is an in-house node graph
(`NuclearOption.NodeGraph`, 46 types).

That is a very different bet from Broken Arrow's, and §16 argues it is the
*correct* bet **for this specific game** for a reason that has nothing to do
with craftsmanship: the thing this game sells is how the vehicles feel, and that
is not for sale.

---

## 2. The one idea worth taking away

**[inferred]** Before any detail: the whole simulation follows one rule, and it
explains most of the design decisions below.

> **The vehicle's *geometry* is its physics model. Nothing else is.**

There is no lookup of "the F-16's pitch response". No stability derivative table,
no per-aircraft `rollRate` a controller drives toward, no per-ship `turnRadius`.
A vehicle is a hierarchy of parts; some parts are marked as lifting surfaces
with an area, an airfoil and a transform, or as hull compartments with a
displacement and a height. Every tick, each of those transforms is read in world
space, the local flow is resolved against **that transform's own orientation**,
a force comes out, and it is applied at that transform's position. Pitching
moment is what you get when the tailplane's force is applied 6 m behind the
centre of mass. A ship's righting moment is what you get when the submerged
compartments on the low side push harder than the ones on the high side.

Everything downstream falls out of that commitment:

| Feature | How it is implemented |
|---|---|
| Elevator authority | the elevator transform **rotates**; its own lift changes; the moment changes (§4) |
| Swing wing | the wing transforms **rotate**; area, centre of pressure and stall speed all change (§8.6) |
| Flaps / slats | the `AeroPart`'s **wing area is rewritten**; nothing else changes (§8.5) |
| Losing a wing | the part is destroyed and its area leaves the loop; the aircraft rolls because the other wing is still there (§5) |
| A shot-up wing | `wingEffectiveness` scales its lift down *and converts its wing area into drag area* (§3.3) |
| Ditching in water | air density is swapped to 1000 and `wingEffectiveness` to 0.01 — **the same code does hydrodynamics** (§3.6) |
| A ship listing | flooded compartments lose displacement; the remaining buoyancy is off-centre; the hull heels (§11.4) |
| A ship slowing when hulled | the damaged compartment's *longitudinal* drag coefficient is lerped toward its *lateral* one (§11.4) |
| Flying at 10 km | one LUT read changes ρ; lift, drag, engine power and speed of sound all move together (§10) |

This is the same idea as [`space_engineers.md`](space_engineers.md)'s grids —
*simulate the assembly and let the object's behaviour be a consequence* —
applied to fluid dynamics instead of rigid-body construction. It is expensive in
a very specific way (§12 is the whole answer to that cost) and it is the reason
a small team can ship thirteen aircraft **[STORE]** and eight ship classes with
genuinely distinct handling without authoring thirteen flight models.

---

## 3. Fixed-wing aerodynamics

### 3.1 The unit of simulation is `AeroPart`

**[CODE]** `AeroPart : UnitPart : MonoBehaviour`. Its serialised fields are the
entire aerodynamic description of a piece of aeroplane:

```csharp
[SerializeField] private float     wingArea;        // m², 0 for non-lifting parts
                 public  float     dragArea;        // m², parasitic
[SerializeField] private float     streamlining;    // drag ADDED TO THE PARENT when this breaks off
[SerializeField] private Transform liftNormal;      // the frame the airflow is resolved in
[SerializeField] private Vector3   centerOfLift;    // offset from the transform, for local torque
[SerializeField] private float     buoyancy;
[SerializeField] private float     airflowChanneling;
[SerializeField] private int       airfoil = -1;    // index into the aircraft's airfoil list
```

`liftNormal` is the important one. It defaults to the part's own transform, but
it is a separate reference so a designer can point the aerodynamic frame at
something that moves — which is how control surfaces work (§4).

**[inferred]** Note `streamlining`. When a part detaches it *adds* its
streamlining value to the drag area of the part it was attached to — the
aerodynamic penalty for the hole it leaves. The release notes call this out:
**"Severed aircraft parts now induce drag"** **[PATCH]**, Update 0.34. A
one-float model of a phenomenon most games do not model at all, sitting on the
part that knows what it was covering.

### 3.2 The airfoil is a 128-entry lookup table over the full ±180°

**[CODE]** `Airfoil` holds two `AnimationCurve`s — Cl(α) and Cd(α) — authored in
the editor. At level load, `JobManager.GenerateCharts()` walks every aircraft in
the encyclopedia, collects every airfoil, and bakes each curve into a flat
`NativeArray<float>`:

```csharp
for (int i = 0; i < 128; i++) {
    float time = (i - 64) * 0.04908734f;   // π/64 rad = 2.8125° per step
    result[i] = liftCoef.Evaluate(time);
}
```

128 samples × π/64 rad covers **−180° to +177.2°**. Reading it back is one
multiply-add and a lerp:

```csharp
float index = alpha * 20.37185f + 64f;     // 20.37185 = 64/π
liftCoef = ChartHelper.SafeRead(index, chart);
```

All airfoils for all aircraft live in two contiguous `NativeArray<float>`s of
`nAirfoils × 128`, indexed by `airfoilID * 128`. **[inferred]** Three things are
right about this:

- **The table covers the whole circle, not the linear range.** Post-stall,
  inverted, flat-plate, backwards — every attitude has a defined Cl and Cd, so
  spins, tumbles and departures are *the same code path* as cruise. The store
  page's "stalls, spins and g-force effects fully implemented" is not a special
  case; it is the absence of one.
- **The authoring format is an `AnimationCurve` and the runtime format is not.**
  The curve is a designer's tool; evaluating a Unity `AnimationCurve` inside a
  Burst job is impossible and inside a hot loop is slow. Bake once at load into
  the format the loop wants. This is the CLAUDE.md derived-cache pattern with
  the invalidation problem removed by construction, because the source is
  *content*.
- **128 entries is 512 bytes.** Every airfoil in the game fits in L1 together.

There is also a **hardcoded fallback** when `airfoilID < 0`, for parts that are
draggy but not properly foiled:

```csharp
liftCoef = 1.8f * Mathf.Sin(5f * alpha);
dragCoef = 1.5f * (1f - Mathf.Cos(2f * alpha)) + 0.02f;
```

A flat-plate approximation: peak Cl of 1.8 at α = 18°, symmetric drag bucket.
Cheap, plausible, and it lets an artist add a fairing without authoring a foil.

### 3.3 The force equations, exactly

**[CODE]** From `AeroJob_Math.Execute`, in the order the code computes them.
`v` is the part's rigidbody velocity minus the local wind, `ρ` is from the
altitude table, `S` is `wingArea`, `e` is `wingEffectiveness`:

**Angle of attack** — resolve the airflow in the lift transform's own frame:

```csharp
Vector3 vLocal = Quaternion.Inverse(liftTransform.Rotation) * v;
float   alpha  = Mathf.Atan2(vLocal.y, vLocal.z);
```

**[inferred] There is no separate sideslip term anywhere in the fixed-wing
model, and there does not need to be.** A vertical fin is an `AeroPart` whose
transform is rolled 90°; the *same* `atan2(y, z)` in *its* frame is the sideslip
angle, and the lift it produces is a side force. One equation covers both
because the geometry carries the difference. This is the §2 rule paying for
itself, and it is the single most important thing to copy.

**Lift** — perpendicular to the airflow and to the part's span axis:

```csharp
Vector3 liftDir = Vector3.Cross(v, liftTransform.Right()).normalized;
Vector3 lift    = -liftDir * (Cl * ρ * v² * 0.5f * S * e);
```

A true lift *direction*, recomputed from the current velocity — not the part's
local up. Induced-drag-by-vector-tilt therefore happens for free: at high α the
lift vector tilts backwards and retards the aircraft, which is why hard turns
bleed speed without a drag-due-to-lift term.

**Drag** — two independent terms:

```csharp
float damageDrag = (dragArea + S * 0.1f) * (1f - e);            // the wound
float parasitic  = 0.5f * ρ * v² * 0.5f * (dragArea + damageDrag);
float profile    = Cd  * ρ * v² * 0.5f * S * e;
float3 drag      = -normalize(v) * (profile + parasitic);
```

**[inferred]** The `damageDrag` line is the whole damage-affects-handling claim
in one expression: as a wing's effectiveness falls its *area does not
disappear* — 100% of its drag area and 10% of its wing area are re-added as pure
parasitic drag, scaled by how badly it is hurt. A holed wing lifts less **and
drags more**, which is what actually happens and what a naive `lift *= health`
model gets wrong.

`wingEffectiveness` itself is set in `ApplyDamage`:

```csharp
wingEffectiveness = Mathf.Lerp(0.5f, 1f, hitPoints * 0.01f);
```

so a wing at 0 hit points still makes half its lift — until it separates, at
which point `Detach()` sets it to zero and gives the loose part `rb.drag = 0.15`.

**Transonic drag rise:**

```csharp
if (v > 0.8*a && v < 1.2*a) {
    float m = Mathf.Min(Mathf.Abs((a - v) / a), 0.2f);   // |1 - M|, capped
    float t = (0.2f - m) / 0.2f;                          // 1 at M=1, 0 at the edges
    drag *= 1f + t*t*t * 0.15f;
}
```

A **+15% drag peak exactly at Mach 1**, falling off as a cubic to nothing at
M 0.8 and M 1.2. Corroborated: *"Transonic effect increases drag when an
aerodynamic simulated object gets close to Mach 1.0"* **[PATCH]**, 0.30.9.
**[inferred]** A cubic rather than a Gaussian because it is three multiplies and
reaches exactly zero at a known point — no tail to clamp.

The airframe also shakes between M 0.99 and M 1.0 (`Aircraft.FixedUpdate` calls
`ShakeAircraft(0, 0.25f * airDensity)`) — buffet as a function of dynamic
pressure, so it fades at altitude.

**Application:**

```csharp
fields.force = lift + drag;
if (centerOfLift != Vector3.zero) {
    fields.torque = Vector3.Cross(force, -(liftTransform.Rotation * centerOfLift));
}
```

and the main thread does `rb.AddForce` / `rb.AddTorque` after the job completes.
The `centerOfLift` offset is a *local* correction — the big pitching moments come
from the part's world position relative to the centre of mass, which
`AddForce` on a shared rigidbody would lose, so parts that need it use the
explicit cross product.

### 3.4 Airflow channelling

**[CODE]** One field, `airflowChanneling`, with an unusually large payoff:

```csharp
if (fields.airflowChanneling > 0f) {
    Vector3 target = otherTransform.Forward();
    v = Vector3.RotateTowards(v, target, fields.airflowChanneling, 0f);
}
```

The airflow seen by this part is rotated, by at most `airflowChanneling`
radians, toward another transform's forward axis. **[inferred]** This is the
model for *ducts, shrouds and intakes* — a lifting surface inside a duct does
not see the freestream, it sees whatever the duct is pointing at. It is also the
cheapest possible model of prop wash. One `RotateTowards` per part, and it buys
ducted VTOLs a sensible answer to "what happens when you tilt the duct at 60°
and 200 knots".

### 3.5 Wind is a Perlin field sampled per part

**[CODE]** Both `LevelInfo.GetWind(GlobalPosition)` and the identical inlined
copy inside the Burst job:

```csharp
Vector3 n = (-0.75 + Perlin(p.z*0.02) + 0.5*Perlin(p.z*0.1)) * windDir
          + 0.5 * (-0.75 + Perlin(p.x*0.02) + 0.5*Perlin(p.x*0.1)) * Cross(windDir, up)
          + 0.3 * (-0.75 + Perlin(p.y*0.02) + 0.5*Perlin(p.y*0.1)) * up;
return windVelocity + turbulence * max(windSpeed, 10) * n;
```

Two octaves per axis, anisotropic (along-wind full strength, cross-wind half,
vertical 0.3), and the whole field **scrolls with time** (`p += t * 10`). Each
aero part samples it at its own position, so a 30 m wingspan can be in different
air on each side and the aircraft rolls in turbulence without a rolling term.

**[inferred]** The function is duplicated — once in managed code for callers like
the rotor, once inlined into the job because Burst cannot call it. That is the
honest cost of the jobified path and it is worth noting as a maintenance
hazard: **two copies of the wind field that must agree.**

### 3.6 Water is aerodynamics with the constants swapped

**[CODE]** `AeroPart.OnTriggerStay` uses `Physics.ComputePenetration` against the
water collider to get a submerged depth, and the job does the rest:

```csharp
float frac = Mathf.Clamp01(submerged / (collisionSize.y * 2f));
// planing: upward force from horizontal speed, only if not descending fast
if (velocity.y > -4f) force += 3f * frac * mass * horizontalSpeed * Vector3.up;
// drag and buoyancy
force += frac * -velocity * mass * 5f + 9.81f * buoyancy * mass * frac * Vector3.up;
// and then:
wingEffectiveness = Mathf.Lerp(e, 0.01f, frac);
airDensity        = Mathf.Lerp(ρ, 1000f, frac);
```

The part then falls through **the same lift and drag code**, now at ρ = 1000 with
its wing switched off, so a submerged fuselage gets enormous form drag from its
`dragArea` term automatically. Buoyancy decays while submerged
(`buoyancy -= 0.2 * frac * dt`) — the part fills with water. The total force is
clamped to `|v| * 0.5 * mass * 60` to stop the impulse exploding on a fast
ditching, and below −100 m there is a flat 20 m/s² upward shove as an escape
hatch.

**[PATCH]** Update 0.34 added *"Glancing collisions with water at high speed will
sever aircraft parts"* — the `ApplyJobFields` branch that zeroes every joint's
break force when the part splashes above 83 m/s.

**[inferred]** A very good trade. Water is not a separate system with its own
solver; it is four extra lines in a loop that was already running, and the one
genuinely water-specific behaviour (planing) is a single term. Note that this is
a *different* water model from the ships' (§11) — aircraft ditching is handled
by the aero job, ships by a dedicated buoyancy job, and the two never meet.

---

## 4. Control surfaces: the deflection *is* the model

**[CODE]** This is the part I would most want a reader to take away, because it
is where most games take the shortcut and this one does not.

`ControlSurface` is a `MonoBehaviour` that owns ranges, a servo speed, and a
`visibleMesh` transform. Its job does **nothing but produce a quaternion**:

```csharp
float value = controlInputs.pitch * fields.pitchRange - fields.currentPitch;
fields.currentPitch += Mathf.Clamp(value, -servoSpeed * dt, servoSpeed * dt);
// ... same for roll and yaw ...
float angle = currentPitch + currentRoll + currentYaw;
mainRotation = restingRotation * Quaternion.AngleAxis(angle, Vector3.right);
```

Three axes summed onto one hinge — so an elevon is a surface with both a
`pitchRange` and a `rollRange`, and a ruddervator has `pitchRange` and
`yawRange`. No special-casing per configuration. The rate limit (`servoSpeed`,
default 20°/s) is a real actuator: **[PATCH]** 0.28.6 lists *"Synchronized Ifrit
flap and aileron servo speeds to improve roll consistency"*, a bug that can only
exist if servo rate is genuinely in the loop.

**The quaternion is then written to the transform, and the aero job reads that
transform.** The dependency chain is explicit in `AeroJobSettings.Schedule`:

```
ControlSurfaceJob_Math   (IJobParallelFor, batch 32)      →  deflection angles
        ↓ handleMath
SetLocalRotationJob      (IJobParallelForTransform)       →  write surface transforms
        ↓ handleAccess ───────────────┐
                                       │  passed as the aero read's dependency
ReadTransformJob         (ScheduleReadOnly, batch 64)     →  world pos/rot of lift normals
        ↓ handleAccess
AeroJob_Math             (IJobParallelFor, batch 64)      →  forces and torques
        ↓
main thread: rb.AddForce / rb.AddTorque
```

`new ReadTransformJob(...).ScheduleReadOnly(transformAccess, 64, controlJob.handleAccess)`
— the aero job's transform read is scheduled **as a dependent of the control
job's transform write**. That single argument is the whole architecture.

**[inferred] The consequence is that control authority is emergent.** There is no
"pitch input × 12,000 N·m" anywhere. Pull the stick and:

- the elevator's transform rotates by up to `pitchRange` at `servoSpeed`;
- next the aero job resolves the airflow **in the rotated frame**, so its α jumps;
- its Cl comes off the same 128-entry table as the wing's;
- the force is applied at the tail, behind the CoM, and the aircraft pitches.

Which means, without a line of code written for any of them: control authority
falls off with dynamic pressure; the elevator **stalls** if you deflect it too
far at high α, because the table says so; a damaged elevator with reduced
`wingEffectiveness` is less effective in exactly the right proportion; and a
surface whose part has separated contributes nothing because the job checks
`fields.IsDetached` and returns early.

Two extras hang off the same class:

- **Split surfaces** — `maxSplit`, `splitUpper`/`splitLower`, `yawSplitFactor`.
  Deflected apart when throttle is zero (a speed brake) and differentially with
  yaw (a drag rudder for tailless aircraft). Their drag *is* modelled explicitly
  and not geometrically: `ControlSurface.Aero()` runs on the main thread and does
  `-splitAmount * splitDrag * ρ * v²` along the velocity vector. **[inferred]** A
  deliberate exception to §2, and the right one: modelling the two clamshell
  halves as real `AeroPart`s would double the part count for a term that is pure
  drag.
- **Flaps** — `flap = true` swaps the input for the gear state, so flaps track the
  gear lever at servo speed instead of the stick.

### 4.1 The other control-surface implementation

**[CODE]** `ControlSurfacePhysics` is a second, entirely different class for the
same job, used when the surface is its **own rigidbody**:

```csharp
float target = pitch*pitchRange + yaw*yawRange + roll*rollRange;
currentAngle += Mathf.Clamp(target - currentAngle, -servoSpeed*dt, servoSpeed*dt);
part.SetHingeJoint(i, connectedParts[i], spring, damp, currentAngle, breakStrength, ...);
```

A real Unity `HingeJoint` with a spring driven toward the commanded angle. The
surface is not *placed* at the commanded deflection — it is *pushed* toward it
against the air load, and the joint has a `breakStrength`. **[inferred]** So a
surface can be blown back by aerodynamic load at high q, and can be torn off.
The same servo command expressed as a constraint rather than a kinematic write,
and which one you get depends on the physics LOD in §5.

---

## 5. The airframe is a physics assembly, and it has two levels of detail

**[STORE]** *"each aircraft is composed of up to 50 individually simulated
physics parts"*, with 30–50 detachable parts.

**[CODE]** The interesting part is that "individually simulated" is a *runtime
state*, not a build-time fact. Every `AeroPart` can be in one of two modes:

**Simple physics** (`simplePhysics = true`, the default): the part has no
`Rigidbody` of its own; `rb` points at the parent aircraft's rigidbody. All parts
add their forces to one body. The transform hierarchy is intact.

**Complex physics**: `CreateRB()` unparents the part, gives it its own
`Rigidbody` with `mass`, zero drag, zero angular drag and `sleepThreshold = 0`;
then `CreateJoints()` adds a `FixedJoint` per entry in `joints[]` with
`breakForce`/`breakTorque` scaled ×10, and bumps `solverIterations` where the
designer asked for it.

The switch:

```csharp
private void CheckPhysicsLod() {
    bool near = FastMath.InRange(camera.position, transform.position, 10000f);
    if (simplePhysics) { if (gForce < 2f && near)  SetComplexPhysics(); }
    else               { if (gForce < 2f && !near) SetSimplePhysics();  }
}
```

**[inferred] Both conditions matter and the second one is the clever one.**
Distance is the obvious LOD axis — a jet 12 km away does not need 40 rigidbodies
and 40 joints. The **`gForce < 2f` guard is a transition-safety condition**: the
state change re-derives mass distribution, resets the centre of mass and the
inertia tensor, and hands the solver a completely different constraint problem.
Do that mid-8g-turn and the aircraft visibly jolts. So the switch waits for a
quiet moment, and if the aircraft never has one it simply stays in whichever mode
it is in. That is a pattern worth naming: **an LOD transition gated on the
dynamics being calm, not only on the metric crossing a threshold.**

`SetComplexPhysics` also promotes the aircraft to
`CollisionDetectionMode.Continuous`, but **only for the local player or when
debug visualisation is on** — continuous collision is bought exactly where a
missed terrain contact would be noticed.

**[PATCH]** The corroboration that this is real and not vestigial is a bug report
in the release notes: 0.28.6, *"Increased rigidity of Ifrit wings to reduce
flapping"*. Wings that flap are wings on spring-loaded joints. The
multi-rigidbody airframe is doing structural dynamics whether or not that was the
intent, and the fix was a joint stiffness value.

### 5.1 Damage, and how it reaches the flight model

**[CODE]** Damage is typed — pierce, blast, fire, impact — and each type is
reduced by that part's `ArmorProperties` (`pierceArmor`/`pierceTolerance` etc.)
before being subtracted from 100 hit points. `AeroPart.ApplyDamage` then does
three things:

1. `wingEffectiveness = Lerp(0.5f, 1f, hitPoints * 0.01f)` — §3.3.
2. Rescales **every joint's break force** by
   `max((hitPoints - structuralThreshold) / (100 - structuralThreshold), 0)`, so a
   damaged wing separates at a lower g load rather than at a fixed hit-point count.
3. Recomputes `attachInfo.attachmentStrength` as the sum of the surviving joints'
   strengths.

Separation itself is not a joint event but a **position** test, run round-robin:

```csharp
private class PartChecker {
    public void Check() {
        if (parts.Count == 0) return;
        i++; if (i >= parts.Count) i = 0;
        parts[i].CheckAttachment();          // ONE part per FixedUpdate
    }
}

// CheckAttachment:
Vector3 b = parent.xform.InverseTransformPoint(xform.position);
if (FastMath.OutOfRange(attachInfo.localPosition, b, 0.5f)) {
    if (streamlining > 0) parent.ModifyDrag(streamlining);
    parentUnit.DetachPart(...);
}
```

**[inferred] Two things here are worth stealing.** First, the detachment
criterion is *"this part has physically moved more than 50 cm from where it is
supposed to be"* — which catches joint breaks, solver blowups and geometry that
has been pulled apart, all with one test and no callback plumbing. Second, **it
is amortised one part per fixed step**. At 50 Hz with 40 parts, each part is
checked every 0.8 s. A structural failure taking up to 0.8 s to be noticed is
invisible; running 40 `InverseTransformPoint`s per aircraft per tick would not
be. That is exactly the CLAUDE.md question — *how many times does this actually
run?* — answered by changing how often it runs at all rather than by making it
faster.

---

## 6. The fly-by-wire, layer by layer

**[CODE]** `ControlsFilter` is 974 lines and is where the aircraft's *character*
is authored — the only place per-aircraft numbers live in quantity. It is a
`MonoBehaviour` with three serialised sub-objects (`FlyByWire`, `AutoHover`,
`AimAssist`) and a `virtual Filter` that `HeloControlsFilter` overrides.

The pipeline, from `Aircraft.FilterInputs()`:

```
raw stick  →  RelaxedStabilityController (if fitted)
           →  ControlsFilter.Filter
                 ├─ AutoHover      (if enabled and active)
                 ├─ AimAssist      (if flight assist on)
                 └─ FlyByWire
           →  controlInputs  →  ControlSurface job  →  transforms  →  aero job
```

### 6.1 Dynamic pressure is the master variable

Everything in the FBW is scaled by one number:

```csharp
float q0 = cornerSpeed * cornerSpeed * 1.225f;          // reference q at corner speed
float q  = speed * speed * airDensity / q0;             // normalised dynamic pressure
remapFactor = 1f / Mathf.Max(q, 1f);
```

`remapFactor` is 1 at or below corner speed and falls as 1/q above it. It scales
the pitch PID output, the yaw loop and the roll blend. **[inferred] This is the
gain-scheduling that stops a rate-command loop from oscillating**: the same
stick-to-surface gain that feels right at 150 m/s would be violently
overcontrolled at 400 m/s, because the surface's authority went up with q while
the loop's gain did not. Dividing by q cancels it. One variable, computed once,
and it is the difference between an FBW that feels tuned and one that wobbles.

### 6.2 Pitch is a g-command, not a rate command

```csharp
targetPitchAngVel = inputs.pitch * gLimitPositive * 9.81f
                  / Mathf.Max(aircraft.speed, cornerSpeed * 0.75f);
```

ω = g/V — full aft stick asks for exactly the turn rate that produces the
aircraft's g limit at the current speed. Above corner speed you get the g limit;
below it, the command is degraded:

```csharp
if (q < 1f) {
    targetPitchAngVel *= Mathf.Clamp(q, 0.3f, 1f);
    float alphaDeg = atan2(vLocal.y, vLocal.z) * Mathf.Rad2Deg;
    if (Mathf.Abs(alphaDeg) > alphaLimiter && sign matches command) {
        targetPitchAngVel *= 1f - Mathf.Clamp(|alphaDeg| - alphaLimiter, 0, 10) * alphaLimiterStrength;
    }
}
```

An **α limiter that only exists below corner speed**, tapering the command over a
10° band past `alphaLimiter` (default 25°). Above corner speed the g limit binds
first and the α limiter is irrelevant, so it is not evaluated. **[inferred]** The
two limits are the two halves of the real envelope — the lift limit at low speed,
the structural limit at high speed — and the code switches between them on q
rather than blending both everywhere.

The loop closing on that target is a rate PID with an integrator on **stick
position**, not on surface angle:

```csharp
float err = Mathf.Clamp(localAngularVelocity.x - targetPitchAngVel, -0.25f, 0.25f);
i  = Mathf.Clamp(i + err * dt, -0.2f, 0.2f);
float d = (err - pPrev) / dt;  pPrev = err;
pitchAdjuster += Mathf.Clamp(-(err*pFactorFast + i*iFactor + d*dFactorFast) * remapFactor, -2, 2) * dt;
pitchAdjuster  = Mathf.Clamp(pitchAdjuster, -1f, 1f);
inputs.pitch   = pitchAdjuster;
```

**[inferred]** `pitchAdjuster` is an *integrating* output — the PID drives its
rate of change, not its value. That is a velocity-form controller, and it is why
the aircraft trims itself: with the stick centred the loop drives ω to zero and
whatever surface deflection achieves that is held. There is no separate pitch
trim system. Roll gets an explicit slow trim integrator instead (`rollTrim`,
±`rollTrimLimit`, only accumulating when pitch rate is near zero and the aircraft
is above 0.5 m), and yaw gets a **weathervaning** term that drives sideslip to
zero:

```csharp
float yawErr = yawTightness * remapFactor * (localAngularVelocity.y - inputs.yaw);
if (yawWeathervaning > 0f)
    yawErr -= yawWeathervaning * TargetCalc.GetAngleOnAxis(forward, rb.velocity, up);
inputs.yaw = Mathf.Clamp(-yawErr, -1f, 1f);
```

There is also a ground case: with gear down, below 0.1 m radar altitude and under
1.5× takeoff speed, the trim integrator is decayed and — if the **nose gear has
weight on the wheel** — the pitch rate command is clamped so the aircraft cannot
rotate before it is ready. `noseGear.WeightOnWheel(0.05f)` is a real squat
switch, backed by the suspension model in §9.

### 6.3 The helicopter's filter is a different law

**[CODE]** `HeloControlsFilter.HeloFlyByWire` is an **attitude-rate command with
a direct-control feedforward plus an integrating compensator** on all three axes
at once:

```csharp
float gLimitRate = gLimit * 9.81f / Mathf.Max(aircraft.speed, 10f);
Vector3 target = new Vector3(Mathf.Clamp(pitch * maxAngularVel.x, -gLimitRate, gLimitRate),
                             yaw  * maxAngularVel.y,
                            -roll * maxAngularVel.z);
Vector3 err = localAngularVelocity - target;
Vector3 d   = (err - pPrev) / dt;  pPrev = err;

// weathervane above 40 m/s, ramping to full by 60 m/s
if (speed > yawWeathervaneMinSpeed) {
    float t = Mathf.Clamp01((speed - min) / (max - min));
    err.y += TargetCalc.GetAngleOnAxis(rb.velocity, forward, up) * 0.1f * yawWeathervaneStrength * t;
}

compensator += -(Scale(err, pFactor) + Scale(d, dFactor)) * dt;   // clamped ±1
if (aircraft.radarAlt < 0.5f) compensator = Vector3.zero;         // reset on the ground
inputs.pitch = Clamp(-err.x * directControlFactor.x + compensator.x, -1, 1);
```

**[inferred]** The differences from the fixed-wing law are all correct for a
rotorcraft: the direct term (`directControlFactor`, default 0.7) matters because
a helicopter's control response is nearly instantaneous and a pure integrator
would feel mushy; the compensator is zeroed on the ground so integrator wind-up
cannot tip the aircraft on the skids; and the weathervane only engages above 40
m/s, because below that there is no airflow to weathervane into.

And the anti-torque bookkeeping is done for the pilot:

```csharp
tailRotor.SetDesiredBaseThrust(rotorShaft.GetTorque() / tailRotorDist);
```

Torque divided by moment arm is the anti-torque thrust required *right now*, from
the *measured* shaft torque. Pedal is added on top. So the pilot commands **yaw**,
not tail-rotor pitch.

### 6.4 The layers that are no longer wired up

**[CODE]** `ControlsFilter` also contains five fully implemented nested classes
that are **not serialised fields on the class** and therefore never run in the
shipped configuration: `AutoTrimmer`, `GLimiter`, `AngularVelocityDamper`,
`SpeedRemap`, `ResponseRateLimiter`, plus a standalone `AoALimiter`.

They are worth reading anyway because they are the previous generation:

- `GLimiter` — measures actual g along the body up axis, differentiates it,
  **predicts g one second ahead** (`predictionTime`), and rolls a limiter strength
  on and off at asymmetric rates (`rollonRate = 1`, `rolloffRate = 0.1`). It also
  latches `inputMagnitudeAtOverG` — once you have exceeded the limit, your stick
  range is clamped to where it was when you did, relaxing back to 1 over ~2 s.
- `AutoTrimmer` — a PD on angular rate error with a clamped trim integrator,
  zeroed by the nose-gear squat switch.
- `SpeedRemap`, `ResponseRateLimiter` — stick-authority scaling above a speed
  threshold, and a rate limit on stick movement that *tightens as the stick moves
  further from centre*.

**[PATCH]** The release notes date the transition: 0.27.3 and 0.28.5 talk about
per-aircraft *"auto-trimmer"* behaviour; 0.31.1 and 0.32 both say **"New
aerodynamics and Fly-by-wire control filters for all aircraft"**. The auto-trimmer
era ended, the `FlyByWire` class replaced it, and the old code was left in place.

**[inferred]** Two readings, and both are probably true. Charitably, these are a
**library of assists** kept so a future aircraft can be given a G-limiter without
rewriting one. Uncharitably, 400 lines of dead code sit in the class a new reader
opens first. If this project does the same thing, the fix is not to delete the
ideas but to move them somewhere that says they are not currently in the loop.

### 6.5 The named escape hatches

Two classes are honest about being cheats, and the honesty is the right call:

**[CODE]** `MagicTorqueController` — literally that name. Applies a direct
`AddRelativeTorque(pitch*gain.x, yaw*gain.y, roll*gain.z)` plus a sideslip damping
force, both scaled by `powerRatio` drawn from the aircraft's electrical
`PowerSupply`. **[inferred]** It is a reaction-control system for VTOLs in the
hover, where there is no airflow for §4 to work with, and gating it on electrical
power means losing the generator costs you hover authority — a game consequence
for a physics cheat.

`RelaxedStabilityController` — for canard/unstable airframes. Above 30 m/s it
replaces the pitch input with a **blend between commanded α and raw stick**:

```csharp
float alphaCmd = TargetCalc.GetAngleOnAxis(forward, rb.velocity, right) / canardRange;
inputs.pitch = Mathf.Lerp(alphaCmd, rawPitch, Mathf.Abs(rawPitch));
```

Small stick → the surface holds the current flow angle (α hold, i.e. artificial
stability). Full stick → direct control. And `OnEngineDisable` sets
`effectiveness = 0` — **lose the engine, lose the stability augmentation, and an
unstable airframe becomes unstable.**

---

## 7. Rotorcraft: a real blade-element rotor

**[CODE]** The deepest part of the simulation, and a genuine blade-element model
with a flapping hinge and a driveline. Three classes: `SwashRotor` (one blade),
`RotorShaft` (the hub and drivetrain), `Transmission` (the power bus).

### 7.1 One blade, N radial stations

`SwashRotor.SampleForces` walks `samples` stations along the span (default
`radialSamples = 3`) and at each one:

```csharp
float r      = length * (i + 1) * step;
Vector3 pos  = hinge.position + span.forward * r;

// blade twist (washout), linear from root to tip:
pitchTransform.localEulerAngles = new Vector3(-washout * (1f - r/originalLength), 90*dir, 0);

// local airflow = rotation + freestream + airframe motion + flapping velocity
float   uT   = angularSpeed * (r + hubRadius) * cos(flapAngle);
Vector3 vRel = uT * hinge.right * dir - airVelocity + rb.GetPointVelocity(pos);
        vRel += r * flapAngularVelocity * flapDirection;

Vector3 F    = SampleForce(liftNumber, stallAngle, dragBase, dragExponent, vRel, out alpha);
        F   += originalMass * step * -9.81f * Vector3.up;                       // blade weight
        F   += originalMass * step * uT*uT / (r + hubRadius) * hinge.forward;   // centrifugal
```

with the per-element aerodynamics being a thin-airfoil model with a smoothly
blended post-stall branch:

```csharp
float Cl = alphaDeg * liftNumber;
if (|alphaDeg| > stallAngle) {
    float postStall = Mathf.Sin(2f*alphaRad) * (liftNumber * stallAngle);
    Cl = Mathf.SmoothStep(Cl, postStall, (|alphaDeg| - stallAngle) * 0.1f);   // 10° blend band
}
float Cd = dragBase + dragExponent * (1f - Mathf.Cos(2f*alphaRad));
```

**[inferred]** Analytic rather than a table, unlike the fixed wing. The reason is
that the fixed wing needs the *full ±180° circle* to survive spins, whereas a
rotor blade lives near its stall angle and needs a smooth, well-behaved
derivative there — an analytic `SmoothStep` blend has no table quantisation to
produce chatter in a loop that runs at 12× the physics rate.

Forces are accumulated three ways: as a total force, as a torque about the hub
(`Cross(F, hubPos - stationPos)`), and as a **flapping moment**
(`Dot(F, flapDir) * r`).

### 7.2 Flapping is a real integrated degree of freedom

```csharp
M  /= stiffnessMultiplier;
float k = (flapAngle < 0f) ? flapSpringDown : flapSpringUp;   // asymmetric droop stop
if (Mathf.Abs(flapAngle) > 0.5f) k *= 5f;                      // hitting the stops
M += k * -flapAngle;
M += -flapAngularVelocity * flapDamp;

flapAngularVelocity += dt * M / momentOfInertia;               // I = (1/3)mL²
flapAngle           += dt * flapAngularVelocity;
span.localEulerAngles.x = -flapAngle * Mathf.Rad2Deg;          // and the mesh follows
```

Then the flapping *reaction* is fed back into the hub force and torque
(`force -= M/(0.7L) * flapDir`), so hub moment and blade flap are coupled.

**[inferred] This is the single most consequential decision in the helicopter
model.** Blade flapping is what makes a helicopter behave like a helicopter:
dissymmetry of lift in forward flight is resolved by the advancing blade flapping
up and the retreating blade flapping down, which *is* the flapback that makes the
disc tilt back as you accelerate, which is why cyclic and collective are coupled,
why a helicopter pitches up as it gains speed, and where retreating blade stall
comes from. None of those are coded anywhere. They are consequences of
integrating β̈ = M/I with the right M.

The asymmetric spring (`flapSpringUp` vs `flapSpringDown`, ×5 past 0.5 rad) is the
droop stop and the flap stop. `BendSegments` even droops the visual mesh when the
rotor is slow and the blades are hanging — a cosmetic use of the same state.

### 7.3 Cyclic and collective through an actual swashplate

```csharp
// SwashRotor.SetPitch, per blade, per sub-step:
float a = Vector3.Dot(hinge.forward,  swashPlate.forward);
float b = Vector3.Dot(hinge.forward, -swashPlate.right);
float pitch = a * -cyclicTravel * swashPlatePitch
            + b *  cyclicTravel * swashPlateRoll
            + collective * collectiveTravel;
span.localEulerAngles.z = pitch * directionMult;
```

Blade pitch is the **projection of the blade's current azimuth onto the tilted
swashplate**. That is mechanically what a swashplate does, and it means phase lag
is a `swashPlate.localEulerAngles.y = phaseLag` on a transform rather than a
correction term in a formula. Rotate that transform 90° and you have correctly
modelled the 90° phase lag of a real rotor; the designer dials it per aircraft.

Collective comes from **throttle**, with a yaw contribution
(`throttle + yaw * yawCollective`) so pedal on a helicopter without a tail rotor
(NOTAR, coaxial) still does something. The swashplate inputs are all rate-limited
by `maxInputRate` (default 20/s) and collective is clamped to
`[collectiveMin, 1.1]`.

### 7.4 Azimuthal sub-stepping

```csharp
private const int frameSamples = 4;   // serialised, default 4
float subDt = Time.fixedDeltaTime / frameSamples;
for (int i = 0; i < frameSamples; i++) {
    hubRotator.Rotate(0, angularSpeed * dir * Rad2Deg * subDt, 0);
    foreach (var blade in rotors) {
        blade.SetPitch(...);
        forceAndTorque.Add(blade.SampleForces(rb, angularSpeed, ..., subDt, out alpha));
    }
}
Vector3 force  = forceAndTorque.force  / frameSamples;   // averaged
Vector3 torque = forceAndTorque.torque / frameSamples;
```

The hub is physically advanced **four times per FixedUpdate** and the forces
averaged. **[inferred] This is necessary, not luxury.** A 280 rpm rotor turns ~34°
per 50 Hz tick. With two blades and no sub-stepping the disc would be sampled at
4 discrete azimuths per revolution and the model would alias into a periodic
force that shakes the airframe at a beat frequency. Four sub-steps takes it to
~8.4° per sample. Total cost: `blades × frameSamples × radialSamples` =
2 × 4 × 3 = **24 blade-element evaluations per rotor per tick**, which is nothing.

Note the ordering trick: `angularPosition` is advanced by the *full* step first
and written to the hub, then the sub-step loop rotates from there. The rotor's
visual position is exact; the sub-steps are a sampling detail that does not drift.

### 7.5 The drivetrain, and why autorotation needs no code

```csharp
// RotorShaft.FixedUpdate
torqueFromEngine = Mathf.Min(condition * availablePower / Mathf.Max(angularSpeed, 1f), torqueLimit);
if (disengaged || !unfolded) torqueFromEngine = 0f;

float net = torqueFromEngine + torqueFromRotors;          // ← aerodynamic torque, signed
net -= shaftFriction * Mathf.Clamp(angularSpeed * 20f, -1f, 1f);
angularSpeed += net * dt / momentOfInertia;               // I = (1/3) N m (L+r)²

rb.AddTorque(torqueFromEngine * dir * -transform.up);     // reaction torque on the airframe

// governor: ask the transmission for power proportional to RPM droop
float droop = (angularSpeedNominal - angularSpeed) / angularSpeedNominal;
transmission.RequestPower(this, Mathf.Clamp(nominalPower * (1f + droop * 30f), 0f, nominalPower * 1.1f));
```

**[inferred] Read what falls out of those five lines.**

- **Autorotation is not implemented.** `torqueFromRotors` is the sum of the blade
  elements' torque about the shaft. In powered flight it is negative (drag). In a
  descent through rising air at low collective it goes **positive** — the blades
  are driven by the upflow — and `angularSpeed` is sustained with
  `torqueFromEngine = 0`. Flare, and the extra inflow spins the rotor up and buys
  thrust. That is autorotation, and it exists because nobody wrote a special case.
- **Torque reaction is not implemented either.** The airframe gets
  `-torqueFromEngine` about the mast, so it yaws, so you need the tail rotor, so a
  tail rotor failure spins you.
- **Rotor inertia is a real number** with a real consequence: a heavy rotor droops
  slowly under a sudden collective pull and stores energy for the flare.
- **The governor is a proportional controller with a gain of 30**, requesting up to
  110% power, and it is *requesting* rather than commanding, which matters next.

`RotorShaft` also adds **gyroscopic precession explicitly**, because Unity's
rigidbody knows nothing about the spinning mass:

```csharp
Vector3 ω = rb.transform.InverseTransformDirection(rb.angularVelocity);
torque += angularSpeed * 0.5f * momentOfInertia * (-ω.x * right - ω.z * forward);
```

Pitch the airframe, get a roll moment. Real, felt, one line.

Startup is staged: `startupProgress` ramps the available torque limit in from
zero once the turbine is producing >10% power, slowly below 90% RPM (0.03/s) and
faster above it (0.2/s), which gives the correct slow-then-quick rotor spin-up.
Rotors spawned airborne skip it (`angularSpeed = angularSpeedLimit * 1.1f`).

### 7.6 The transmission is a power bus

**[CODE]** `Transmission` collects `RequestPower` calls from every consumer during
the tick, sums them, throttles the sources toward `totalRequested / maxOutput`
through a `SmoothDamp` (`governorSmoothing`), then distributes what actually
arrived **proportionally**:

```csharp
float ratio = availablePower / Mathf.Max(totalPowerRequested, 1f);
for each output: output.SendPower(request[j] * ratio);
```

**[inferred] A small class with a large payoff.** Multi-engine helicopters,
one-engine-inoperative behaviour, a tail rotor and a main rotor competing for a
shared turbine, and a tiltrotor's cross-shaft all fall out of "everyone asks,
supply is divided pro rata". Kill one of two turboshafts and the main rotor gets
50% of what it asked for, RPM droops, the governor asks for more, the remaining
engine goes to its limit — all emergent. `TurbineEngine` even derates with air
density (`currentPower *= airDensity / 1.225f`), so hot-and-high performance loss
is free.

### 7.7 Induced flow, ground effect and vortex ring state

**[CODE]** All three live in `RotorPhysics`, as modifications to the air velocity
the blades see:

```csharp
Vector3 air = wind;
air -= downdraft   * transform.up;                 // the rotor's own induced flow
air -= VRSSmoothed * VRSStrength * transform.up;   // recirculation

float translational = |Dot(forward, v)| + |Dot(right, v)|;
downdraft = Lerp(downdraft, currentThrust * 0.04f / (rotorDiskArea * (1f + translational*0.1f)), dt);
downdraft *= Mathf.Clamp01(1f - groundEffect * 0.15f);

float vrs = Mathf.Clamp01(Mathf.Min(Dot(v, -up), 10f) - (VRSThreshold + translational * 0.4f));
VRSSmoothed = Mathf.Lerp(VRSSmoothed, vrs, 0.25f * dt);
if (VRSSmoothed > 0.1f) aircraft.ShakeAircraft(VRSSmoothed * 0.02f, 0f);
```

**[inferred]** The structure is right in all three cases:

- **Induced velocity is thrust over disc area**, i.e. momentum theory, lagged with
  a ~1 s time constant, and **reduced by forward speed** — which is translational
  lift, and it is why the aircraft climbs when it accelerates through ~15 kt
  without anyone writing "translational lift".
- **VRS is modelled as its actual cause**: descending into your own downwash adds
  *more* downward inflow at the disc, which reduces blade α, which reduces thrust,
  which increases the descent rate. A positive feedback loop through the blade
  elements, not a scripted "if descending fast, lose lift". It needs a descent
  rate past `VRSThreshold` (default 4 m/s) and is **suppressed by translation**
  (`+ 0.4 × translational`), so the escape is to fly forward, which is the real
  escape. The 4 s smoothing time constant means it builds and clears gradually.
- **Ground effect reduces the induced flow rather than adding lift**, which is
  again the actual mechanism.

**[CODE] One observation, offered as read.** The ground-effect term is:

```csharp
groundEffect = Mathf.Lerp(groundEffect, Mathf.Clamp01(bladeLength / (aircraft.radarAlt * bladeLength)), dt);
```

`bladeLength` cancels, so this reduces to `clamp01(1 / radarAlt)` — full strength
below 1 m, negligible by 3 m. Combined with the 15% cap on its effect, ground
effect in this build is essentially inert above skid height. Given the shape of
everything around it, that reads like an algebraic slip in a term meant to be
scaled by rotor radius (ground effect is conventionally significant within about
one rotor diameter). I have not run the game, so this is a **[CODE]** reading of
the expression, not a claim about how it feels.

**[inferred]** Worth flagging for its own sake: this is the failure mode of an
emergent model. When behaviour is a consequence rather than a rule, a wrong
constant does not throw an error or produce an obviously broken value — it
quietly removes a phenomenon, and no test catches it because there is nothing to
assert against. A model like this needs a **debug visualisation of the derived
quantities** (this one has `PlayerSettings.debugVis`, drawing per-station velocity
and lift arrows and the flap vector) far more than a conventional one does.

### 7.8 Rotor strikes

```csharp
bool hitSolid = Physics.Linecast(span.position, span.position + span.forward * length, out hit, ~mask);
bool hitWater = Datum.WaterPlane().Raycast(new Ray(span.position, span.forward), out enter) && enter < length;
```

One linecast **per blade per tick** along the current span, plus an analytic plane
test for water. On a hit: `RotorStrike` removes angular momentum
(`angularSpeed -= sign * impactTorque / momentOfInertia`), halves `condition`,
plays a strike effect reflected off the surface normal, and damages the blade by
the fraction of span inside the obstacle. Blade damage shortens `length`, and
`BreakSegments` detaches the outboard mesh segments beyond the new length as free
rigidbodies with a tip-speed velocity. **[PATCH]** 0.29 added *"individual blade
damage model for turboprops"*.

**[inferred]** Cheap, correct enough, and it exists because the blade positions
are already real transforms — another dividend of §2.

### 7.9 The experimental one: `SoftBodyRotor`

**[CODE]** A fourth rotor class, structurally different from `SwashRotor`: the
blade is a **lumped mass-spring chain**.

```csharp
private class MassPoint {
    public float mass; public Vector3 position; private Vector3 velocity;
    public bool anchoredToTransform;                       // hub attachment
    public void Simulate(float dt, out ForceAndTorque ft) {
        AddForce(mass * -9.81f * Vector3.up);
        if (!anchoredToTransform) { velocity += frameForces * dt / mass; position += velocity * dt; }
        else { MoveAnchoredTransform(dt); ft = new ForceAndTorque(frameForces, position - anchor.position); }
    }
}
private class Segment {                                     // spring + damper between two points
    public void Simulate(float dt) {
        Vector3 n = (end.position - start.position).normalized;
        float len = Distance(start.position, end.position);
        float f = (len - restLength) * spring + ((len - lengthPrev)/dt) * damp;
        start.AddForce(n * f);  end.AddForce(-n * f);
    }
}
```

Anchored points are driven by the transform and report their reaction force back
to the hub as a `ForceAndTorque`; free points integrate semi-implicit Euler. Lift
is sampled at three stations (`r = 0.35, 0.65, 0.95` of span) with a slightly
different post-stall law (linear to `stallAngle`, then lerping to zero over 25°)
and a `1 - vrsFactor` multiplier applied directly to lift.

**[inferred] This reads as unfinished.** Three tells: the `MassPoint` and
`Segment` constructors instantiate debug geometry (`GameAssets.i.debugPoint`,
`debugArrowGreen`) with **no `PlayerSettings.debugVis` guard**; `CheckCollisions`
computes its hit and then ends in a run of discarded expressions
(`_ = tipVelocity.magnitude; _ = length; _ = rotorHit.distance;`) with no damage
response; and the class duplicates `SwashRotor`'s job rather than extending it.
It is worth reading as *the more physical rotor someone started*, and worth not
copying as-is. A flexible blade is the right answer if you want coning, lead-lag
and blade-stall flutter to emerge; it is a much harder stability problem than a
single hinge integrator, and `SwashRotor` is the one that shipped.

---

## 8. Propellers, ducted fans and VTOL

### 8.1 Propellers get the blade-element treatment too

**[CODE]** `ConstantSpeedProp` (769 lines) runs a `PropBlade` per blade with the
same structure as the rotor: per-blade force and torque into a shared
`ForceAndTorque`, hub torque summed back onto the shaft, and

```csharp
engineTorque = powerAvailable * 9.5488f / Mathf.Max(RPM, 1f);   // 9.5488 = 60/2π
engineTorque = Mathf.Min(engineTorque, Mathf.Clamp(rpmRatio*5f, 0.3f, 1f) * propTorqueLimit);
angularVelocity += dt * (engineTorque + hubTorque) / momentOfInertia;
```

Plus a **constant-speed governor** (`AutoPropPitch`) varying blade pitch to hold
RPM, feathering on power loss (`featherIfPowerLost`), a throttle boost
proportional to RPM droop (`throttle += (1 - rpmRatio) * 5f`), a water-strike
check when the hub is within a blade length of the sea, and — the detail I liked
most — an **imbalance model**:

```csharp
if (imbalance > 0f) {
    imbalanceApplicationAngle += min(angularVelocity * dt, 2π/3);
    Vector3 dir = right * sin(angle) + up * cos(angle);
    forceAndTorque.Add(new ForceAndTorque(Σ centripetalForce.magnitude * dir, Vector3.zero));
}
```

Lose a blade and the hub gets a **rotating force vector** at shaft frequency, of
the magnitude of the missing blade's centripetal load. That is real rotor
imbalance, and it shakes the aircraft apart in the right way.

`PropFan` is the simpler sibling — momentum theory (§8.2) multiplied by an
`efficiencyCurve.Evaluate(airspeed)`, with RPM spooling toward nominal and
decaying as `sqrt(rpmRatio)` when power is lost.

### 8.2 Ducted fans are momentum theory, in both directions

**[CODE]** `DuctedFan` inverts the actuator-disc relation to convert between
thrust and power:

```csharp
maxThrust       = Mathf.Pow(2.4f * area * (nominalPower * nominalPower), 0.3333f);
availableThrust = Mathf.Pow(2f * airDensity * area * (availablePower * availablePower), 0.3333f);
// and, going the other way, the power needed for a demanded thrust:
float powerNeeded = Mathf.Sqrt(T*T*T / (2f * airDensity * area));
```

i.e. `T = (2ρA P²)^⅓` and `P = T^{3/2} / √(2ρA)` — textbook ideal actuator disc.
**[inferred]** The payoff is that **hover performance degrades correctly with
altitude and temperature for free**: ρ is in the expression, so a VTOL that hovers
at sea level cannot hover at 4,000 m, without a table, a curve or a rule. Same
argument as the airfoil table — put the physical relation in and the special cases
stop being needed.

Thrust is applied with
`AddForceAtPosition(thrustVector.forward * thrust * rpm²/maxRPM², thrustVector.position)`,
so **which way the nozzle points is a transform**, and tilting it is animation.
§2 again. A fan driven by a `RotorShaft` (a helicopter tail rotor) instead takes
its RPM from the shaft through a `gearing` ratio and requests the power its
demanded thrust implies, so tail-rotor demand loads the main transmission.

### 8.3 Jets

**[CODE]** `Turbofan` and `Turbojet` share a structure:

```csharp
targetRPM   = Lerp(minRPM, minRPM + (maxRPM - minRPM) * (condition*0.5f + 0.5f), throttleRemapped);
rpm        += Clamp(targetRPM - rpm, -spoolRate, spoolRate) * dt;      // startupRate below minRPM
spoolRatio  = Clamp01((rpm - minRPM*0.85f) / (maxRPM - minRPM));
thrust      = max((staticThrust * altitudeFactor * speedFactor - parasiticLoss) * spoolRatio, 0);
```

with `altitudeThrust` and `speedThrust` as authored `AnimationCurve`s evaluated on
a 1 Hz `SlowUpdate` and `SmoothDamp`ed — **[inferred]** a nice touch, since ram
and altitude effects change on a timescale of seconds, so sampling them at 50 Hz
would be waste. `Turbojet` additionally applies a hard top-speed penalty
(`thrust *= max(1 - 5*(speed - maxSpeed)/maxSpeed, 0)`).

Thrust vectoring is a transform rotation with a 70°/s rate limit, clamped ±20°,
mixed from pitch and roll into `nozzleAngles.x` and yaw into `.y`, disabled below
3 m radar altitude (so it does not fight the ground) and above
`thrustVectoringMaxAirspeed`. `splitThrustFactor` lets yaw drive asymmetric
throttle on multi-engine aircraft.

Afterburner lives in `JetNozzle`: a smoothed `afterburnerAmount` ramping over a
narrow throttle band (`throttleStart = 99.8`), adding thrust, fuel flow, IR
intensity, nozzle glow and flame scale. An engine below sea level is killed
outright; an engine above `minDensity` altitude flames out.

### 8.4 The lift fan solves a moment balance every tick

**[CODE]** The best single piece of VTOL code in the assembly, from
`SwivelDuctSystem.LiftFan.Update`:

```csharp
Vector3 com        = parentUnit.transform.TransformPoint(centerOfMass);
Vector3 jetForce   = jet.GetThrust() * nozzleThrustTransform.forward;
Vector3 jetTorque  = transform.InverseTransformVector(Cross(jetForce, -(nozzlePos - com)));
Vector3 fanTorque  = transform.InverseTransformVector(Cross(thrustTransform.forward, -(fanPos - com)));
float   fanThrust  = -jetTorque.x / fanTorque.x;            // ← the balance
        fanThrust += doorsOpenAmount * pitchInput * pitchFactor * jet.GetRPMRatio();
part.rb.AddForceAtPosition(Mathf.Max(fanThrust, 0f) * thrustTransform.forward, thrustTransform.position);
jet.SetParasiticLoss(parasiticLoss * swivelAmount);
```

**[inferred]** It computes the pitching moment the swivelled rear nozzle is
producing about the **current** centre of mass, divides by the moment the lift fan
produces per newton, and commands exactly the fan thrust that cancels it — then
adds the pilot's pitch input on top as a deliberate imbalance. The CoM is
re-read, so as fuel burns and stores drop the balance follows. And the fan's shaft
power is charged back to the engine as `parasiticLoss`, so hovering costs forward
thrust.

That is how the aircraft it models actually works, and it is six lines. The
alternative — authoring a fan thrust schedule against nozzle angle — would be
wrong the moment a wing pylon was jettisoned.

### 8.5 High-lift devices change the wing area

**[CODE]** `HighLiftDevice` deploys on **speed and on angle of attack**:

```csharp
float target = Clamp01(1f - max(speed - speedDeployed, 0) / (speedRetracted - speedDeployed));
if (swingWing != null && swingWing.GetSwingPosition() > 0f) target = 0f;   // no slats when swept
if (alphaFactor > 0f) {
    float alphaDeg = atan2(vLocal.y, vLocal.z) * -Rad2Deg;
    target += Clamp01(|alphaDeg| - alphaMin) * alphaFactor;                 // automatic slats
}
position += Clamp(target - position, -dt, dt);                              // 1.0 /s travel
aeroPart.SetWingArea(Lerp(partAreaRetracted, partAreaDeployed, position));
```

**[inferred]** Automatic alpha-driven slats, and the aerodynamic effect is *purely*
"the wing area is now bigger". No Cl offset, no camber term. Combined with the
airfoil table that already covers high α, that is enough to move the stall and
the drag in the right directions.

`Airbrake` is the counter-example — explicit drag, `dragAmount * ρ * v²` along the
velocity vector, because a speed brake makes drag and nothing else.

### 8.6 Swing wings, tiltwings, swivel ducts, RCS

**[CODE]** All of them animate transforms and let §2 and §3.4 do the aerodynamics.

- **`SwingWingController`** rotates the wing transforms and **locks the outer
  control surfaces** once swept (`SetLocked(true)` makes `UpdateJobFields` feed
  zeros, so the surface centres and stays there), which is what a real swing-wing
  does — reverting to spoilers for roll. Every aerodynamic consequence of sweeping
  (less area normal to the flow, aerodynamic centre moving aft, higher stall
  speed, less transonic drag) is a consequence of the transforms having moved.
- **`TiltWingController`** computes an automatic tilt schedule from a
  `tiltAtSpeed` curve, attitude and throttle, rate-limits it, and animates the
  joints. **[PATCH]** 0.34: *"Smoother take off transition for ducted thrust and
  swivel duct VTOLs"* — the transition is tuned in the *controller*, not the
  physics, which is the tell that the physics handles it and the scheduling did
  not.
- **`DuctedThrustSystem`** swivels the whole nozzle assembly with a 40°/s rate
  limit, forces at least 45° of deflection when airborne below
  `minSpeedForForward`, and runs a small mode state machine (Manual / Forward /
  hover) with input-gesture detection to decide when to auto-transition.
- **`ReactionControlSystem`** blends the three axes into a per-thruster demand
  (`pitch*axis.x + yaw*axis.y + roll*axis.z`, remapped to 0..1) and scales the
  whole thing by `turbofan.GetThrustRatio()` — **the RCS is bleed air, so it dies
  with the engine** — and is disabled above `maxSpeed` (default 100 m/s).
- **`Repulsorlift`** is the sci-fi one: projectors casting downward, drawing from
  the electrical `PowerSupply`, with `powerRatio` feeding back as an authority
  scale.

---

## 9. Landing gear and ground handling

**[CODE]** `LandingGear` is 727 lines and does **not** use Unity's `WheelCollider`.
It is a hand-written raycast suspension, and it is a more complete tyre model than
most driving games ship.

### 9.1 Suspension

```csharp
Physics.Linecast(castPoint.position, castPoint.position - castPoint.up * suspensionTravel, out hit, mask);
Vector3 pointVel   = attachedPart.rb.GetPointVelocity(hit.point);
compressionDistance = max(suspensionTravel - (hit.distance + groundDepth), 0);
dampingForce        = -Dot(hit.normal, pointVel) * dampingRate;
compressionForce    = springRate * compressionDistance;
Vector3 normalForce = hit.normal * max(compressionForce + dampingForce, 0f);
```

`WeightOnWheel(threshold)` is then just
`compressionDistance > suspensionTravel * threshold` — a genuine squat switch,
which is what §6.2's takeoff logic and the FBW's trim reset are reading.

### 9.2 Soft ground

```csharp
float pressure = (compressionForce + dampingForce) / (contactArea * 10000f);
float soil     = 1f + (4f + sin(gp.x*0.8f) + cos(gp.z*0.37f) + sin(gp.z*0.8f) + cos(gp.z*0.37f));
groundDepth    = Lerp(groundDepth, max(pressure * 0.003f / soil, 0f),
                      3f * dt / Clamp(wheelSpeed * 0.1f, 1f, 3f));
Vector3 bogging = 2.5f * groundDepth² * |wheelSpeed| * pressure² * -pointVel.normalized;
```

**[inferred]** Off a paved surface, the wheel **sinks** by an amount driven by
contact pressure and a positional noise field standing in for soil variation, and
that sinkage both extends the suspension compression *and* generates a resistive
force that grows as the square of both depth and pressure. So a heavy aircraft on
soft ground bogs, decelerates, and — if `bogging.sqrMagnitude > springRate²` —
**breaks the gear**. Landing on grass is a different experience from landing on a
runway, and it costs about six lines.

### 9.3 Tyre forces, including static stiction

Two regimes, and the low-speed one is the good bit:

```csharp
if (|groundSpeed| < 1f) {
    // remember a point on the surface; generate a restoring force toward it
    if (FastMath.OutOfRange(hit.point, contactCollider.transform.TransformPoint(contactPatch), contactPatchSize))
        contactPatch = contactCollider.transform.InverseTransformPoint(hit.point);
    Vector3 stretch = (contactCollider.transform.TransformPoint(contactPatch) - hit.point) / contactPatchSize
                    - pointVel;
    lateral  = Project(stretch, axle.right);
    longitud = Project(stretch, -sign(wheelSpeed) * axle.forward) * (brakeStrength + 0.05f);
    force    = ClampMagnitude(lateral + longitud, 1f) * (compressionForce + dampingForce) * frictionCoef;
} else {
    float slip = Clamp(GetAngleOnAxis(axle.forward * sign(wheelSpeed), travelDir, hit.normal), -10f, 10f);
    Vector3 cornering = Clamp(slip * response * (0.2f + |wheelSpeed| * 0.01f), -1, 1) * μN * lateralDir;
    Vector3 scrub     = Clamp01(|Dot(axle.forward, lateralDir)|) * μN * -travelDir;
    Vector3 braking   = -travelDir * brakeStrength * (compressionForce + dampingForce) * frictionCoef;
    force = ClampMagnitude(cornering + scrub + braking + rollingResistance, μN);
}
```

**[inferred] The stiction model is the part worth copying.** Below 1 m/s the wheel
**anchors to a point on the surface it is touching** and pulls toward it like a
spring, re-anchoring only when it has stretched past `contactPatchSize`. That is
the standard fix for the classic bug — a parked vehicle creeping down a slope, or
jittering because a Coulomb friction force flips sign every tick — and it is
strictly better than raising the friction coefficient, which just makes the
jitter stronger. Above 1 m/s it hands over to a linear slip-angle model with a
10° clamp.

The wheel has its own spin state (`wheelSpeed`), accelerated toward ground speed
at up to 100 (units/s²), and when the slip exceeds 20 m/s it emits tyre smoke —
so a touchdown has a real spin-up transient.

### 9.4 Moving decks

```csharp
if (contactCollider.attachedRigidbody != null) {
    pointVel -= contactCollider.attachedRigidbody.GetPointVelocity(hit.point);
    var animated = contactCollider.GetComponent<AnimatedPhysicsSurface>();
    if (animated != null) pointVel -= animated.GetVelocity();
}
...
attachedPart.rb.AddForceAtPosition(total, hit.point);
if (contactRigidbody != null) contactRigidbody.AddForceAtPosition(-total, hit.point);
```

**[inferred] Three separate things here are the right call.** The wheel's velocity
is taken **relative to the surface**, so an aircraft parked on a moving carrier
does not skid. `AnimatedPhysicsSurface` is a marker component whose only job is to
report the velocity of a collider that is being *animated* rather than simulated —
a lift, a deck elevator, an opening ramp — because such a collider has no
rigidbody velocity to read and the wheel would otherwise fight it. And the
reaction force is applied back to the deck, so the physics is symmetric.

This is the same class of problem as
[`moving_frame_navigation.md`](moving_frame_navigation.md), in the physics layer
rather than the navigation layer, and the answer is the same one: **make the
moving frame's velocity explicitly queryable and subtract it.**

Gear breakage has three triggers: over-compression past `maxCompression`, the
gear hinge bending past 10°, or the soft-ground force exceeding the spring rate.
`ArrestorGear` and `TailHook` handle carrier recovery, and `OpticalLandingSystem`
is the meatball.

---

## 10. Atmosphere

**[CODE]** Two functions, and both are cheap on purpose.

```csharp
airDensityChart = new NativeArray<float>(64, Allocator.Persistent);
for (int i = 0; i < 64; i++) airDensityChart[i] = curve.Evaluate(i * 0.47619f);   // curve authored in km

public static float GetAirDensity(float altitude)
    => ChartHelper.SafeRead(altitude * 0.0021f, airDensityChart.AsReadOnlySpan());

[MethodImpl(MethodImplOptions.AggressiveInlining)]
public static float GetSpeedOfSound(float altitude)
    => Mathf.Max(-0.005f * altitude + 340f, 290f);
```

64 samples covering 0–30 km (≈476 m per step), linearly interpolated; and speed of
sound as a **clamped straight line** — 340 m/s at sea level, −0.005 per metre,
floored at 290. **[PATCH]** 0.30.9 states exactly this: *"Speed of sound now
depends on altitude (340 m/s at sea level, reducing to 290 m/s above 11,000 m)"*.
A two-segment fit to the ISA that is correct at both ends and inside a percent
through the troposphere, for a multiply, an add and a max.

**[inferred]** The pairing is the point: **density gets a table because its shape
is exponential and the designer wants to tune it; speed of sound gets a line
because its shape is a line.** Reaching for the same mechanism for both would have
been worse in one direction or the other.

Density feeds lift, drag, jet thrust, turboshaft power (`× ρ/1.225`), ducted-fan
thrust (momentum theory), engine ignition (`airDensity > minDensity` — flame out
high enough and the engine quits) and the water swap in §3.6. **One number, and
everything about high-altitude flight moves together.**

---

## 11. Ships

**[CODE]** A completely separate physics path from the aircraft, sharing only the
job infrastructure. Eight hull classes **[CODE]** (`ShipType`: CV, LHA, LFD, DDG,
FFG, FFL, LC, PB — carrier, amphibious assault, dock landing, destroyer, frigate,
corvette, landing craft, patrol boat).

### 11.1 One rigidbody, N buoyancy probes

A `Ship` is **a single `Rigidbody`**. `ShipPart`s are not separate bodies — each
one's `rb` points at the parent ship (`rb = parentUnit.rb`) — they are *sample
points* carrying a displacement, a height and a drag tensor.

```csharp
// ShipPart, serialised
[SerializeField] private float   displacement;      // m³ of water this compartment displaces
[SerializeField] private float   height;            // overwritten from the collider bounds at Awake
[SerializeField] private Vector3 directionalDrag;   // x = lateral, y = vertical, z = longitudinal
[SerializeField] private float   leakThreshold, leakRateMin, leakRateMax, sinkThreshold;
[SerializeField] private ShipPart[] connectedCompartments;
[SerializeField] private bool    compartmentalized;
```

`height` and `surfaceArea` are taken from the part's collider bounds at `Awake`,
so the artist's collision mesh *is* the hull form — nobody authors a waterline.

### 11.2 The buoyancy job

**[CODE]** `WaterJob_Math`, in full, is 30 lines:

```csharp
float y = datum.GlobalY(transform.Position);
float submerged = Mathf.Clamp01((partHeight * 0.5f - y) / partHeight);
Vector3 force = Buoyancy(submerged, displacement) * Vector3.up + Drag(submerged, ...);

private float Buoyancy(float submerged, float displacement)
    => Mathf.Lerp(1.2f, 1000f, submerged) * 9.81f * displacement;

private Vector3 Drag(float submerged, ...) {
    float fwd = Dot(velocity, forward), rt = Dot(velocity, right), up = Dot(velocity, upAxis);
    float Fz = fwd * |fwd| * directionalDrag.z * mass * submerged;
    float Fx = rt  * |rt|  * directionalDrag.x * mass * submerged;
    float Fy = up  * |up|  * directionalDrag.y * mass * submerged;
    return ClampMagnitude(-forward*Fz - right*Fx - upAxis*Fy, mass * 500f);
}
```

**[inferred] Three decisions in that small function, all of them good.**

- **Buoyancy interpolates the fluid density, not the force.** `Lerp(1.2, 1000,
  submerged) × g × displacement` is Archimedes with the medium blended from air to
  water across the part's height. A half-submerged compartment gets ~half the
  force *and* the transition is C⁰-smooth, so a hull rolling through the waterline
  does not chatter. The 1.2 term also means a compartment in air experiences a
  tiny aerostatic force, which is physically true and costs nothing.
- **Drag is quadratic and anisotropic in the part's own axes.** `directionalDrag`
  as a `Vector3` with lateral ≫ longitudinal is what makes a hull *track* — it
  resists sideways motion far more than forward motion, which is a keel, and it is
  why the ship turns instead of sliding. Same trick as the aero model's per-part
  frame: the geometry (which way the part faces) carries the physics.
- **The force is clamped to `mass * 500`** — an explicit stability rail, because a
  quadratic drag term with a large coefficient will explode if a tick ever
  delivers a large velocity.

Force is applied per-part at the part's position, but *not* with
`AddForceAtPosition`:

```csharp
// Ship.ApplyPartsForce
Vector3 com = transform.TransformPoint(rb.centerOfMass);
foreach (ShipPart part in parts) {
    ref var f = ref part.JobFields.Ref();
    force  += f.force;
    torque += Vector3.Cross(f.force, -(f.forcePosition - com));
}
rb.AddForce(force);
rb.AddTorque(torque);
```

**[inferred]** N `AddForceAtPosition` calls become **one `AddForce` and one
`AddTorque`**, with the moment arms accumulated in C#. For a carrier with dozens
of compartments that is dozens of interop calls saved per tick, and the result is
identical.

### 11.3 The water is a flat plane

**[CODE]** `Datum.WaterPlane()` returns `new Plane(Vector3.up, origin.position)`.
`ConstrainToSeaLevel` forces water objects to `Datum.LocalSeaY` with identity
rotation every frame. There is **no wave height function anywhere in the
assembly**, and the buoyancy job reads a scalar `y` with no displacement term.

**[inferred] Ships float on a flat plane and the visible ocean is decoration.** No
Gerstner sum, no FFT, no heightfield sampling — nothing like
[`sea_of_thieves_water.md`](sea_of_thieves_water.md)'s ocean, where the wave
field *is* the physics and ships pitch to the swell. This is the single largest
simplification in the game's physics and it is almost certainly correct for it: a
destroyer is 150 m long, the camera is usually a kilometre up in an aircraft, and
the gameplay is "did the missile hit the ship", not "can I sail this in a storm".
Wave response would cost a sampled height field, a job that reads it, and a whole
class of stability problems at the waterline — to be seen by nobody.

Worth stating plainly as a design lesson: **the fidelity of a medium should be set
by what interacts with it, not by what it is.** The same game models blade-element
aerodynamics on a rotor and a completely flat sea, and both are right.

### 11.4 Flooding, damage control and the death test

**[CODE]** This is where the ship model earns its keep. `ShipPart.ApplyDamage`:

```csharp
float t = Clamp01((hitPoints - structuralThreshold) / (leakThreshold - structuralThreshold));
leakRate           = Lerp(leakRateMax, leakRateMin, t);
leakToDisplacement = Clamp(originalDisplacement * hitPoints / leakThreshold, 0, originalDisplacement);
directionalDrag.z  = Lerp(directionalDrag.z, directionalDrag.x, pow(1f - t, 2f));   // ← hull form lost
```

and then, per tick, while the compartment is below the waterline:

```csharp
private void Leak() {
    if (transform.GlobalPosition().y - height < Datum.SeaLevel.y) displacement -= leakRate * dt;
    if (compartmentalized || displacement >= originalDisplacement * ship.damageControlDeploymentThreshold) return;
    ship.damageControlAvailable -= originalDisplacement;      // seal it, and pay for it
    compartmentalized = true;
    if (ship.damageControlAvailable <= 0f)
        foreach (var c in connectedCompartments) c.Flood();   // ← progressive flooding
}
```

with a crew working the other way on a 1 Hz slow update, delayed by
`damageControlDelay / skill`:

```csharp
private void DamageControl() {
    leakRate     = max(leakRate - 0.02f * leakRateMin, 0f);
    displacement = min(displacement + 0.001f * originalDisplacement, originalDisplacement);
    ship.damageControlAvailable -= 10f * 0.02f * leakRateMin + 0.001f * originalDisplacement;
}
```

**[inferred] Four things fall out of this and none of them is scripted.**

1. **A ship lists and settles as compartments fill**, because buoyancy is per-part
   and off-centre flooding is an off-centre force. Nobody wrote a heel angle.
2. **Damage control is a shared, exhaustible resource** — a single ship-wide
   `damageControlAvailable` pool that both sealing a compartment and pumping it out
   draw from. So a lightly damaged ship recovers and a heavily damaged one hits
   zero, at which point **flooding propagates to connected compartments** and the
   loss becomes unrecoverable. That is a damage model with a *tipping point*, from
   one float and a graph of neighbours.
3. **Crew quality is one number.** `damageControlDelay /= Clamp(ship.skill, 0.1f, 1f)`
   — a better crew starts fighting the flooding sooner.
4. **A holed ship slows down.** `directionalDrag.z → directionalDrag.x` means the
   damaged compartment's *longitudinal* drag coefficient converges on its
   *lateral* one — the hull has stopped being a streamlined shape and started
   being a hole. `Flood()` sets `directionalDrag.z = directionalDrag.x` outright.
   Speed loss after a torpedo hit is thus a consequence of the hull form
   degrading, not a speed multiplier.

The kill test is a physical posture check on a 2 s slow update:

```csharp
if (transform.position.y < Datum.LocalSeaY - definition.spawnOffset.y   // settled below the surface
 || Dot(transform.up, Vector3.up) < 0.5f                                 // heeled past 60°
 || Dot(transform.forward, Vector3.up) > 0.25f) {                        // bow up past ~14°
    Networkdisabled = true; ReportKilled();
}
```

plus a parallel test: if at least `criticalRatio` (default 0.5) of the designated
`criticalParts` are critically damaged, the ship is dead. **[inferred] The first
test is the one to admire** — a ship dies because it is *physically sinking or
capsizing*, evaluated from its actual transform, not because a hit-point pool
reached zero. Capsize, settle by the bow, or go under; all three read the same
three dot products.

### 11.5 Breaking in half

**[CODE]** `ShipPart.Detach` promotes the section to its own `Rigidbody` (mass
transferred out of the parent), caps it at `maxLinearVelocity = 60`, and joins it
with a **`ConfigurableJoint`**:

```csharp
joint.xMotion = joint.yMotion = joint.zMotion = ConfigurableJointMotion.Locked;
joint.angularXMotion = joint.angularYMotion = joint.angularZMotion = ConfigurableJointMotion.Limited;
// ±20° on all three angular axes
joint.breakForce = rb.mass * breakJointStrength;
```

Locked linearly, limited to ±20° angularly — **a hull that has broken its back but
is still hanging together**, until the joint's own break force is exceeded. The
severed section is flooded and zeroed for displacement, its parent loses 20% of
its displacement and floods, and every connected compartment does the same. And
crucially, `UnitPart_OnParentDetached` re-registers the drifting section with the
buoyancy job (`JobManager.Add(SetupJob())`), so **the wreck still floats, drifts
and sinks on its own**, with `rb.angularDrag` set from its submergence.

### 11.6 Propulsion and steering

**[CODE]** `ShipPropulsion` is 199 lines and the physics is four:

```csharp
int inWater = (!underwater || thrustTransform.position.y < Datum.LocalSeaY) ? 1 : 0;
float steerTarget = (1f + ship.speed * momentumFactor) * inputs.steering;
thrustSmoothed = SmoothDamp(thrustSmoothed, inputs.throttle, ref v1, inputSmoothing);
steerSmoothed  = SmoothDamp(steerSmoothed,  steerTarget,     ref v2, inputSmoothing) * ship.AllowedSteerRate;

part.rb.AddForceAtPosition(inWater * thrustSmoothed * thrust * transform.forward
                         + inWater * steerSmoothed  * steeringThrust * transform.right,
                           thrustTransform.position);
```

**[inferred] Every term earns its place.**

- The steering force is applied **at the stern transform**, so it is a rudder: a
  lateral force aft of the centre of mass, producing a yaw moment plus a small
  sideways translation. Combined with the lateral hull drag from §11.2, that
  produces the correct behaviour — the ship pivots, the hull resists the resulting
  sideslip, and the ship carves a turn with a drift angle.
- **`(1 + speed * momentumFactor)`** scales steering demand with speed, which is
  how a rudder actually works: no flow over the rudder, no authority. A ship
  stopped in the water barely turns, and a ship at flank speed turns hard.
- **`inWater`** requires the screw to be below the waterline. Pitch the stern out
  and thrust stops.
- `AllowedSteerRate` is a gameplay hook (dropped to 0.1 during e.g. flight
  operations); `SmoothDamp` on both axes is the engine-order-telegraph lag.

Propulsion is disabled by part detachment, by damage past `damageThreshold`, or by
the ship being disabled — one shaft at a time on a multi-screw ship, so losing an
engine room costs half the thrust *and* biases the remaining thrust off-centre,
because the surviving screw is still applying its force at its own transform.

### 11.7 Wakes — particles, and only particles

**[CODE]** The visible wake is `Ship.WakeParticles`, an array of authored
`ParticleSystem`s driven from a **1 Hz slow update**:

```csharp
public void Initialize(Unit parentUnit) {
    main.simulationSpace        = ParticleSystemSimulationSpace.Custom;
    main.customSimulationSpace  = Datum.origin;               // ← survives origin shifts
    main.emitterVelocityMode    = ParticleSystemEmitterVelocityMode.Custom;
}
public void Update(float speed, Vector3 velocity) {
    float t = Clamp01((speed - minSpeed) / (maxSpeed - minSpeed));
    if (t <= 0 && system.isPlaying) system.Stop();
    if (t >  0 && !system.isPlaying) system.Play();
    emit.rateOverTime  = Lerp(minRate,    maxRate,    t);
    main.startColor    = new Color(1,1,1, Lerp(minOpacity, maxOpacity, t));
    main.startSize     = Lerp(minSize,    maxSize,    t);
    main.startLifetime = Lerp(minLife,    maxLife,    t);
    main.startRotation = parentUnit.transform.eulerAngles.y * Deg2Rad;   // aligned to heading
    velocity.y = 0f;  main.emitterVelocity = velocity;
}
```

**[inferred] Everything about the wake is in that snippet, and what is *not* there
matters more than what is.** I searched the whole assembly: there is **no wake
render target, no displacement texture, no foam accumulation buffer, no
water-normal injection, no decal, and no ripple simulation**. The ocean material
receives exactly two per-map textures (`_macro_basecolor`, `_macro_depth` from
`MapSettings`) plus a global `_Global_BlastMap` for nuclear detonations. The wake
is a quad spray.

The five details that make it work anyway:

1. **`simulationSpace = Custom` bound to `Datum.origin`.** Particles are simulated
   in the datum's frame, so a floating-origin shift (§14) moves the particles with
   the world instead of leaving a wake hanging in space. This is the non-obvious
   requirement for any persistent particle effect in a large world, and the same
   trick is used by `TrailEmitter`, `Downwash` and `WaterEffect`.
2. **`startRotation` is the ship's heading**, so the (presumably V-shaped, tiling)
   wake quads are laid down aligned with the hull rather than randomly oriented.
3. **`emitterVelocity` is set explicitly and flattened to horizontal**, which is
   what makes emitted particles inherit the ship's motion smoothly rather than
   popping.
4. **Rate, size, lifetime and opacity are all lerped by the same speed factor**, so
   a wake grows and lengthens with speed from one parameter, and switches off
   entirely at rest.
5. **It updates at 1 Hz.** A wake is a slowly varying phenomenon and nobody can
   see the quantisation.

`TrailEmitter` is the related class for aircraft contrails and torpedo tracks, and
its emission is **distance-based, not time-based**:

```csharp
emitCounter += dt * rb.velocity.magnitude / segmentLength;    // one particle per segmentLength travelled
if (emitCounter > 1f) {
    main.startColor        = new Color(1,1,1, opacity * (1 - opacityVariation * Random.value));
    main.startSizeMultiplier = baseSize * (1 - Random.value * scaleVariation);
    emitParams.position    = emitTransform.position.ToGlobalPosition().AsVector3();
    emitParams.velocity    = rb.velocity + startSpeed * randomJitter;
    trailSystem.Emit(emitParams, 1);  emitCounter = 0f;
}
```

**[inferred]** Distance-based emission gives a trail with constant spatial density
regardless of speed — the correct choice, and one that also bounds the particle
count for a fast mover. The per-particle size and opacity jitter is what stops a
tiled trail reading as a repeating texture.

And the rotor/jet downwash on the surface (`Downwash`, §7.7's visual counterpart)
is a **raycast from each thrust source against the water plane and the terrain**,
positioning a dust or spray effect at the contact point, driving a
`ParticleSystemForceField` whose `directionY` is scaled by the thrust ratio, and
lagging the effect position toward the target at `downwashSpeed / max(radarAlt, 1)`
— so the spray patch chases the helicopter and lags further behind the higher it
is.

### 11.8 The two-phase job

**[CODE]** The ship job is scheduled differently from the aero job, and the reason
is worth understanding:

```csharp
// Schedule_1, at the very start of the fixed step
handleAccess = new ReadTransformJob(transformValues, ...).ScheduleReadOnly(transformAccess, 32);

// ... other systems schedule ...

// Schedule_2, later in the same phase
handleAccess.Complete();
SetArgs_Update();                                     // needs the transform positions:
                                                      //   fields.velocity = rb.GetPointVelocity(pos)
handleMath = new WaterJob_Math{...}.ScheduleByRef(countInJob, 32);
```

**[inferred]** The aero job can fill its input fields without knowing where the
part is (`rb.velocity` is a property of the body). The water job cannot — a ship
is one rigidbody, so each probe's local velocity is
`rb.GetPointVelocity(worldPosition)` and the world position comes from the
transform read. So the transform read is scheduled *first, alone*, allowed to
overlap with everything else being scheduled, and only then completed. The
dependency is real and the code pays for it with the minimum possible
serialisation rather than by reading transforms on the main thread.

---

## 12. The threading model, which is the whole cost story

**[PATCH]** Update 0.32, December 2025: *"Implemented CPU multithreading for
aerodynamics calculations"*, *"...for ship water physics calculations"*, and
elsewhere *"...for line-of-sight calculations"*.

**[CODE]** `NuclearOption.Jobs` is 43 types and is the answer to §2's cost.

### 12.1 One scheduler, driven off the player loop

`JobManager` is a `SceneSingleton` that does **not** use `FixedUpdate`. It runs a
`UniTask` loop:

```csharp
while (true) {
    await UniTask.Yield(PlayerLoopTiming.FixedUpdate);
    FixedUpdateEarly();      // ScheduleJobs(); PilotAeroInputs();
    FixedUpdateLate();       // FinishJobs();
}
```

**[inferred]** Hooking `PlayerLoopTiming.FixedUpdate` rather than declaring
`FixedUpdate()` on a MonoBehaviour buys guaranteed ordering without touching
Unity's script execution order settings, which are a global, invisible,
unversioned config file. A real maintenance win in a project where "schedule
before every part reads its fields" is a correctness requirement.

The scheduling order, from `ScheduleJobs()`:

```
waterJobs.Schedule_1(shipParts)     ─┐  transform read only
vehicleJob.Schedule_1(vehicles)      │
controlJob.Schedule(controlSurfaces) │  math → write transforms
aeroJob.Schedule(aeroParts)          │  ← depends on controlJob.handleAccess
detector.Schedule()                  │  line of sight / detection
waterJobs.Schedule_2(shared)         │  ← completes its own read, then math
vehicleJob.Schedule_2(shared)       ─┘
JobHandle.ScheduleBatchedJobs();
```

then — and this is the part worth stealing —

```csharp
private void FixedUpdateEarly() { ScheduleJobs(); PilotAeroInputs(); }
private void FixedUpdateLate()  { FinishJobs(); }
```

**[inferred] The pilot and AI work runs on the main thread while the jobs are in
flight.** The window between scheduling and completing is not idle — it is filled
with exactly the work that does not depend on this tick's forces (each `Pilot`'s
state machine, which is where the AI's stick inputs are computed). A one-tick
input latency is the price, and for a control surface with a 20°/s servo that is
0.4° of deflection. Free parallelism for a cost nobody can feel.

### 12.2 The data layout

Each simulated thing keeps its job state in an **unmanaged allocation** it owns:

```csharp
private PtrAllocation<AeroPartFields> JobFields;      // native memory, ref-counted
private JobPart<AeroPart, AeroPartFields> JobPart;    // the registration handle
```

`AeroPartFields` and `ShipPartFields` are flat POD structs. The jobs iterate
`NativeArray<Ptr<XFields>>`. Registration is dynamic and cheap: `JobUnitList`
batches adds and removes, capacity doubles from a floor of 128, and removal is a
**swap-with-last**. Transforms are held in a `TransformAccessArray` with a
ref-counted `IndexLink` so several parts can share one transform without
double-registering it.

**[inferred]** Note what this is *not*. It is not an ECS. `AeroPart` is still a
`MonoBehaviour` with virtual methods, events, audio sources and a damage model.
What was extracted is **only the ~40 bytes and the ~60 lines that run per part per
tick**. That is the same line CLAUDE.md draws between the spatial query layer and
the entity layer, arrived at independently, and it is the pragmatic recipe for
data-oriented design: move the hot fields into a parallel flat array, leave the
object alone.

The per-tick sync is three explicit methods on each part — `UpdateJobFields()`
(managed → native, before scheduling), the job, and `ApplyJobFields()` (native →
managed, after completing). Verbose, and completely unambiguous about when each
side owns the data.

### 12.3 Only locally-simulated vehicles are in the job at all

```csharp
private void AeroPart_OnInitialize() {
    if (!parentUnit.remoteSim && parentUnit is Aircraft) {
        airfoilID = definition.aircraftParameters.GetAirfoilID(airfoil);
        JobManager.Add(new JobPart<AeroPart, AeroPartFields>(this, GetOrCreateJobField()));
    }
}
// and for ships:
if (!base.LocalSim) return;
JobManager.Add(this);
```

**[inferred] The single largest performance decision in the game, and it is one
`if`.** An aircraft flown by another player is never in the aero job — not at
reduced rate, not at reduced fidelity, *not at all*. It is a kinematic shell
driven by snapshot interpolation (§13). So the physics cost on a client is **your
own vehicle, plus whatever the host is simulating**, and it does not grow with the
number of players in the server. §13 is why that is safe.

### 12.4 Profiler markers, everywhere

`ProfilerMarker` instances exist for `AeroPhysics`, `SwashRotor.SampleForces`,
`Ship.ApplyJobResults`, `JobManager FixedUpdateEarly`/`Late`, `JobManager
Schedule`, `AeroJob Schedule`/`SetArgs`/`SetArgs_Full`/`Finish`,
`ControlSurfaceJob *`, `WaterJob *`, `JobManager Pilot Inputs`,
`FloatingOrigin.OriginShift`/`_MoveRoots`, `ApplySnapshot`, `Aircraft.VisualUpdate`,
`ValidateInternal_v1`, `Aircraft.LocalSimFixedUpdate`. There is a bespoke
`JobPerf` that takes timestamps **inside Burst jobs**
(`JobPerf.GetTimestampBurst()` is the first line of every `Execute`) and logs them
back through the shared fields struct.

**[inferred]** CLAUDE.md's "give every new system a zone" observed in the wild,
including the harder half — instrumenting inside jobs, where the Unity profiler's
own hierarchy does not reach. The granularity is also right by CLAUDE.md's rule:
the aero job gets *four* markers because it is a large share of the fixed step,
and `Downwash` gets none because it is a particle effect.

---

## 13. Networking: the physics is client-authoritative, and that is the point

**[CODE]** `AircraftNetworkTransform : NetworkTransformBase`, over Mirage.

**The owning client simulates its own aircraft and tells the server where it is.**

```csharp
public override float SyncInterval => 0.05f;              // 20 Hz
public int SendInputsInterval = 4;                        // control inputs every 4th snapshot

private NetworkSnapshot CreateSnapshot() {
    CompressedInputs? clientInputs = null;
    if (--inputIntervalCounter <= 0) { inputIntervalCounter = SendInputsInterval;
                                       clientInputs = new CompressedInputs(Aircraft.GetInputs()); }
    return new NetworkSnapshot(clientInputs, transform.GlobalPosition(), transform.rotation, rb.velocity);
}
```

Position, rotation, velocity at 20 Hz on an **unreliable** channel, each snapshot
**resent up to 4 times** (`clientSnapshots[4]`, `sendsRemaining = 4`) so a lost
packet is covered by the next one rather than by a retransmit round trip. Control
inputs ride along at 5 Hz — used only to animate the remote aircraft's control
surfaces, since `ControlSurface` jobs run on every client.

The server does not re-simulate. It **validates**, in `ClientAuthChecks`:

```csharp
float dt = snapshot.timestamp - previousTimestamp;
Vector3 accel = (v - previousVelocity) / dt;

if (speed² > 2500 && |accel|² > 3600) {              // >50 m/s and >60 m/s²
    float alignment = Dot(previousVelocity.normalized, accel.normalized);
    if      (alignment >  0.3) { if (|accel|² > 10000) reject(AccelerationForward); }       // >100 m/s²
    else if (alignment > -0.6) { if (|accel|² >  3600) reject(AccelerationPerpendicular); } // >60 m/s²
    else { float limit = (|previousVelocity| + 10) / dt;                                    // deceleration
           if (|accel|² > limit²) reject(AccelerationBackwards); }
}
// dead reckoning position check
GlobalPosition expected = previousPosition + (previousVelocity + v) * 0.5f * dt;
if (FastMath.OutOfRange(snapshot.globalPos, expected, |v̄| * dt + 10f)) reject(Position);
```

**[inferred] Three things about those thresholds.** They are **direction-aware** —
100 m/s² forward is allowed (afterburner plus a dive), 60 m/s² sideways is not
(that would be a 6 g turn appearing between two ticks), and deceleration is
allowed to be nearly unbounded because *hitting the ground* is a legal way to
decelerate. They are **gated on speed**, so taxiing and spawning are not false
positives. And the position check is a **dead-reckoning envelope with 10 m slop**,
validating against the client's own claimed velocity rather than an absolute speed
limit — a Mach 2 aircraft and a helicopter get the same rule.

Rejection is graded, not binary: `Owner.SetError(errorCost, InvalidTransformSnapshot)`
accumulates against a per-player error budget, there is a rate limit on the RPC
(`Refill = 25, MaxTokens = 100`), and only one condition revokes authority
outright — `snapshot.globalPos.y < -10f`, an aircraft claiming to be underwater.

Remote vehicles are pushed into a snapshot buffer and played back on the **render**
tick, not the physics tick, with per-client clock estimation (`SmoothNetworkTime`,
an EMA of RTT and of the client/server clock offset) and per-snapshot extrapolation
allowance (`extraExtrapolation = RTT/2`, clamped).

**[inferred] Why this is the right call here, and why it would be wrong
elsewhere.** [`valve_networking.md`](valve_networking.md)'s model — server
authority, client prediction, server rewind — requires the server to be able to
reproduce the client's simulation. Affordable for a capsule with a ground-friction
model. **Not** affordable for 40 rigidbodies, 24 blade elements, a joint solver and
a PID cascade, at 50 Hz, times 24 players — and not even reproducible: the Unity
solver is not deterministic across machines, and complex-physics mode is entered
and left at *different times on different clients* because it depends on **each
client's own camera distance** (§5). There is no shared ground truth to rewind to.
So the architecture concedes authority and defends the boundary with plausibility
checks, paying for it in the one place it can afford to: a cheating client can fly
implausibly-but-not-impossibly.

**[inferred]** The line this draws is worth keeping: **simulation depth and server
authority trade against each other directly.** A game can have a deep per-vehicle
physics model or a server-authoritative one, and the deeper the model, the more the
honest answer is "validate, don't reproduce".

---

## 14. Large worlds

**[CODE]** A 100 km map in float precision needs an origin strategy, and this is
the tersest good one I have read:

```csharp
[SerializeField] private float threshold       = 1024f;  // shift when the camera passes this
[SerializeField] private float originShiftStep = 64f;    // and always by a multiple of this

public void OriginShift(Vector3 cameraPosition) {
    if (EditorHandle.DraggingHandle || !ShouldShift(cameraPosition)) return;
    Vector3 shift = ShiftPosition(cameraPosition);      // Mathf.Round(p / step) * step, per axis
    foreach (GameObject root in SceneManager.GetActiveScene().GetRootGameObjects())
        root.transform.position -= shift;
    Datum.AfterOriginShift();
    Physics.SyncTransforms();
}
```

**[inferred] The `originShiftStep` quantisation is the detail worth taking**, and
the source comment says why: *"Aligns the origin move to a grid, Origin Shift will
always be a multiple of this. This helps shaders create stable positions at large
scales."* Shift by an arbitrary float and every world-space noise lookup, every
triplanar projection and every detail-scatter hash moves by a sub-texel amount —
so foliage crawls and detail textures swim on every shift. Quantise to 64 m and any
shader whose input is world position modulo something ≤ 64 sees **no change at
all**.

Alongside it, `Datum` is a static holding the origin transform and a
`GlobalPosition` type used everywhere a real-world coordinate is needed. The Burst
jobs get a `BurstDatum` snapshot so they can convert without touching managed
state — which is how `AeroJob_Math` knows the part's true altitude for the density
lookup and `WaterJob_Math` knows its height above the sea. Network snapshots carry
`GlobalPosition`, so the origin shift is purely local and never crosses the wire.
`ShaderGlobalManager.SetDatum` pushes it to shaders as `_Datum_OriginPosition`,
alongside `_Datum_WorldExtent`.

`FloatingOrigin` also defines a ±90 km `worldSimulate` bounds and an (unreferenced)
`RBKillBounds` that destroys rigidbodies outside it.

Read against the other large-world notes:
[`elite_dangerous.md`](elite_dangerous.md) needs two maths libraries and 64-bit
sector addressing because its extent is galactic;
[`space_engineers.md`](space_engineers.md) partitions into physics clusters rather
than rebasing because it has many independent reference frames. Nuclear Option has
**one frame and 100 km**, so it takes the cheapest option that works, and the only
sophistication it adds is the one shaders force.

---

## 15. Gameplay systems, briefly

Not the focus, but the shape matters for judging the whole.

**[CODE] Damage and detection.** Typed damage (pierce/blast/fire/impact) through
`ArmorProperties`. Detection is its own Burst job (`DetectorManager`,
`DetectionRequest`). Radar has a horizon check (**[PATCH]** 0.30), RCS that
decreases as external stores are expended (0.30.9 — and drag decreases with it,
which is the same `dragArea` field from §3.1), chaff, ECM with an accumulated
`ecmIntensity`, and five seeker families (`IRSeeker`, `SARHSeeker`, `ARHSeeker`,
`ARMSeeker`, `OpticalSeeker*`) each as its own class. **All of this is covered
properly in [`nuclear_option_combat.md`](nuclear_option_combat.md)** — the signal
equation, the clutter and doppler terms, and the seekers.

**[CODE] G and the pilot.** `Pilot` accumulates `gForce = Dot(accel, up)` and takes
damage above 20 g. `GLOC` runs a blood-pressure integrator rather than a timer:

```csharp
float pump = bloodPumpRate - gForce * 0.04f;              // 0.28 baseline
if (bloodPressure < 0.55f) pump += stamina * 0.25f;       // the body fights back, while it can
stamina       += (staminaRecoveryRate - gForce * 0.04f) * dt;   // 0.18 baseline, clamped 0..1
bloodPressure += pump * dt;                                     // clamped 0..1
if (bloodPressure < 0.2f) { LOC(); }                            // out for 3–6 s
```

with vignette, desaturation and a low-pass audio filter driven off `bloodPressure`
between 0.2 and 0.6. **[inferred]** Because stamina is a separate slow pool,
*repeated* high-g pulls grey you out sooner than the first one, and recovery is
incomplete — which is the actual physiology and which a timer cannot express.

**[CODE] AI.** `Pilot` is a state machine (`PilotPlayerState`, `PilotParkedState`,
`AIPilotTakeoffState`, `AIPilotTaxiState`, `AIPilotLandingState`,
`AIPilotShortLandingState`, and helicopter equivalents).
`AIPilotCombatModes` is 1,269 lines. **[inferred] The important structural fact is
that AI pilots write to `ControlInputs` — the same struct the human stick writes to
— and then go through the same `ControlsFilter` and the same aerodynamics.** There
is no AI flight model. That is why **[PATCH]** *"Improved maneuvering and terrain
avoidance skills for AI aircraft"* ships in the same update as *"New aerodynamics
and Fly-by-wire control filters"*: they are the same change. How the AI actually
flies — the turning-circle rate limit, bank scheduling, energy management and
evasion — is [`nuclear_option_combat.md`](nuclear_option_combat.md) §7. `ShipAI`,
`AssaultCarrierAI`, `LandingCraftAI`, `MobileArtilleryAI` do the same for surface
units, writing `ShipInputs.throttle` and `.steering`.

**[CODE] Rendering.** URP. In-house GPU foliage: `DetailRenderer`, `GrassRenderer`,
`TreeRenderer`, `ComputeFrustumCulling` + `ComputeGroupCount` (compute-shader
culling into indirect-draw args), `TerrainHeightMap`, `ShaderGlobalManager`.
`MeshLightSettings`, `ExposureController`, `CloudLayer`, `SonicBoomManager`,
`VaporEffect`/`VaporEmitter` (**[PATCH]** 0.27: *"Vapour effects over wings during
hard maneuvers"*), `MushroomCloud`, `Shockwave`, `BlastManager` (which writes a
global `_Global_BlastMap` texture — the one screen-space effect the terrain and
water shaders read), `DebrisManager`, `FragmentManager`. There is a
`UnityGraphicsBullshit` class, which I mention only because every renderer has one
and most are not honest about it.

**[CODE] Mission layer.** An in-house node-graph editor (`NuclearOption.NodeGraph`,
46 types) plus `NuclearOption.SavedMission` (74 types) with **46 further types
dedicated to version conversion** (`SavedMission.ConvertVersions`). **[inferred]**
That ratio — 46 converters against 74 schema types — is what shipping a user-facing
editor into Early Access actually costs, and it is worth knowing before building
one.

---

## 16. Read against Broken Arrow

Two Unity games, same era, opposite manifests.

| | Broken Arrow | Nuclear Option |
|---|---|---|
| Scripting backend | IL2CPP | **Mono** |
| Pipeline | HDRP | **URP** |
| Install | 54 GB | **2.3 GB** |
| Third-party managed assemblies | ~90 | **26** |
| In-house assemblies | 6 | **1** |
| Rendering / streaming / vegetation | **bought** (GPUInstancer, ProjectDawn.Impostor, Nature Renderer, Granite VT, FSR) | **written** (`NuclearOption.Effects`) |
| Entity model | two ECS frameworks compiled in, one used | plain MonoBehaviours + a hot-field job layer |
| Scripting | MoonSharp Lua + a bought node-graph framework | in-house node graph |
| Data authoring | Excel spreadsheets | ScriptableObjects |
| Networking | LiteNetLib, server-authoritative-ish | Mirage, **client-authoritative vehicles** |
| Serialisation | MemoryPack **and** protobuf-net | Newtonsoft.Json |

**[inferred] The obvious reading is wrong.** This is not "one team bought, one team
built". It is that **the two games sell different things, and each wrote the thing
it sells.**

Broken Arrow sells a battlefield: hundreds of units, a whole-map camera, unique
terrain texels everywhere. That is a *rendering and streaming* problem, those have
vendors, so buying is rational — and Broken Arrow's in-house code is the
simulation, which is what it actually sells.

Nuclear Option sells **how the vehicles feel**, and there is no package for that. A
bought flight model would be the product. So the one thing it *must* write is the
one thing this note is about, and the reason it can afford to write the rest is the
flip side of the same choice: at 2.3 GB with a handful of maps, the asset-scale
problems that make Granite worth 27 GB never arise. **The buy/build line follows
the product, and the asset budget follows the buy/build line.**

Two smaller notes:

- **Both shipped their dev tooling.** Broken Arrow ships a hot-reload package and a
  perf overlay; Nuclear Option ships `JamesFrowen.Graphy` and a
  `PlayerSettings.debugVis` mode that draws force arrows on every rotor blade. In
  Nuclear Option's case I would argue for it: a model whose behaviour is *emergent*
  cannot be debugged by reading its parameters, only by looking at its intermediate
  vectors, and §7.7 is the case in point.
- **`Mono` versus `IL2CPP` is a real decision, not an oversight.** Mono is slower and
  trivially decompilable; it is also why the game has a healthy BepInEx modding
  scene, which for a niche sim is worth more than the AOT margin. Given that all the
  hot code is in Burst jobs anyway (§12), the managed tier's speed matters much less
  than it would in a game that ran its simulation in C#.

---

## 17. Reconstruction guide

**[inferred] throughout.** What follows is the recipe, written so this can be
rebuilt. Where a value is not marked as read from the code, it is a
physically-derived starting point, not Shockfront's number (§19).

### 17.1 The data model

Four component types carry everything.

**`AeroPart`** — a lifting or draggy piece of airframe.

| Field | Meaning | Starting value |
|---|---|---|
| `wingArea` | m² of lifting surface; 0 for pure-drag parts | real planform area, split across parts |
| `dragArea` | m² of equivalent flat-plate parasitic area | ~0.02 × wingArea for clean surfaces; 0.5–2 m² for a fuselage |
| `liftNormal` | the transform the flow is resolved in — **z = chordwise, y = lift, x = spanwise** | the part transform, or the moving surface |
| `centerOfLift` | local offset for the part's own pitching moment | usually zero; use it for a quarter-chord offset |
| `airfoil` | index into the aircraft's airfoil list; −1 for the flat-plate fallback | −1 until you need a real foil |
| `streamlining` | drag area handed to the parent when this breaks off | ≈ the part's frontal area |
| `airflowChanneling` | radians the local flow is bent toward a duct axis | 0, or up to ~1.0 rad inside a duct |
| `buoyancy` | multiplier on `mass × g` when submerged | 2 |
| `mass`, `collisionSize`, `joints[]` | structure | from the model |

**`Airfoil`** — two Cl(α)/Cd(α) curves over ±180°, baked to 128 floats each.

**`ControlSurface`** — `pitchRange`, `rollRange`, `yawRange` (degrees per unit of
that axis; sum them onto one hinge), `servoSpeed` (deg/s, 20 is a reasonable
default and it *matters* — see §4), `flap`, and the split-surface set.

**`ShipPart`** — `displacement` (m³), `directionalDrag` (Vector3 in part axes),
`leakThreshold`/`leakRateMin`/`leakRateMax`/`sinkThreshold`,
`connectedCompartments[]`. Height and surface area come from the collider bounds.

### 17.2 The tick order

This is the part that is hard to reinvent and easy to copy. Everything below
happens inside one fixed step, at 50 Hz.

```
FIXED STEP BEGIN
 1. [main]  read pilot/AI stick → ControlInputs                 (previous tick's, see 6)
 2. [main]  ControlsFilter: RelaxedStability → AutoHover → AimAssist → FlyByWire
 3. [main]  ControlSurface.UpdateJobFields()   (copy inputs into POD)
 4. [main]  AeroPart.UpdateJobFields()         (copy rb.velocity, mass, areas into POD)
 5. [jobs]  ControlSurfaceJob_Math  →  SetLocalRotationJob  →  (transforms now current)
            ReadTransformJob        →  AeroJob_Math
            ShipReadTransformJob → (complete) → ShipUpdateFields → WaterJob_Math
 6. [main]  ← WHILE THE JOBS RUN: pilot state machines, AI, targeting
 7. [main]  complete jobs; AeroPart.ApplyJobFields() → rb.AddForce / AddTorque
                           Ship.ApplyPartsForce()    → ONE AddForce + ONE AddTorque
 8. [main]  rotors, props, jets, ducted fans: their own FixedUpdate, AddForceAtPosition
 9. [main]  landing gear raycasts and tyre forces
10. [main]  amortised checks: ONE part's attachment test, buoyancy posture every 2 s
FIXED STEP END → Unity solver integrates
```

Three ordering facts are load-bearing:

- **Step 5's chain must be a real dependency, not a hope.** The aero read is
  scheduled as a dependent of the control-surface transform write. Get this wrong
  and control inputs apply one frame late *sometimes*, which is a bug you will
  never reproduce.
- **Step 6 is where you get parallelism for free.** Any work that does not consume
  this tick's forces belongs here.
- **Step 1 reads the previous tick's inputs** by construction, because the fields
  were copied at step 3. Accept the one-tick lag; do not try to close it.

### 17.3 Building a fixed-wing aircraft

1. **Lay out the parts.** Wing left, wing right, horizontal tail left/right,
   vertical tail, fuselage, engine nacelles. Six to ten `AeroPart`s is enough to
   fly; 30–50 is what Nuclear Option uses to also *break* convincingly.
2. **Orient each `liftNormal`.** Chord along +z, lift along +y, span along ±x.
   **The fin is a wing rolled 90°.** Nothing else is needed for directional
   stability — get this right and you will not write a sideslip term.
3. **Place them honestly.** Static pitch stability is `tail area × tail arm`; roll
   damping is `wing area × span`. If the aircraft is unstable, the tail is too
   small or too close, not "the numbers are wrong".
4. **Airfoil curve.** Start with the fallback (`Cl = 1.8 sin 5α`, `Cd = 1.5(1−cos 2α) + 0.02`)
   which is already a full-circle flat-plate model. Author a real curve only when
   stall behaviour needs character: linear `Cl = 2π·α` to ~15°, a rounded peak of
   1.2–1.6, a drop to the flat-plate line by 30°, and the flat-plate line
   thereafter.
5. **Control surfaces.** `pitchRange` ≈ 20–25° on an elevator, `rollRange` ≈ 15–20°
   on an aileron, `yawRange` ≈ 25–30° on a rudder. Set `servoSpeed` the same on
   surfaces that must move together.
6. **Then, and only then, the FBW.** Bare-airframe first — it must be flyable, if
   unpleasant, with a direct stick-to-surface mapping. If it is not, the geometry is
   wrong and no controller will hide it. Then add, in order: the q-scaled gain
   (§6.1), the g-command pitch law (§6.2), the α limiter, roll trim, yaw
   weathervaning.
7. **Engine.** Static thrust × altitude curve × speed curve × spool ratio.
   `spoolRate` such that idle-to-military takes 4–8 s.

**The debugging order matters.** Trim, then stability, then control authority, then
the assists. A wrong assist layered on a wrong airframe produces a system where no
single change improves anything.

### 17.4 Building a helicopter

1. **Rotor geometry.** N blades as child transforms of a hub; per blade a `hinge`
   at the root and a `span` transform whose local z is the blade axis. Blade
   `length`, `mass`, and `hubRadius` from the model.
2. **Inertias.** Blade flapping `I = (1/3) m L²`. Shaft `I = (1/3) N m (L + r)²`.
   These are not tuning values; get them from the geometry.
3. **Per-station loop.** 3 radial stations is enough. At each: tangential speed
   `ω(r + r_hub)cos β`, minus the freestream, plus `rb.GetPointVelocity(station)`,
   plus the flapping velocity `r β̇`. Add blade weight and the centrifugal term
   `m_station ω²(r+r_hub)` along the span.
4. **Blade aerodynamics analytic, not tabulated** — `Cl = a·α` with a `SmoothStep`
   blend to `sin 2α` past stall over a 10° band; `Cd = Cd0 + k(1 − cos 2α)`.
5. **Flapping.** Accumulate `M = Σ F·n̂_flap · r`, add `−kβ` (asymmetric up/down,
   ×5 past the stops) and `−cβ̇`, integrate `β̈ = M/I`. Feed the reaction back into
   the hub force as `−M/(0.7L)·n̂_flap`.
6. **Swashplate.** Blade pitch = `dot(bladeAzimuth, swash.forward)·cyclic·pitchCmd`
   + `dot(bladeAzimuth, −swash.right)·cyclic·rollCmd` + `collective·collectiveTravel`.
   Put phase lag on the swashplate transform's Y rotation (start at 90° and tune by
   feel).
7. **Sub-step the azimuth 4×** per fixed step and average the forces. Non-optional
   for 2–4 bladed rotors.
8. **Driveline.** `Q_engine = P/ω` clamped to a torque limit; `ω += (Q_engine +
   Q_aero − friction)·dt / I`; apply `−Q_engine` about the mast to the airframe;
   governor requests `P_nom(1 + 30·droop)`. **Do not special-case autorotation** —
   it appears.
9. **Induced flow.** `v_i = T·k/(A(1 + 0.1·v_translational))`, lagged ~1 s, injected
   as extra downward flow at the disc. Ground effect **scaled by rotor radius**
   (see §7.7's slip) — reduce `v_i` by up to ~40% within one rotor diameter.
10. **VRS.** Extra downward inflow proportional to `descentRate − threshold −
    0.4·v_translational`, smoothed with a ~4 s constant. It becomes self-reinforcing
    through step 3 with no further code.
11. **Gyroscopic term** on the airframe: `ω_rotor · 0.5 I · (−ω_body.x·right −
    ω_body.z·forward)`.
12. **Tail rotor** thrust auto-set to `Q_shaft / tailArm`, pedal added on top.

### 17.5 Building a VTOL

The three archetypes and the one thing each needs:

- **Tiltwing / tiltrotor** — rotate the wing/nacelle transforms; the `AeroPart`s go
  with them and the aerodynamics is automatic. What you must add is the **tilt
  schedule** (a curve of tilt vs airspeed, rate-limited) and the parts' own
  `airflowChanneling` toward the rotor axis.
- **Lift fan + swivel nozzle** — the fan thrust is not authored, it is **solved**
  each tick from the moment balance about the current CoM (§8.4). Charge the fan's
  shaft power back to the engine as a parasitic thrust loss.
- **Vectored thrust** — nozzle transforms rotated at a rate limit; thrust applied
  with `AddForceAtPosition` along the nozzle's forward axis so the moment is free.

All three need a **hover authority source that does not depend on airflow** —
either an RCS (§8.6) or the honestly-named `MagicTorqueController` (§6.5) — and
both should be gated on something that can be lost (engine bleed, electrical
power), so damage has consequences in the hover.

### 17.6 Building a ship

1. **One rigidbody.** Do not give compartments their own bodies.
2. **Compartments as buoyancy probes.** Ten to thirty per hull. Each carries a
   `displacement` in m³. **The sum of displacements × 1000 kg/m³ should be ~1.5–2×
   the ship's mass** so it floats with a sensible freeboard and has reserve
   buoyancy to lose.
3. **Submergence** from the part's own height:
   `clamp01((h/2 − y_global) / h)`. Buoyancy `= lerp(1.2, 1000, submerged) · g ·
   displacement` upward.
4. **Anisotropic quadratic drag** in part axes, scaled by part mass and submergence.
   **Lateral ≈ 10–30× longitudinal** — this is the keel and it is what makes the
   ship track. Clamp the total to `mass × 500` for stability.
5. **Sum, don't `AddForceAtPosition`.** Accumulate force and `Cross(F, −(pos − com))`
   and issue one of each.
6. **Propulsion**: `throttle · thrust` along forward plus `steer · (1 + speed·k) ·
   steeringThrust` along right, both applied **at a stern transform**. Require the
   thrust point to be below the waterline. `SmoothDamp` both inputs.
7. **Flooding.** Damage sets a `leakRate` and a target `leakToDisplacement`;
   displacement bleeds toward it while the compartment is below the waterline. A
   ship-wide damage-control pool both seals compartments (paying `originalDisplacement`)
   and slowly restores them; when it runs out, **flood the neighbours**. Lerp the
   compartment's longitudinal drag toward its lateral drag as it floods.
8. **Death by posture**, on a slow update: below the surface, heeled past 60°
   (`dot(up, worldUp) < 0.5`), or bow up past ~14° (`dot(forward, worldUp) > 0.25`).
9. **Wreckage.** A severed section gets its own rigidbody plus a `ConfigurableJoint`
   (linear locked, ±20° angular, `breakForce = mass × k`), is flooded, and **is
   re-registered with the buoyancy job** so it drifts and sinks on its own.
10. **Skip waves** unless something interacts with them. §11.3.

### 17.7 Wakes and water effects

- Wake = **particle systems in a custom simulation space bound to the world datum**,
  with rate, size, lifetime and opacity all lerped from one normalised speed factor,
  `startRotation` set to the hull heading and `emitterVelocity` set to the
  (horizontal) hull velocity. Update at 1 Hz.
- Trails = **distance-based emission**, one particle per `segmentLength` of travel,
  with per-particle size and opacity jitter to break up tiling.
- Rotor/jet downwash = a raycast per thrust source against the water plane and the
  terrain, positioning a spray/dust effect at the contact and driving a
  `ParticleSystemForceField` by thrust ratio, with the effect position lagging by
  `k / max(altitude, 1)`.
- **Binding particle simulation space to the origin-shift datum is not optional** in
  a floating-origin world. It is the difference between a wake that stays on the
  water and one that teleports every kilometre.

### 17.8 What to build in what order

1. `AeroPart` + the airfoil table + the force equations, single-threaded, on one
   rigidbody. **You can fly at this point.**
2. Control surfaces as transform rotations feeding step 1.
3. Atmosphere (density LUT, speed of sound line).
4. Landing gear (raycast suspension + the stiction anchor) — otherwise you cannot
   take off or land and cannot test anything.
5. The FBW, layer by layer, in §17.3's order.
6. Jobify. Not before: the single-threaded version is the reference you will debug
   the parallel one against.
7. Damage: `wingEffectiveness`, joint break-force scaling, the position-based
   detachment test.
8. Rotors, if you need them. They are a week on their own.
9. Ships. The buoyancy job is an afternoon; the flooding model is the interesting
   part.
10. Networking last, and read §13 before choosing an authority model, because it
    constrains everything above it.

### 17.9 Gotchas

- **Forces, never accelerations.** Every equation above produces a force applied at
  a position. That is what makes moments free and mass changes automatic.
- **Resolve flow in the part's frame, not the body's.** This is the whole trick. If
  you find yourself writing a sideslip term, you have put the fin in the body frame.
- **Lift direction is `Cross(v, spanAxis).normalized`, not the part's up.** Using the
  part's up loses induced drag and breaks inverted flight.
- **Clamp every quadratic term.** A quadratic drag law with a large coefficient will
  find the one tick where the velocity is absurd. Both jobs here clamp
  (`mass × 500` for ships, `|v| × 0.5 × mass × 60` for ditching aircraft) and check
  `float.IsFinite` before applying.
- **Rate-limit every actuator.** Servos, nozzles, tilt mechanisms, swashplates. It
  costs one `Mathf.Clamp` and it is the difference between an aircraft that feels
  mechanical and one that feels like a cursor.
- **Sub-step anything rotating fast**, and average the result.
- **Bake authored curves at load.** `AnimationCurve.Evaluate` cannot go in a Burst
  job and should not go in a hot loop.
- **Amortise the checks nobody can see** — attachment tests, buoyancy posture,
  thrust-curve lookups, wake parameters. One per tick, or 1 Hz, is usually enough.
- **Give the derived quantities a debug visualisation from day one.** In an emergent
  model, a wrong constant deletes a phenomenon silently (§7.7). Arrows for per-part
  force, per-station blade velocity and lift, flap angle, and induced flow.

---

## 18. What is worth taking

Ranked by how much I would want it in cromwell, filtered against CLAUDE.md's rules.

1. **Make the geometry the model where you can afford to.** (§2, §4, §8.6, §11.2.)
   The highest-leverage idea in the codebase. The test: *would a rule I am about to
   write be a consequence of the parts' positions and orientations if I simulated
   them?* Control authority, swing-wing effects, damage asymmetry, sideslip and a
   ship's righting moment are all things this game never wrote, and they are all
   correct.

2. **Bake authored curves into flat LUTs at load; keep the authoring format separate
   from the runtime format.** (§3.2.) 128 floats, one multiply-add to index, all
   contiguous. The CLAUDE.md derived-cache pattern with the invalidation problem
   removed by construction.

3. **Gate LOD transitions on the dynamics being calm, not only on the metric.** (§5.)
   `gForce < 2f && near` — distance says the transition is *wanted*, the g test says
   it is *safe*. Any state change that re-derives mass, inertia or constraint
   topology needs the second half, and it is one extra condition.

4. **Amortise per-part validation round-robin, one per tick.** (§5.1, §11.4.)
   `i++; if (i >= count) i = 0; parts[i].Check();` The right answer to "how many
   times does this actually run" is often "fewer" rather than "faster".

5. **Extract only the hot fields into a parallel flat array; leave the object alone.**
   (§12.2.) `AeroPart` stays a MonoBehaviour with events and audio; `AeroPartFields`
   is ~40 bytes of POD in native memory. Exactly CLAUDE.md's DOD-in-the-query-layer,
   OOP-in-the-entity-layer split, with a concrete recipe: two explicit sync methods
   and an owned allocation.

6. **Fill the job latency window with work that does not depend on the result.**
   (§12.1.) Schedule, run the AI and input logic, then complete. One tick of latency
   where nobody can perceive it.

7. **Sum moments yourself instead of N `AddForceAtPosition` calls.** (§11.2.) One
   `AddForce` + one `AddTorque` per body, identical result, dozens of interop calls
   saved.

8. **Scale controller gains by the measurable quantity that scales the plant's gain.**
   (§6.1.) `remapFactor = 1/max(q,1)`. One variable, and it is the difference between
   an FBW that feels tuned and one that wobbles.

9. **Anchor low-speed contact to a remembered point on the surface.** (§9.3.) The
   contact-patch stiction model is the correct fix for creeping and jitter, and it is
   strictly better than raising the friction coefficient.

10. **Make a moving frame's velocity explicitly queryable.** (§9.4.)
    `AnimatedPhysicsSurface` exists solely so a wheel on an animated collider can
    subtract the surface's motion. Same lesson as
    [`moving_frame_navigation.md`](moving_frame_navigation.md), one layer down.

11. **Quantise floating-origin shifts to a grid.** (§14.) 64 m, one `Mathf.Round`, and
    every world-space shader input stops swimming — a bug that is extremely hard to
    diagnose from the symptom. And **bind persistent particle systems to the datum**.

12. **Put the physical relation in rather than a curve, when one exists.** (§8.2, §10,
    §11.2.) `T = (2ρA P²)^⅓` gives correct hot-and-high hover for free;
    `lerp(ρ_air, ρ_water, submerged) · g · V` gives a smooth waterline for free.

13. **Set a medium's fidelity by what interacts with it.** (§11.3.) Blade-element
    aerodynamics and a perfectly flat sea, in the same game, both correct.

14. **Validate, don't reproduce, when the simulation is too deep to re-run.** (§13.)
    Direction-aware acceleration bounds plus a dead-reckoning envelope, with a graded
    error budget rather than a boolean kick — and be explicit that this is what deep
    per-vehicle physics costs you in authority.

And two anti-patterns:

15. **Do not leave superseded subsystems inline in the class that replaced them.**
    (§6.4.) Five unwired assist classes sit above the one that runs, in the file a
    reader opens first.

16. **Do not ship a half-finished parallel implementation of a system that already
    works.** (§7.9.) `SoftBodyRotor` duplicates `SwashRotor`'s job, instantiates debug
    geometry unconditionally, and computes a collision result it discards. Either
    finish it or fence it off.

---

## 19. What is *not* established here

- **Nothing was run or profiled.** Every performance claim is structural — "this is
  scheduled in parallel", "this is 24 evaluations per rotor" — not measured.
- **Per-aircraft and per-ship authored values were not recovered, and here is why.**
  The `AircraftParameters` / `ShipDefinition` ScriptableObjects hold the tuning —
  airfoil curves, `aircraftGLimit`, `cornerSpeed`, the PID vectors, `displacement`,
  `directionalDrag`. I loaded all 13 serialized files with UnityPy (Unity 2022.3.62f2
  confirmed from the headers) and found 29,268 `MonoBehaviour` objects of which only
  **118 carry a type tree** — this build ships with script type trees stripped, so
  field names and layouts are not in the file. Recovering them needs a type-tree
  generator run against `Assembly-CSharp.dll` (AssetRipper or equivalent), which I did
  not do. **So I can describe the shape of every vehicle's tuning and none of its
  values**, and §17's numbers are physically derived, not Shockfront's.
- **The prefabs were not inspected**, so actual part counts, wing areas, joint break
  forces, compartment layouts and which aircraft use `MagicTorqueController` or
  `RelaxedStabilityController` are unknown. §5's "30–50 parts" is the store page's
  number, not one I counted.
- **The shaders were not read.** §15's rendering paragraph is a list of class names
  and their obvious purpose. In particular I have described what the wake system
  *emits*, not what the water material does with it.
- **The ground-effect observation in §7.7 is a reading of an expression**, not a claim
  about how the game feels.
- **`GroundVehicle`** has its own two-phase Burst job (`GroundVehicleJob_Math1/2`,
  `SampleGroundResult`) which I did not read; ground vehicles are outside this note's
  scope.
- **There is no developer account of any of this.** Where I have written "because", it
  is **[inferred]** — my reconstruction of a reason, not a reported one. The release
  notes are the only first-party technical source and they are one line per change.

---

## 20. Where things are

To re-read the assembly: `ilspycmd -p -o <out> -r <Managed> <Managed>/Assembly-CSharp.dll`
(version 8.2.0.7535 works; the current release fails to install as a dotnet tool).
1,214 files come out.

| System | Files |
|---|---|
| Fixed-wing aero | `AeroPart.cs`, `Airfoil.cs`, `NuclearOption.Jobs/AeroJob_Math.cs`, `AeroPartFields.cs`, `AeroJobSettings.cs` |
| Control surfaces | `ControlSurface.cs`, `ControlSurfacePhysics.cs`, `NuclearOption.Jobs/ControlSurfaceJob_Math.cs`, `ControlJobSettings.cs` |
| Airframe & LOD | `Aircraft.cs` (`SetComplexPhysics`, `SetSimplePhysics`, `CheckPhysicsLod`, `PartChecker`), `UnitPart.cs`, `PartJoint.cs` |
| Fly-by-wire | `ControlsFilter.cs`, `HeloControlsFilter.cs`, `RelaxedStabilityController.cs`, `MagicTorqueController.cs`, `AoALimiter.cs`, `AeroPID.cs`, `PID.cs` |
| Rotorcraft | `SwashRotor.cs`, `RotorShaft.cs`, `Transmission.cs`, `SoftBodyRotor.cs`, `VRSWarning.cs`, `Downwash.cs` |
| Props & VTOL | `ConstantSpeedProp.cs`, `PropFan.cs`, `DuctedFan.cs`, `DuctedThrustSystem.cs`, `SwivelDuctSystem.cs`, `TiltWingController.cs`, `SwingWingController.cs`, `HighLiftDevice.cs`, `Airbrake.cs`, `ReactionControlSystem.cs`, `AirCushion.cs`, `Repulsorlift.cs` |
| Jets | `Turbofan.cs`, `Turbojet.cs`, `TurbineEngine.cs`, `JetNozzle.cs` |
| Ground handling | `LandingGear.cs`, `GearPart.cs`, `AnimatedPhysicsSurface.cs`, `ArrestorGear.cs`, `TailHook.cs`, `OpticalLandingSystem.cs` |
| **Ships** | `Ship.cs`, `ShipPart.cs`, `ShipPropulsion.cs`, `ShipInputs.cs`, `ShipDefinition.cs`, `ShipAI.cs`, `NuclearOption.Jobs/WaterJob_Math.cs`, `ShipPartFields.cs`, `WaterJobsSettings.cs` |
| **Water effects** | `Ship.WakeParticles` (in `Ship.cs`), `TrailEmitter.cs`, `WaterEffect.cs`, `Downwash.cs`, `ConstrainToSeaLevel.cs`, `Datum.cs` (`WaterPlane`) |
| Atmosphere | `LevelInfo.cs`, `Datum.cs`, `FloatingOrigin.cs`, `GlobalPosition.cs` |
| Threading | `NuclearOption.Jobs/` (43 files), especially `JobManager.cs`, `Ptr*.cs`, `JobUnitList.cs`, `IndexLink.cs` |
| Networking | `NuclearOption.NetworkTransforms/` (21 files), especially `AircraftNetworkTransform.cs`, `ClientAuthChecks.cs`, `SnapshotBuffer.cs`, `SmoothNetworkTime.cs` |
| Pilot & AI | `Pilot.cs`, `PilotPlayerState.cs`, `AIPilot*.cs`, `AIHelo*.cs`, `AIPilotCombatModes.cs`, `Autopilot*.cs`, `CombatAI.cs`, `GLOC.cs`, `ShipAI.cs` |

---

## Sources

- **The retail install**, `E:\SteamLibrary\steamapps\common\Nuclear Option` —
  `Assembly-CSharp.dll` decompiled with ILSpy 8.2, plus `boot.config`,
  `ScriptingAssemblies.json`, `Plugins/`, `StreamingAssets/`, and the serialized
  asset headers read with UnityPy 1.25.3.
- [Nuclear Option — Development (release notes)](https://nuclearoption.wiki.gg/wiki/Development) — the only first-party technical source.
- [Nuclear Option on Steam](https://store.steampowered.com/app/2168680/Nuclear_Option/) — feature claims, aircraft roster, map scale.
- [Nuclear Option Wiki](https://nuclearoption.wiki.gg/) — general.
- [PC Gamer, first look](https://www.pcgamer.com/fly-hard-fly-fast-drop-nukes-in-this-accessible-near-future-flight-combat-sim/) and [Skyward FM first impressions](https://www.skywardfm.com/post/first-impression-nuclear-option-early-access-release) — positioning only, no technical content.
- Developer interviews exist ([2024 review / 2025 preview](https://www.youtube.com/watch?v=HnQ9Xn49u2g), [VR and future plans](https://www.youtube.com/watch?v=Ojqd0oC2c8U), [The Simulator Lite with Nukes](https://www.youtube.com/watch?v=2X-tjlC6Mqw)) but cover roadmap and design intent rather than implementation. **No engineering talk, blog or paper was found.**
