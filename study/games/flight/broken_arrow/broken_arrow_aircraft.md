# Broken Arrow — how aircraft fly, and why the bombs land where you clicked

Companion to [`broken_arrow.md`](broken_arrow.md), which named three aircraft
systems in its system table and said nothing about what they do. This note
answers what that table left open: **how an aircraft gets to a waypoint, how it
steers, whether it slows down to turn, how it hits a strike point accurately
enough for bombing, and what happens when the order changes mid-flight.**

The short answer, because it is the interesting part and it is not what a flight
game would do:

> **The planner and the follower use the same motion model, and that is the whole
> trick.** The aircraft is a kinematic Dubins vehicle — constant forward speed,
> hard-clamped turn rate, no aerodynamics. The route is generated as a **Dubins
> path built with exactly that aircraft's turn radius**. So the route is
> achievable by construction: the follower is never asked to fly a corner it
> cannot make, and tracking error stays small **because the plan already respects
> the follower's limits**, not because the controller is good.
>
> **Weapon release is scheduled in arc-length space, not world space.**
> `GetBombDropPoint(float bombingProgress)`, `InTargetZone(float progress)`,
> `GetFrameFireTarget(float strafeProgress)` — all keyed to distance travelled
> along the route, which passes through the strike point by construction. The
> release *timing* therefore cannot drift even if the aircraft is slightly off
> the line.
>
> **It does slow down to turn, and it is authored, not emergent.** Every airframe
> has a `CornerSpeed` — tooltip: *"Maximum speed a plane can stay at while
> turning"* — plus a global `DecreaseSpeedAngle`, *"When Angle between the
> aircraft and target will be more this value, plane starts decrease speed"*.
>
> **Re-planning is guarded by hysteresis in at least three places**, the clearest
> being `SHORTWAY_LENGTH_MIN_DIFF_RATIO = 1.5` — a newly-computed route must be
> **1.5× shorter** to displace the current one.
>
> **No PIDs on aircraft.** Broken Arrow ships three PID implementations and uses
> none of them here. The single live consumer in the retail binary is a missile.

> **Source and its limits.** Everything tagged **[DUMP]** was read from an
> Il2CppDumper v6.7.46 dump of the **retail** `GameAssembly.dll` +
> `global-metadata.dat` (metadata v31, files dated 2025-08-06), from the Steam
> install on this machine. That is a stronger grade than the main note's
> **[BUILD]** tag, which was the *flat identifier table* — this dump has
> namespaces, base classes, field names and offsets, method signatures, `const`
> values, and Unity `[Header]`/`[Tooltip]`/`[Range]` attributes.
>
> **The hard limit: IL2CPP compiles method bodies to native code, and the dump
> does not contain them.** Every method here is `{ }`. Names, types, constants and
> tooltips are *literal*; anything about control flow inside a method is
> **[inferred]** and tagged. Tooltips are the strongest evidence in this note —
> they are the developers' own prose about their own fields.
>
> An older alpha dump also exists on this machine (`E:\Il2CppDumper`). It lacks
> `NetworkAircraftLocalComponent` entirely and stores the airframe preset as a
> `ScriptableObject` rather than a database row. Everything structural below is
> the same in both — mild evidence the design is settled — but every value quoted
> here is from the retail dump.

Related: [`broken_arrow.md`](broken_arrow.md) §5 (the ground navigation graph
these aircraft entirely bypass),
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) (§11
here reads the two against each other),
[`nuclear_option_control.md`](../nuclear_option/nuclear_option_control.md),
[`navigation.md`](../../../topics/agents/navigation.md).

---

## 1. Aircraft do not touch the navigation system

**[DUMP]** The main note's §5 describes two pathfinders — a bespoke layered
`BrokenArrow.Core.Navigation.NavigationGraph` and Unity's `NavMeshQuery`.

**Neither appears anywhere in the aircraft path.** The whole fixed-wing stack
lives in `BrokenArrow.Client.Ecs.Planes` and depends on no graph, no navmesh, no
A*, no grid:

```
BrokenArrow.Client.Ecs.Planes
  AircraftControl                 the mover
  AircraftHelper                  static geometry helpers
  TangentWaysBuilder              turn circles + common tangents
  BaseWaypointBuilder             route storage, sampling, Catmull-Rom
    WaypointBombing               : IPlaneFlyRoute
    WaypointStrafe                : IPlaneFlyRoute
    WaypointCircle                : IPlaneFlyRoute
    WaypointSinglePoint           : IPlaneFlyRoute
  RoutePoint                      { Vector3 Position; Vector3 Direction; }

BrokenArrow.Client.Ecs.Planes.Systems
  AircraftAiControlSystem                 [With(AircraftControlComponent, AltitudeComponent)]
  AircraftWaypointProgressTrackerSystem   [With(AltitudeComponent, AircraftControlComponent)]
  AircraftWeaponSystem                    [With(AircraftControlComponent)]
  AirCombatSystem, AfterburnerAbilitySystem, AltitudeChangeAbilitySystem, PlaneAutoEvacSystem
```

**[inferred]** This is the right call and it is worth saying why. A pathfinder
exists to route *around* things. An aircraft at altitude has nothing to route
around — its constraint is not obstacles, it is **the turn radius**, which no
graph search expresses. A graph search would produce a polyline no aircraft could
fly, and every corner would then need smoothing back into something with bounded
curvature — at which point you have done the geometry anyway, badly and twice.
Broken Arrow generates the flyable curve directly. The main note's §5 argument —
*"there is no navigation system, only representations and algorithms that read
them"* — gets a third representation here, and it is not a graph at all.

---

## 2. Building the route

### 2.1 The turn radius is computed, not discovered

**[DUMP]** `AircraftHelper`:

```csharp
public static float GetRotationRadius(float turnRate, float forwardSpeed) { }
```

**[inferred]** `r = v / ω` — the standard rate-turn radius. Because turn rate is a
data field rather than a consequence of lift (§6.4), this is **exact rather than
approximate**, which is the property everything downstream leans on. Every route
builder takes a `_radius`; `TangentWaysBuilder` stores one `readonly float
_radius`.

`PlaneFlyPreset` exposes derived read-only properties beside the authored ones:

```csharp
public float CornerSpeedYaw          { get; }   // derived, no setter
public float CornerAfterburnSpeedYaw { get; }
public float AfterburnCornerSpeed    { get; }
```

**[inferred]** Yaw rate *at corner speed* is precomputed. That is precisely the
number to feed `GetRotationRadius` for the tightest circle the aircraft can
actually hold, and exposing it as a property rather than a local says it is
wanted in more than one place — route planning and the "is this target too close
to attack" check both need it (`PlanesConfig.RadiusMultForTooCloseCheck`,
`AirCombatSystem.TOO_CLOSE_CHECK_RADIUS_MULT = 2`).

### 2.2 `TangentWaysBuilder` is a Dubins path

**[DUMP]** The core of the system:

```csharp
public sealed class TangentWaysBuilder {
    private readonly float _radius;
    private readonly float _altitude;
    private Vector3 _targetCircleCenterLeft,  _targetCircleCenterRight;
    private Vector3 _planeCircleCenterLeft,   _planeCircleCenterRight;
    private readonly Vector3[] _targetCircleLeft,  _targetCircleRight;
    private readonly Vector3[] _planeCircleLeft,   _planeCircleRight;
    private readonly FastList<TangentWayInfo> _tangentDistances;

    public void GenerateTargetCircles(Vector3 enterManeuverPos, Vector3 maneuverDirection);
    public void GeneratePlaneCircles (Vector3 planePos, Vector3 planeForward, int offset);
    private TangentsData GetCommonTangentLines(in Vector3 circleCenterPlane,
                                               in Vector3 circleCenterTarget);
    private void CalculateTangentDistance(Vector3[] planePoints, Vector3[] targetPoints,
                                          in Vector3 pCircleCenter, in Vector3 tCircleCenter);
    public ReadOnlySpan<TangentWayInfo> GetTangentDistances();
}

public class TangentWayInfo {
    public float Distance;
    public readonly List<Vector3> Waypoints;
    public void RecalculateDistance();
}
```

**[inferred] This is a textbook Dubins CSC path**, readable from the names alone:

1. Place two circles of radius `r` tangent to the aircraft's current heading — one
   turning left, one right (`GeneratePlaneCircles(pos, forward, …)`).
2. Place two more at the destination, tangent to the *required arrival heading*
   (`GenerateTargetCircles(enterManeuverPos, maneuverDirection)` — note it takes a
   direction, not just a point).
3. Compute common tangent lines between each plane-circle / target-circle pair
   (`GetCommonTangentLines`).
4. Measure each resulting **arc → straight → arc** route
   (`TangentWayInfo.Distance`, `RecalculateDistance`) and return them all
   (`GetTangentDistances`).
5. The caller picks the best — see §5.2, where "best" is not simply "shortest".

`CreatePointsForCircle(…, int degreesMult = 1)` discretises each circle into a
`Vector3[]` at roughly one point per degree, and `IndexOfClosestCirclePoint` /
`GetBestIndexAboutPoint(points, point, bool toPoint)` snap the tangent contact
back to an array index — so arcs are emitted as polylines rather than solved
analytically, and `toPoint` picks which way round the circle to walk.

**Why this matters more than the mechanism:** a Dubins path is *the shortest
curvature-bounded route between two poses*. Broken Arrow is not approximating a
plausible-looking approach, it is solving for the optimal one before the aircraft
has moved a metre — and solving it **with the same radius the follower will
actually be limited to**. That equality is the subject of §4 and it is the real
answer to the accuracy question.

`GeneratePlaneCircles` taking an `int offset` is **[inferred]** how a re-plan
avoids starting from a position the aircraft has already left: offset ahead along
the existing route by the distance covered while the new route is built, so the
new route begins where the aircraft *will be*.

### 2.3 Smoothing, and arc-length parameterisation

**[DUMP]** `BaseWaypointBuilder`, shared by all four route types:

```csharp
protected Vector3[] FlyPoints;
protected float[]   ToPointDistances;
protected bool      IsCirleRoute;      // their spelling
protected float     FinishLength;
public    float     RouteLength                { get; }
protected float     RouteFirstTargetDistance   { get; }   // ← arc-length of the target
protected float     RouteSecondTargetDistance  { get; }
public    ReadOnlySpan<Vector3> PointsAccess   { get; }

public    RoutePoint GetRoutePoint   (float distance, out int targetPointIndex);
public    Vector3    GetRoutePosition(float distance, out int targetPointIndex);
private   Vector3    CatmullRom(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float i);
protected void       CalculateDistances(Vector3 firstTargetPoint,
                                        Vector3 secondTargetPoint, Vector3 finishPoint);
public    bool       InTargetZone(float progress);
public    bool       IsFinished  (float progress);
public    void       DrawFlypath();
```

**[inferred]** Three things are load-bearing:

**The route is parameterised by arc length, not by index.** `GetRoutePoint(float
distance, …)` plus the parallel `ToPointDistances[]` makes "where am I" a single
float and "where will I be in 200 m" one addition. §4 is entirely built on this.

**`CalculateDistances(firstTargetPoint, secondTargetPoint, finishPoint)` converts
the strike geometry into arc-length coordinates** and caches them as
`RouteFirstTargetDistance` / `RouteSecondTargetDistance`. **The target's position
on the route is known as a distance the moment the route is built.**

**`CatmullRom` runs over the tangent polyline**, rounding where arc meets
straight. Catmull-Rom passes *through* its control points — the property you need
here, since smoothing must not push the route off the strike line it was built
for.

`RoutePoint` carries a `Direction` as well as a `Position`, so sampling yields a
full pose: the aircraft is handed a heading to hold, not merely a point to aim
at. `DrawFlypath()` shipping in the retail binary is the same pattern the main
note's §8 found everywhere — debug draw never stripped.

### 2.4 Four route shapes, one interface

**[DUMP]**

```csharp
public interface IPlaneFlyRoute {
    float      RouteLength { get; }
    bool       IsFinished  (float dist);
    bool       InTargetZone(float dist);
    RoutePoint GetRoutePoint(float distance, out int targetPointIndex);
    void       ChangeAltitude(float targetAltitude);
}
```

| Implementation | Shape | Notable members |
|---|---|---|
| `WaypointSinglePoint` | fly to one point | `IsStaticAltitude`, `IsNonBuildingPixel`, `UpdatePoint` |
| `WaypointCircle` | orbit | `WAY_POINTS = 6`, `POINT_ANGLE = 60`, `InitialPointSet`, `IsApproachPhase`, `LastApproachDistance`, `CircleCenterPosition` |
| `WaypointBombing` | run-in, release, egress | `GetBombDropPoint(float)`, `MainTrajectoryStartIndex`, `FakeBombing`, `_weaponsTimeOffset`, `_afterburnerAssumed` |
| `WaypointStrafe` | run-in, dive, strafe, pull out | `GetFrameFireTarget(float)`, `DiveStartIndex`, `StrikeLength`, `_beforeDivePoint`, `_endDivePoint`, `_exitStrafePoint` |

`WaypointBombing` and `WaypointStrafe` each own a `TangentWaysBuilder`;
`WaypointCircle` and `WaypointSinglePoint` do not — **[inferred]** an orbit *is*
its own turn circle, and a bare point needs no arrival heading solved for.

**[inferred] The manoeuvre vocabulary is the route type.** There is no "attack
behaviour" that steers differently — there is a route shape per attack, built
once, and one follower that flies whatever it is handed. A bombing run and a
patrol orbit differ in the curve, not in the code that flies it.

---

## 3. Following the route

### 3.1 It is a kinematic Dubins vehicle, and it really is steering

**[DUMP]** Per-aircraft state:

```csharp
public struct AircraftControlComponent {
    public readonly int             PilotID;
    public readonly PlaneFlyPreset  PlaneConfig;
    public readonly AircraftControl AircraftController;
    public readonly Transform       TargetTransform;   // the follow target
    public readonly Transform       PlaneRoot;         // visual
    public readonly Transform       PlaneTransform;    // logical
    public Vector3        LastPosition;
    public int            ProgressTargetIndex;
    public RoutePoint     ProgressPoint;
    public float          ProgressDistance;            // ← position on the route
    public IPlaneFlyRoute FlyRoute;
    public float Speed { get; }
}
```

and the mover writes a `TransformComponent` that has settable `Position` and
`Rotation` (with `POSITION_MARGIN = 0.001` / `ROTATION_MARGIN = 0.001` change
flags):

```csharp
public void UpdateMove(in Entity, in AircraftControlComponent, float deltaTime);
private void RotateToTarget(ref TransformComponent, Vector3 toTargetDirection, float dt);
private void YawToTarget   (ref TransformComponent, float yRotToTarget, float dt);
private void PitchToTarget (ref TransformComponent, float xRotToTarget, float dt);
public float ForwardSpeed { get; }
```

**[inferred] So this is a genuine steering loop, not curve playback** — an
important correction to the tempting reading. Each frame: sample the route ahead
of `ProgressDistance`, take the heading error to that point, rotate the logical
transform toward it at a **hard-clamped rate**, and advance the position along
the aircraft's own forward vector at `ForwardSpeed`.

That is exactly the **Dubins car**: constant speed, bounded turn rate, heading
integrated. Which is the point — **it is the same vehicle model
`TangentWaysBuilder` assumed when it drew the circles.**

### 3.1a How it faces the path: yaw and pitch are separate, rate-clamped axes

**[DUMP]** The rotation entry points decompose:

```csharp
private void RotateToTarget(ref TransformComponent, Vector3 toTargetDirection, float dt);
private void YawToTarget   (ref TransformComponent, float yRotToTarget, float dt);
private void PitchToTarget (ref TransformComponent, float xRotToTarget, float dt);
private float _currentYawRate, _currentPitchRate;
```

**[inferred] This is not a slerp toward a target rotation, and the difference
matters.** `RotateToTarget` takes a *direction* and splits it into a yaw error
(`yRotToTarget`, about Y) and a pitch error (`xRotToTarget`, about X), each fed to
its own method with its own rate — `_currentYawRate` and `_currentPitchRate`,
which §6.4 shows come from **two independent speed curves**
(`MinSpeedYaw`/`MaxSpeedYaw`/`AfterburnYaw` versus
`MinSpeedPitch`/`MaxSpeedPitch`/`AfterburnPitch`).

So an aircraft turning hard while also climbing spends its yaw budget and its
pitch budget separately, and an airframe can be authored as a good turner and a
poor climber. A single quaternion slerp toward the target pose could not express
that — it has one rate, so pitch and yaw would trade against each other in a
ratio nobody chose.

The direction being tracked is the carrot from §3.3, and
`AircraftControlComponent.TargetTransform` is a real `Transform` **[inferred]**
parked at that point — which is also why `DrawFlypath` and the debug systems can
show it.

Note what is *absent*: no roll axis is steered at all. `_dotRight` / `_lerpRight`
(§7) are the visual bank, applied to a different transform. **The aircraft is
steered in two axes and decorated in the third.**

### 3.1b Acceleration is a rate toward a target speed, with four of them

**[DUMP]** `PlaneFlyPreset` carries four rates, not one:

```csharp
public float Acceleration              { get; set; }
public float Deceleration              { get; set; }
public float AfterburnerAcceleration   { get; set; }
public float AfterburnerDeceleration   { get; set; }
```

against `AircraftControl`:

```csharp
public  float ForwardSpeed   { get; }          // scalar, along the nose
public  bool  IsAfterburning { get; }
private void  SetForwardSpeed(Entity, in AircraftControlComponent, float deltaTime);
private void  ModifyDogfightSpeed(Entity, ref float targetSpeed);
private void  ModifyStrafeSpeed  (in AircraftControlComponent, ref float targetSpeed);
```

**[inferred]** §6.2 establishes the shape: compute a *target* speed, then move
`ForwardSpeed` toward it at a rate chosen by direction of travel and afterburner
state. **Separating acceleration from deceleration is the honest part** — a jet
accelerates far worse than it decelerates, and one symmetric rate would make
aircraft either sluggish to slow or unrealistically brisk to speed up. Doubling
again for afterburner means the burner changes *how fast the speed changes*, not
merely the ceiling, which is what makes lighting it feel like an event.

**The critical structural fact is that `ForwardSpeed` is a scalar.** There is no
velocity vector on an aircraft. Speed is magnitude along the nose, and the nose is
`YawToTarget`'s output, so **direction of travel and facing are the same thing by
construction — an aircraft here physically cannot slip, skid or drift.** That is
the property §4.1's accuracy argument rests on and it is worth contrasting with
the ground units, which do not have it (§9a).

### 3.2 Waypoint acceptance is an angle test, not a distance test

**[DUMP]** `AircraftWaypointProgressTrackerSystem`:

```csharp
private const float GROUND_CRASH_LINECAST_TIMELENGTH  = 1.5f;
private const float GROUND_CRASH_PREVENTION_INTENSITY = 100f;
private const float PROGRESS_ADD_ANGLE = 80f;
private readonly float _progressAddDot;   // precomputed cos(80°) ≈ 0.1736
```

**[inferred]** Progress advances while the next route point is within 80° of the
aircraft's heading — i.e. while it is still meaningfully *ahead*. Converting the
angle to a dot product once in the constructor rather than per frame is the
"hoist the trig out of the loop" discipline CLAUDE.md asks for.

**This is the fix for the failure mode the accuracy question is really about.** A
distance test — "am I within N metres" — is what makes aircraft **orbit a
waypoint they overshot**, because the distance never closes and the point never
retires. An angle test retires it the moment it is behind, reached or not.
Nuclear Option solves the same bug the same way:
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) §7.4
records that acceptance has a second condition, *the waypoint is behind me*,
"which is what prevents the classic orbit-around-a-missed-waypoint failure."
**Two engines, two genres, same guard.**

`PlanesConfig.NextPointStepMultiplier` — *"After plane catch a pont, the next
point will be placed with this offset"* (their typo) — **[inferred]** sets how far
along the route the next progress point lands once one retires.

### 3.3 The carrot, and the tooltip that explains the trade-off

**[DUMP]** `PlaneFlyPreset` — the single most useful line in the dump:

```csharp
[Range(0, 5)]
[Tooltip("How far ahead in time (this * currentSpeed) of current progress point plane should aim for flying.
Lesser values will force the plane to try following the fly-route more precisely, but it also may cause problems,
whereas higher values are safer.")]
public float TargetAheadFactor { get; set; }
```

**This is pure pursuit, and the developers wrote the trade-off down themselves.**
The aim point sits `TargetAheadFactor × currentSpeed` further along the route than
the aircraft's own progress — a lookahead **measured in seconds**, so the carrot
stretches automatically with speed.

**[inferred]** "May cause problems" is the known pathology of a short lookahead:
the aim point is close, heading error swings hard, and the aircraft either
oscillates about the path or is commanded into a turn its rate clamp cannot make.
A long lookahead cuts corners but never fights itself. Exposing it per airframe
is the honest answer — a nimble attack jet and a strategic bomber want different
values.

Note the lookahead is measured **along the route**, not through the world. A
world-space carrot would cut across the inside of every turn; an arc-length
carrot stays on the curve. That is what §2.3's parameterisation bought.

### 3.4 Terrain is a reactive override, not a planning constraint

**[DUMP]** `GROUND_CRASH_LINECAST_TIMELENGTH = 1.5f`,
`GROUND_CRASH_PREVENTION_INTENSITY = 100f`.

**[inferred]** One linecast **1.5 seconds ahead** — time again, not distance — and
on a hit, nose-up with a weight of 100 against whatever else is steering. The
route builders only call `CorrectPointsAltitude()` against `MapMetaData`; **they
do not path around terrain.** The plan assumes empty sky and buys back the one
case where that is false with a single raycast per aircraft per frame.

The main note's §5 shows what they pay for the ground units that cannot make that
assumption: a layered graph, a navmesh, and 16,056 logged exceptions.

---

## 4. Why it hits the strike point accurately

This is the question that matters for bombing, and the answer has three parts.
None of them is "the controller is well tuned".

### 4.1 The planner and the follower share one motion model

**[inferred]** Put §2.2 and §3.1 side by side:

| | Value |
|---|---|
| Follower's constraint | constant `ForwardSpeed`, turn rate clamped to `_currentYawRate` |
| Planner's assumption | circles of radius `GetRotationRadius(turnRate, forwardSpeed)` |

**These are the same constraint, and that is the entire accuracy story.** The
route is not "a nice curve we hope the aircraft can follow" — it is *the locus of
positions this exact vehicle model can occupy*. The follower is never commanded
into a corner it cannot make, so the steering loop operates in its linear region
permanently and residual error comes only from lookahead corner-cutting (§3.3),
the one-degree circle discretisation (§2.2), and Catmull-Rom rounding (§2.3) —
all bounded, all tunable, none accumulating.

**The usual reason waypoint following is inaccurate is a plan/execute model
mismatch**: a path planner that knows nothing about turn radius emits corners the
vehicle physically cannot take, and the controller spends its life recovering from
impossible commands. No amount of PID tuning fixes that, because the error is not
a tuning problem — the reference itself is infeasible. Broken Arrow removes the
mismatch instead of compensating for it. **That is the transferable lesson and it
is worth more than any of the specific constants in this note.**

### 4.2 Weapon release is scheduled in arc length, not world position

**[DUMP]** Every fire-control query on a route takes a **progress float**:

```csharp
// WaypointBombing
public Vector3 GetBombDropPoint(float bombingProgress);
public int     MainTrajectoryStartIndex { get; }

// WaypointStrafe
public Vector3 GetFrameFireTarget(float strafeProgress);
public int     DiveStartIndex { get; }
public float   StrikeLength   { get; set; }

// BaseWaypointBuilder
public bool  InTargetZone(float progress);
public bool  IsFinished  (float progress);
protected float RouteFirstTargetDistance  { get; }
protected float RouteSecondTargetDistance { get; }
```

and the consumers take the route object plus a progress index, never a world
position:

```csharp
// AircraftWeaponSystem
public  void BombingRun(Entity planeEntity, WaypointBombing bombFlyPath);
private void StrafeRun (Entity planeEntity, WaypointStrafe strafeFlyPath,
                        int flyProgresIndex, float deltaTime);
private const float STRAFE_END_EXTRA_TIME = 0.33f;
public static void SetStrafeAmmoQuota(in UnitBattleInfoSetComponent, in AmmunitionBoxComponent, bool state);
```

**[inferred] The release decision never asks "how close am I to the target".** It
asks "how far along this route am I", compares against
`RouteFirstTargetDistance`, and releases. Because `CalculateDistances` put the
strike point on the route at a known arc length, and the route passes through the
player's clicked point by construction, **the timing is exact by arithmetic
rather than by proximity.**

That decoupling is what makes the feature survive. A proximity trigger has to
choose a radius: too tight and a slightly-off aircraft never fires at all; too
loose and it releases early. An arc-length trigger has neither failure. Residual
*lateral* error still displaces the bomb — §4.1 is what keeps it small — but the
*longitudinal* error, which is the one that decides whether you hit the building
or the street behind it, is zero.

### 4.3 Ballistic lead is folded into the plan, in seconds

**[DUMP]** `WaypointBombing`'s constructor takes a `float timeOffset` and stores
`_weaponsTimeOffset`. `PlanesConfig` supplies it:

```csharp
[Tooltip("(CornerSpeed * this) = Trajectory distance offset from bombing start point, for Low-Drag(High-Altitude) bombs")]
public float BombingStartTimeOffset;
[Tooltip("(CornerSpeed * this) = Trajectory distance offset from bombing start point, for High-Drag(Low-Altitude) bombs")]
public float BombingStartTimeOffsetHighDrag;

[Tooltip("Minimal distance between bombs for low drag bombing")]     public float BombDropOffset;
[Tooltip("Minimal distance between bombs for high drag bombing")]    public float BombDropOffsetHighDrag;
[Tooltip("Distance between bombs = Bomb offset + (ammunition AoE * this)")]
public float BombDropOffsetAmmoAoeMult;

[Tooltip("(CornerSpeed * this) = Trajectory distance offset from strafe dive start point.")]
public float StrafeStartTimeOffset;
[Tooltip("[Min.WeaponRange - CornerSpeed * this] = Dive end-to-start distance offset to generate strafe weapon activation point.")]
public float StrafeWeaponPointOffset;
```

with `BombRunControlComponent { Dictionary<int,int> AlreadyThrownBombs; }` tracking
the stick as it goes.

**[inferred]** A bomb is ballistic: it must leave the aircraft *before* the
target. That lead is authored **in seconds** and multiplied by `CornerSpeed` into
an arc-length offset subtracted from the target's route distance. Two variants
because drag class changes the throw distance — low-drag bombs from altitude lead
much further than retarded bombs from the deck.

**Corner speed is the unit of account for the entire attack geometry.** An
airframe that turns slower automatically gets a longer run-in and an earlier
release without anyone re-tuning the attack. Bomb *spacing* is likewise derived —
`BombDropOffset + ammoAoE × BombDropOffsetAmmoAoeMult` — so a stick of large
bombs spreads itself. This is good data design and it is why the numbers stay
sane across a roster of dozens of aircraft.

### 4.4 What the player actually perceives

**[inferred]** Worth stating plainly, since the frustration the question names is
a UX outcome rather than a maths one. Three properties combine into "the plane
did what I asked":

1. **The run-in is visible and committed.** The route is drawn (`DrawFlypath`),
   the aircraft flies a wide, obviously-deliberate arc, and §5's hysteresis stops
   it second-guessing. The player can predict the attack, which is most of the
   feeling of control.
2. **The release is longitudinally exact** (§4.2), so ordnance lands on the
   clicked point rather than short or long — the error direction players notice
   most.
3. **A physically impossible order fails visibly rather than silently.**
   `AircraftHelper.IsTooCloseForManeuver(…)` in the alpha, and retail's
   `PlanesConfig.RadiusMultForTooCloseCheck` + `AirCombatSystem.TOO_CLOSE_CHECK_RADIUS_MULT
   = 2`, test the target against a multiple of the turn radius. A target inside
   that gets a go-around, not a botched pass. **The aircraft admits it cannot make
   the turn instead of trying and missing** — which reads as competence, whereas
   a missed pass reads as a bug.

---

## 5. Sudden order changes

The second half of the question, and it is handled in four distinct places.

### 5.1 Routes are rebuilt in place

**[DUMP]** Every builder has an update method rather than requiring
reconstruction:

```csharp
WaypointBombing.UpdatePoints(Vector3 startPosition, Vector3 endPosition, float altitude, Transform planeTransform);
WaypointStrafe .UpdatePoints(Vector3 startPosition, Vector3 endPosition, float altitude);
WaypointCircle .UpdatePoints(float altitude, float radius);
WaypointSinglePoint.UpdatePoint(Vector3 position, float altitude);
IPlaneFlyRoute .ChangeAltitude(float targetAltitude);
```

**[inferred]** `FlyPoints` / `ToPointDistances` are fields, and
`TangentWaysBuilder`'s four circle arrays are `readonly` and preallocated in its
constructor. **A re-plan overwrites arrays rather than allocating them**, so
re-ordering a flight of aircraft mid-attack does not spike the collector. Given
the main note's §1 finding that `boot.config` sets `gc-max-time-slice=3` — an
explicit GC budget, the tell that pauses hurt — that is a deliberate choice, not
an accident.

`ChangeAltitude` being on the interface itself **[inferred]** means the common
case of "same route, different height" never re-solves the geometry at all.

### 5.2 A new route must be clearly better to win

**[DUMP]** `WaypointStrafe`:

```csharp
private const float SHORTWAY_LENGTH_MIN_DIFF_RATIO = 1.5f;
private const float MAX_DIVE_ENTER_ANGLE           = 25f;
private const float MIN_CHECKANGLE_STRAIGHT_STRIKE = 40f;
private const float MIN_CHECKANGLE_ROTATION        = 30f;
private readonly float _rotationCheckDot;    // cos(30°)
private readonly float _straightStrikeDot;   // cos(40°)

[CompilerGenerated]
private TangentWayInfo <UpdatePoints>g__GetBestWayToPoint|27_0(
    Vector3 targetPoint, Vector3 strafeDirection, ref <>c__DisplayClass27_0 );
```

plus `AircraftHelper.MAX_WIDE_CHECKANGLE_MULT = 2`.

**[inferred] `SHORTWAY_LENGTH_MIN_DIFF_RATIO = 1.5` is explicit re-plan
hysteresis**: among the candidate tangent routes, a shorter one only displaces
the incumbent if it is **1.5× shorter**, not merely shorter. Two approaches of
near-equal length — which is exactly what a target drifting a few metres produces
— cannot swap.

**This is CLAUDE.md's incumbency rule, in a shipped game, at a much larger
coefficient than the rule suggests.** The codebase's stated guidance is *"a bias
of a percent or so on what I chose last time"*; Broken Arrow uses **50%**. The
difference is instructive: a cover-selection flip costs one wasted move, whereas
flipping a bombing approach throws away a ten-second run-in and the player sees
an aircraft dither over a target. **The right size of an incumbency bonus scales
with what re-deciding costs, not with how noisy the score is.** That is worth
carrying back into the rule.

The banded angles do related work. `MIN_CHECKANGLE_ROTATION = 30°`,
`MIN_CHECKANGLE_STRAIGHT_STRIKE = 40°` and `MAX_DIVE_ENTER_ANGLE = 25°` sort a new
target into *straight in*, *needs a turn*, or *needs a full circuit*, with the
dots precomputed. **Bands are inherently hysteretic** — a target has to cross a
threshold to change the answer, where continuous re-evaluation would jitter at
every boundary.

### 5.3 Commit guards near the target

**[DUMP]** `PlanesConfig`:

```csharp
[Range(5, 45)]
[Tooltip("Planes will never place fly steering point away from current strike target if angle to it is less than this.")]
public float PrecisionMinAllowedAngle;

[Tooltip("Extra distance from precision strike point when calculation away maneur distance")]
public ushort PrecisionFlyAwayExtraDistance;

[Tooltip("If closer than this, plane is allowed to change target prediction state to escape maneur
(prevents occasional buggy state when planes dogfight)")]
public float PlaneVsPlaneEscapeDistance;
```

**[inferred]** `PrecisionMinAllowedAngle` is the strongest guard: once inside that
cone of the target, **stop generating new approach geometry and commit.** Without
it a target that drifts slightly would trigger a fresh Dubins solve on short
final, and the aircraft would break off an attack it was about to complete —
precisely the frustration the question anticipates. The tooltip's *"will never"*
says it is a hard gate, not a cost term.

`PlaneVsPlaneEscapeDistance` ships with its bug in the tooltip — *"prevents
occasional buggy state when planes dogfight"* — two aircraft inside each other's
turn circles ping-ponging between pursuit and escape, fixed with a distance
hysteresis.

### 5.4 The new route starts where the aircraft will be

**[DUMP]** `GeneratePlaneCircles(Vector3 planePos, Vector3 planeForward, int offset)`.

**[inferred]** The `offset` walks the starting point forward along the existing
route before the new circles are placed. A re-plan anchored at the aircraft's
*current* position is stale by the time it is installed — by a frame at best, and
by however long the solve took at worst — and the aircraft would begin the new
route already behind it, with a lateral error it must then work off. Offsetting
ahead means the new curve meets the aircraft where it is going to be, so the
handover is continuous in position **and** heading.

**[inferred] Continuity of heading is the part that matters.** A re-plan that
preserves position but not heading commands an instantaneous turn, which the rate
clamp converts into a visible lurch. Building the new route tangent to the
aircraft's *current* forward vector (which `GeneratePlaneCircles` takes as its
second argument) makes the join smooth by construction — the same reason the
original route was built tangent to the heading in the first place.

---

## 6. Speed: it slows down to turn, in three separate ways

Speed management is **authored data**, not a physical consequence.

### 6.1 `CornerSpeed`

**[DUMP]** `PlaneFlyPreset`, tooltips verbatim:

```csharp
[Tooltip("Minimal speed a plane can fly at")]                    MinSpeed
[Tooltip("Maximum speed a plane can stay at while turning")]     CornerSpeed
[Tooltip("Maximum speed without turning afterburner on")]        MaxSpeed
[Tooltip("Maximum speed with an afterburner turned on")]         AfterburnSpeed
                                                                 Acceleration, AfterburnerAcceleration
                                                                 Deceleration, AfterburnerDeceleration
```

The concept is borrowed from real aviation — corner speed is where sustained turn
rate peaks — but here it is **a number a designer types**, not a lift equation.
`AircraftControl` caches `_afterburnerCornerSpeed` at construction, so it is read
often enough to hoist. §4.3 showed the whole attack geometry is denominated in
it.

### 6.2 `DecreaseSpeedAngle`

**[DUMP]** `PlanesConfig`:

```csharp
[Tooltip("When Angle between the aircraft and target will be more this value, plane starts decrease speed")]
public float DecreaseSpeedAngle;
```

**[inferred]** The literal answer to "does it slow down to turn accurately": yes,
on a **heading-error threshold**. Beyond it, throttle comes back; and because §6.4
makes turn rate *worse* at speed, slowing tightens the turn, which closes the
angle, which restores throttle. A stable negative-feedback loop from one threshold
and one curve — no gains, no windup.

The mover composes speed by layering:

```csharp
private void SetForwardSpeed    (Entity, in AircraftControlComponent, float deltaTime);
private void ModifyDogfightSpeed(Entity planeEntity, ref float targetSpeed);
private void ModifyStrafeSpeed  (in AircraftControlComponent, ref float targetSpeed);
```

**[inferred]** A base target speed with per-context adjustments applied by `ref` —
each manoeuvre owns its rule. `PlaneFlyPreset.StrafeSpeedRatio` feeds the second:
*"Speed plane will try to maintain during the strafe. Its a ratio between Min-to-Max
speed where 0 is min and 1 is max speed."* Authoring it as a **ratio** is why one
strafe tuning works for a prop attacker and a fast jet alike.

### 6.3 Dogfighting has its own rules

**[DUMP]** `AirCombatSystem`:

```csharp
public  const float DOGFIGHT_SPEEDCONTROL_RANGE        = 500f;
private const float DOGFIGHT_ATTACK_ANGLE              = 45f;
private const float DOGFIGHT_BACK_ANGLE                = 30f;
private const float DOGFIGHT_AFTERBURN_EXTRA_ALTITUDE  = 20f;
private const float DOGFIGHT_GROUND_CRASH_ALTITUDE     = 90f;
private const float TOO_CLOSE_CHECK_RADIUS_MULT        = 2f;
private const float UP_TURN_RADIUS_MODIFIER            = 0.75f;
private const float PLANE_VS_HELI_TARGET_PREDICTION_MULT = 1f;
private const float PLANE_VS_HELI_DIVE_END_DISTANCE    = 100f;
private const float HELI_ATTACK_FLY_ANGLE              = 45f;

private void VersusGroundTarget(Entity, Entity, float minRange, float maxRange);
private void VersusHelicopter  (Entity, Entity);
private void VersusPlane       (Entity, Entity, float weaponsRange);
```

with `PlanesConfig.PlaneVsPlaneStopDistance` — *"Distance to enemy plane while
plane start to slow down for follow"* — and `AircraftControl._dogfightThrottleSqDistance`
(squared, cached).

**[inferred]** "Slow down to follow" is a **separate** rule from §6.2's: pursuing
a manoeuvring aircraft is not the same problem as flying your own route, and they
did not unify them. `UP_TURN_RADIUS_MODIFIER = 0.75` says a climbing turn is
credited a 25% tighter radius — trading energy for turn rate, the one piece of
real aerodynamic intuition in the system, and applied as a constant rather than
simulated.

### 6.4 Turn rate is an input, not an output

**[DUMP]**

```csharp
[Tooltip("Y axis rotation when plane is at min speed")]           MinSpeedYaw
[Tooltip("Y axis rotation when plane is at max speed")]           MaxSpeedYaw
[Tooltip("Y axis rotation when plane is at afterburner speeds")]  AfterburnYaw
[Tooltip("X axis rotation when plane is at min speed")]           MinSpeedPitch
[Tooltip("X axis rotation when plane is at max speed")]           MaxSpeedPitch
[Tooltip("X axis rotation when plane is at afterburner speeds")]  AfterburnPitch
```

```csharp
private float _currentYawRate, _currentPitchRate, _currentAngleToTarget;
private bool  _isManeuver;
private void  SetManeuverability();
```

**[inferred] This is the inversion the whole design rests on.** In a real aircraft
— and in Nuclear Option — turn rate is an *output*: you bank, the lift vector
rotates, and the aircraft turns at whatever rate physics allows. Here it is an
**input**, looked up from a three-point speed curve by `SetManeuverability()`.

Three consequences:

1. **Turn radius is knowable in closed form**, which §2.1 needed and a physics
   model could not supply cheaply.
2. **Faster is genuinely worse at turning**, so §6.2's slow-down actually buys a
   tighter turn.
3. **Rotation is rate-limited rather than error-corrected.** The only feedback is
   *which way to rotate*; the magnitude is data. **That is a P controller with a
   hard rate clamp, and the clamp does all the work** — no integral term because
   there is no steady-state error to accumulate, no derivative term because the
   rate is already bounded and cannot overshoot.

---

## 7. The roll you see is a lie

**[DUMP]** `PlaneFlyPreset`, `[Header("Local effects")]`:

```csharp
MaxRoll, NoiseSpeed, NoiseSize, LocalShakingDepth, LocalShakingSpeed,
NoseLiftStartSpeed, NoseLiftAngle
```

```csharp
private float _dotRight, _lerpRight, _noseLift, _perlinTime;
private void LocalRotation(Transform root, float deltaTime);
private void LocalShaking (Transform root);
```

and the component holds **two** transforms — `PlaneRoot` (visual) and
`PlaneTransform` (logical).

**[inferred] Bank angle is decoration on a separate transform and has no influence
on the turn.** The heading changed because `YawToTarget` rotated the logical
transform at a data-driven rate; `LocalRotation` then leans `PlaneRoot` by up to
`MaxRoll`, `_lerpRight` easing toward `_dotRight` (how hard the turn is, as a dot
against the right vector). `_perlinTime` with `NoiseSpeed`/`NoiseSize` adds
low-amplitude wander, `LocalShaking` adds buffet, and `NoseLiftStartSpeed` /
`NoseLiftAngle` pitch the nose up as speed bleeds, faking the higher angle of
attack a slow aircraft needs.

**Exactly backwards from a simulator and exactly right for an RTS.** In a
simulator the roll causes the turn; here the turn causes the roll. The
`PlaneRoot` / `PlaneTransform` split is what makes it safe: the visual can be
leaned, shaken and Perlin-wandered as far as looks good with none of it feeding
back into where the aircraft is. Simulation stays exact and reproducible — which
multiplayer needs — while presentation stays alive.

CLAUDE.md's *"desktop, not VR"* framing applies: at RTS camera distances nobody
can tell the bank is not aerodynamic, and buying it with a Perlin lerp costs
nothing and cannot destabilise anything.

---

## 8. The PIDs exist — on missiles

**[DUMP]** Three controllers in `BrokenArrow.Client.Ecs.Utils`:

```csharp
public class PIDController  { float _p, _i, _d; float _previousInputValue, _integralError;
                              float Process(float inputValue); }
public class PsIDController { float _p, _i, _d; float _previousValue, _integralError;
                              float Process(float inputValue); }        // near-duplicate
public class PIDSController { float _p, _i, _d, _s; float[] _previousInputValues;
                              float previousSmoothenedValue; float _integralError; int _n;
                              float Process(float inputValue); }        // + N-sample smoothing
```

**In the entire retail binary there is exactly one field of any of these types:**

```csharp
public struct MissileGuidanceInfoComponent {
    public readonly PIDController LoftAndCruisePID;   // ← the only consumer
    public TrajectoryType TrajectoryType;
    public readonly bool  LoftingTrajectoryUsed;
    public bool  CurrentlyLofting;
    public float ReferenceLoftHeight, LaunchHorizontalDistance, TimeToHit, DiveStartDistance;
    public readonly float MaxLeadSin, MaxLeadCos;
}
```

(The alpha had a second, `CruiseMissileComponent.PID`; retail merged loft and
cruise into one — hence the name.)

**[inferred] The split is principled.** A missile's loft profile is a *tracking*
problem — hold a commanded altitude against a moving target and a changing burn —
and that is what an integral term is for. An aircraft following a route built to
its own turn radius has **no persistent tracking error to integrate**: §4.1
removed it. A PID there would add a gain to tune, an integral to wind up, and
nothing to fix.

Three PID variants for one live use is the main note's §2 thesis in miniature —
utility code accumulates faster than it is deleted. `PsIDController` is a copy of
`PIDController` with one field renamed and no consumers at all.

**[inferred]** Note what the guidance is *not*: `MaxLeadSin` / `MaxLeadCos`
(precomputed, `readonly`) is a bounded lead-pursuit clamp.
[`nuclear_option_combat.md`](../nuclear_option/nuclear_option_combat.md) §2.4
makes the same finding about Nuclear Option's missiles and states it flatly — *"a
PID, and it is **not** proportional navigation."* Two independent teams chose a
PID over proportional navigation for the same job.

---

## 9a. Ground units: speed is a vector, and that changes everything

**[DUMP]** Everything that is not an aircraft shares a component set in
`BrokenArrow.Shared.Ecs.Components.Movement`:

```csharp
public struct SpeedComponent {
    public Vector3 Direction;      // 0x0   ← direction of TRAVEL
    public float   Value;          // 0xC
    public bool    IsReverse;      // 0x10
    public Vector3 Vector { get; }
}

public struct AccelerationComponent {
    public bool    Enabled;        // 0x0
    public bool    IsDeceleration; // 0x1
    public Vector3 Direction;      // 0x4
    public float   Acceleration;   // 0x10
    public float   Deceleration;   // 0x14
}

public struct MaxSpeedComponent {
    public bool  IgnoreModifiers, IgnoreSpeedCorrection;
    public float CurrentValue, MaxSpeedModifier;
    public readonly float MaxCrossCountrySpeed;  // 0x10
    public readonly float MaxSpeedRoad;          // 0x14
    public readonly float MaxSpeedReverseMove;   // 0x18
    public readonly float MaxSpeedWater;         // 0x1C
    public readonly float MaxSpeedForest;        // 0x20
    public Nullable<float> MaxSpeedLimit;        // 0x24
    public float Value { get; }
    public void .ctor(float maxFields, float maxRoad, float maxWater, float maxSpeedForest, float maxReverse);
}

public struct MaxRotationSpeedComponent { public float Value, Modifier; public float ModifiedValue { get; } }
public struct RotationSpeedComponent    { public Vector3 RotationAxis; public float Angle; }
public struct MovingComponent {}   // empty tag
public struct SprintAbilityComponent { public readonly float ActiveStateDuration, CooldownTime; … }
```

**[inferred] `SpeedComponent` carries its own `Direction`, and that single field is
the whole difference from the aircraft.** An aircraft's speed is a scalar along
its nose (§3.1b), so facing *is* heading. A ground unit's travel direction is
stored separately from its transform's facing, and `IsReverse` plus
`MaxSpeedReverseMove` mean **it can move backwards without turning round at all.**

That is the answer to "how does the object turn with the path" being different per
unit type, and it is a *representational* difference rather than a tuning one:

- **A nonholonomic body** (the aircraft) has no way to express "moving one way,
  facing another", so its path must be curvature-bounded and it must turn to go
  anywhere. Hence Dubins.
- **A holonomic-ish body** (ground vehicle, and more so infantry) can decouple the
  two, so its path may have corners and facing becomes a *separate, cosmetic-ish
  problem* solved by a rotation system running alongside movement.

**[DUMP]** That rotation system is tiny — and note precisely who it serves:

```csharp
[Without(new[] { typeof(MovingComponent) })]
[WithEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag) })]
[With(new[] { typeof(TransformComponent), typeof(RotationSpeedComponent), typeof(MaxRotationSpeedComponent) })]
public class RotateUnitSystem : AEntitySetSystem<float> {
    private const float MAX_ALLOWED_ANGLE_DIFF = 1;
    private static bool SetRotationSpeed(in Entity unitEntity, Quaternion targetRotation);
}
```

**[inferred]** A **1° deadband** below which no rotation is requested — the
standard anti-jitter guard, and the reason parked units do not twitch.
`SetRotationSpeed` converting a target quaternion into the axis-angle
`RotationSpeedComponent`, clamped by `MaxRotationSpeedComponent.ModifiedValue`,
is the **same rate-limited-P shape as the aircraft's `YawToTarget`**.

But read the filter: `[WithEither(UnitGroundVehicleFlag, UnitWaterVehicleFlag)]`
and `[Without(MovingComponent)]`. **This is turn-on-the-spot for stationary
ground and water vehicles only.** Infantry is excluded outright, and
[`broken_arrow_squads.md`](broken_arrow_squads.md) §3 explains why: a squad has
no body facing to command — its apparent facing is emergent from where its
soldiers are individually aiming — so `RotateCommand` has no infantry executor at
all. Four locomotion classes, four different answers to "which way am I
pointing", and this system serves one of them.

**[DUMP]** And the ground path follower has its own angle gates:

```csharp
public class NavigationSystem : EcsSetSystem<float> {
    private const float ROADS_CHECK_WIDTH                = 3.6f;
    private const float WIDE_ROADS_FASTMOVE_OFFSET       = 2.8f;
    private const float ALLOWED_ROTATION_ANGLE_DELTA     = 30f;
    private const float TURN_ANGLE_TOLERANCE             = 45f;
    private const float FASTMOVE_NODESKIP_MAX_SQ_DISTANCE = 1600f;   // 40 m

    private static void SkipPathPoint(ref PathComponent pathCom);
    public  static Nullable<Vector3> GetRoadWidePoint(in PathComponent, MapMetaData, Nullable<int> indexOverride);
    private static void OnObstacleRaycast(Entity unitEntity, Span<RaycastHit> result);
}
```

**[inferred]** Two banded angles again — `ALLOWED_ROTATION_ANGLE_DELTA = 30°` and
`TURN_ANGLE_TOLERANCE = 45°` — sorting the heading error into "drive on", "turn
while driving" and "sort your facing out first". **This is where a slow vehicle
and a fast one genuinely differ**: with a shared angular rate and a differing
linear speed, the same 45° gate produces a tank that pivots before moving and a
truck that swings through it, without either being special-cased.

`FASTMOVE_NODESKIP_MAX_SQ_DISTANCE = 1600` (40 m) with `SkipPathPoint` is
**corner-cutting on demand**: in fast-move mode, path nodes within 40 m are
dropped rather than driven to, so a column on a road stops zig-zagging between
graph vertices. `GetRoadWidePoint` and `ROADS_CHECK_WIDTH = 3.6` (a lane) offset
the unit onto the correct part of a wide road — **the path says which road, the
follower decides where in it**, which is the same planner/follower division of
labour §4.1 praises, arrived at for a completely different reason.

---

## 9. Helicopters are a completely different system

**[DUMP]** `HelicopterNavigationSystem` lives in
`BrokenArrow.Client.Ecs.Navigation.Systems` — a different namespace — and shares
no type with the fixed-wing stack:

```csharp
private const float DEFAULT_ACCELERATION_STEP     = 0.25f;
private const float CLIMB_ACCELERATION            = 7.5f;
private const float PASSIVE_DECELERATION          = 0.2f;
private const float MAX_Y_PLANE_ANGLE             = 25f;
private const float MIN_LOOKROTATION_ANGLE        = 30f;
private const float MINIMAL_REACH_POINT_DISTANCE  = 7.5f;
private const float SLOWPOKE_AGILITY_BORDER       = 15f;
private const float STATIC_MANEURS_MAXSPEED_RATIO = 0.125f;

private readonly IRaycastBatchService _raycastService;
private void  HelicopterPixelHeightScan(in MapPixel, ref FlyHeightData, int x, int y);
private float GetTiltAngle(float accelerationRatio);
private static Vector3 ShiftAcceleration(in Vector3 current, in Vector3 target,
                                         float agility, float deltaTime);
```

| | Fixed wing | Helicopter |
|---|---|---|
| Order becomes | a Dubins route | a target **acceleration vector** |
| State | `ProgressDistance` along a curve | velocity + acceleration, slewed |
| Turn constraint | radius from turn rate | `MaxRotationSpeedComponent` |
| Waypoint acceptance | **80° angle test** | **`MINIMAL_REACH_POINT_DISTANCE = 7.5 m`** |
| Terrain | one linecast 1.5 s ahead | `IRaycastBatchService` + per-pixel height scan |
| Lean | cosmetic (`MaxRoll` + Perlin) | `GetTiltAngle(accelerationRatio)` — **from real acceleration** |

**[inferred]** A helicopter can hover, stop and reverse, so a curvature-bounded
route is meaningless for it — and note it accordingly reverts to a **distance**
acceptance test, because with no minimum speed there is no overshoot to guard
against. `FlyHeightData` aggregating `MaxSurfacePixel`, `MaxVolumedPixel` and
`MaxBridgeHeight` says nap-of-the-earth flight must clear terrain, buildings *and*
bridges — a far harder terrain problem than the fixed wing's, because the
helicopter is down among the geometry.

The main note's §4.2 read the movement split as ECS-idiomatic. **[inferred]**
Reading the implementations, it is stronger than idiom: these two share no state,
no representation and no acceptance rule. Merging them would produce a type with
two disjoint halves.

---

## 10. Where orders come from

**[DUMP]** The scripting layer reaches aircraft through the node graph the main
note's §8 describes — `NodeMoveComplex : BaseWaypoint`, `NodeSimpleMove`,
`NodeFollowWaypoint`, `NodeFindWaypoint`, `NodeGetWaypoint`, `NodeAttackEnemy` —
alongside a general `WaypointSystem`, `WaypointComponent` and
`NextWaypointComponent` used by ground units.

**[inferred]** So **two unrelated things are called "waypoint"**: the order queue
(`WaypointComponent` / `WaypointSystem`, all unit types) and the fixed-wing route
geometry (`BaseWaypointBuilder` / `IPlaneFlyRoute`). The aircraft path converts
the first into the second, and `AircraftHelper.FlyToPoint(…, bool flyThroughPoint
= true)` is the seam — that default argument is the entire distinction between
"arrive here" and "pass through here on the way".

---

## 11. Read against Nuclear Option

The two games answer this question at opposite poles, for a structural reason.

| | **Nuclear Option** | **Broken Arrow** |
|---|---|---|
| What the AI produces | `ControlInputs` — six floats, *the same struct a human stick writes* | a route, then a transform update |
| Steering | PIDs (`AutopilotPlane.AutoAim`, `AutopilotHelo`, `tiltPID`, shipped `PIDTuner`) | rate-limited rotation toward an arc-length lookahead |
| Turn rate | an **output** of the flight model | an **input** from a speed curve |
| Turn radius | emergent | `GetRotationRadius(turnRate, speed)`, closed form |
| Route | `PathfindingAgent` + steer point, replanned continuously | Dubins tangents + Catmull-Rom, solved once, guarded re-plan |
| Slowing for corners | `k = max((nextWaypointAngle - 10) * 0.1, 0.1)`; >60° off → crawl | `CornerSpeed` + `DecreaseSpeedAngle`, authored |
| Lookahead | carrot pushed 20 m past the waypoint toward the next | `TargetAheadFactor × speed`, **along arc length** |
| Waypoint acceptance | distance **and** "it is behind me" | 80° angle test |
| Weapon release | flight model + seeker, continuous | **arc-length trigger on the route** |
| Bank | causes the turn | caused by the turn, on a separate transform |

**[inferred] Nuclear Option cannot make Broken Arrow's choice.** Its stated design
— [`nuclear_option_control.md`](../nuclear_option/nuclear_option_control.md) §2 —
is that the AI writes the same six floats the player does, so *"any improvement to
the flight model improves the AI's flying for free."* That forces a controller:
the only interface to the aircraft is stick, throttle and rudder, so reaching a
waypoint **must** be expressed as error → correction, and a PID is right.

**Broken Arrow has no player in the cockpit**, so aircraft position is
authoritative state it may write directly. Given that freedom, a solved route is
strictly better: it cannot oscillate, cannot overshoot, costs nothing per frame
after the solve, and is **exactly reproducible**, where a PID's accumulated
integral is a per-client divergence waiting to happen. The main note's §7 records
that Broken Arrow abandoned lockstep, but cheap determinism is still worth having.

**The lesson is not "Dubins beats PID".** It is that *the interface to the vehicle
decides the technique.* Expose control surfaces and you need a controller; expose
the transform and you should plan a curve. Choosing the interface is the actual
design decision.

---

## 12. What transfers

Judged against RTS / FPS / third-person, as CLAUDE.md requires:

1. **Make the planner and the follower share one motion model.** (§4.1.) This is
   the headline. Most waypoint inaccuracy is a plan/execute mismatch — a planner
   that ignores turn radius emits corners the vehicle cannot take, and no
   controller tuning fixes an infeasible reference. Generate the path *with* the
   follower's constraint and the tracking problem largely disappears. **Applies to
   any steered agent in any genre: vehicles, aircraft, and any FPS/third-person AI
   that cannot turn on the spot.**

2. **Trigger events on arc length, not proximity.** (§4.2.) Weapon release,
   animation cues, sound, script triggers — key them to distance along the path
   and they cannot fire early, late, or not at all. A proximity radius has to
   trade "never triggers" against "triggers early"; arc length has neither
   failure.

3. **Author offsets in time, convert to distance at use.** (§3.3, §4.3.)
   `TargetAheadFactor × speed`, `CornerSpeed × k`, a 1.5 s terrain linecast — one
   authored number that self-scales across the whole speed range. **The single
   most transferable habit here.**

4. **Retire a waypoint on angle, not distance.** (§3.2.) Three lines, and it
   permanently removes orbit-forever. Nuclear Option needed the same guard. Note
   the converse from §9: a vehicle that can stop should use distance, because it
   has no overshoot to guard against.

5. **Size an incumbency bonus by what re-deciding costs.** (§5.2.)
   `SHORTWAY_LENGTH_MIN_DIFF_RATIO = 1.5` is 50%, where CLAUDE.md's rule suggests
   "a percent or so". Both are right — a cover flip wastes one move, an aborted
   bombing run wastes ten seconds and looks broken. **Worth folding back into the
   hot-loop rule: the coefficient scales with the cost of switching, not the noise
   in the score.**

6. **Add a hard commit gate near the goal.** (§5.3.) `PrecisionMinAllowedAngle` —
   *"will never place fly steering point away"* — is a gate, not a cost term.
   Anything with a long committed approach (a charge attack, a landing, a docking)
   needs one, or a target twitch on short final throws the whole approach away.

7. **Re-plan from where the agent will be, tangent to its current heading.**
   (§5.4.) Preserving position but not heading commands an instant turn that a
   rate clamp turns into a visible lurch.

8. **Give the visual its own transform.** (§7.) `PlaneRoot` vs `PlaneTransform`
   lets bank, buffet and wander be as expressive as the artist wants with zero
   risk to simulation state. **Highest value-per-line item here, and it transfers
   to every genre** — recoil, lean, suspension, all of it.

9. **Denominate derived tuning in one authored quantity.** (§4.3.) Every attack
   offset is `CornerSpeed × k`, so a new airframe's geometry falls out of one
   number instead of a re-tune.

10. **Reject impossible orders visibly.** (§4.4.) Testing the target against a
    multiple of the turn radius and flying a go-around reads as competence; a
    botched pass reads as a bug.

11. **Split movement by locomotion when the state genuinely differs.** (§9.) The
    test is not "are both flying things" but "would the merged type have two
    disjoint halves".

---

## 13. What is not established

- **No method bodies.** How `UpdateMove`, `SetManeuverability` or the progress
  tracker's `Update` sequence their work is inference from names, constants and
  tooltips. The Dubins reading of `TangentWaysBuilder` is strongly supported by
  its members but has not been read as code.
- **No authored values.** `CornerSpeed`, `TargetAheadFactor`, `DecreaseSpeedAngle`
  and the yaw/pitch curves live in `BrokenArrowDB.bytes` and the `PlanesConfig`
  asset, not the binary. The *shape* is established; none of the numbers are.
- **The arc-length release argument is inference from signatures.** That
  `GetBombDropPoint`, `InTargetZone` and `GetFrameFireTarget` all take a progress
  float, and that `AircraftWeaponSystem` takes a route plus a progress index and
  never a world position, is literal. That the release compares against
  `RouteFirstTargetDistance` is the natural reading, not a read one.
- **`SHORTWAY_LENGTH_MIN_DIFF_RATIO` is only proven to exist in `WaypointStrafe`.**
  Its use as the general re-plan rule is inference from the name.
- **Nothing was run or profiled.** No claim about cost, frame time, or how many
  aircraft the system supports.

**To go further**, the next step is `BrokenArrowDB.bytes` — referenced by string
literal, and `PlaneFlyPreset : IDataBaseModel` carries `[PrimaryKey]`
`[AutoIncrement]`, so it is a SQLite database with a `PlaneFlyPreset` table.
Reading it turns every "the shape is X" claim above into a tuned number per
airframe, and would say directly what `TargetAheadFactor` is for a MiG-29 versus a
B-52.

---

## Sources

**[DUMP]** Il2CppDumper v6.7.46 over
`C:\Program Files (x86)\Steam\steamapps\common\broken_arrow\GameAssembly.dll` and
`BrokenArrow_Data\il2cpp_data\Metadata\global-metadata.dat` (IL2CPP metadata v31,
files dated 2025-08-06). Method bodies are native and absent; types, fields,
offsets, signatures, `const` values and Unity attributes are literal.
Corroborating alpha dump at `E:\Il2CppDumper\dump.cs`.

Cross-referenced against
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) §6–8,
[`nuclear_option_control.md`](../nuclear_option/nuclear_option_control.md) §2, and
[`nuclear_option_combat.md`](../nuclear_option/nuclear_option_combat.md) §2.4 —
all **[CODE]**, decompiled C# with bodies, and therefore outranking anything
inferred here where the two disagree.
