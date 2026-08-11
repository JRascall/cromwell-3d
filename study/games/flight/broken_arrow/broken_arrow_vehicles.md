# Broken Arrow — ground vehicles: there is no such thing as a tracked vehicle

The third locomotion note, completing the set with
[`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) and
[`broken_arrow_squads.md`](broken_arrow_squads.md). The question is how a
modern-warfare RTS simulates tracked versus wheeled movement — and the answer is
that it doesn't, in a way that turns out to be the interesting part.

> **The simulation has no concept of a chassis type.** No `IsTracked`, no
> `ChassisType`, no propulsion enum. A case-insensitive sweep of all 1,757,307
> lines for `Tracked`, `Wheeled`, `Caterpillar`, `ChassisType` and `Propulsion`
> returns radar-tracking flags, a two-column join table, and Unity's XR
> `TrackedDeviceRaycaster`. Nothing else.
>
> **The difference is a speed ratio.** `Mobility` gives every vehicle a
> `MaxSpeedRoad` and a `MaxCrossCountrySpeed`. A truck has a large gap between
> them; a tank has a small one. **That gap *is* the wheeled/tracked distinction**,
> together with `TurnRate`. No code branches on it, and it emerges as behaviour
> because the pathfinder biases toward roads and the speed selector samples the
> terrain under the unit.
>
> **One `Mobility` table serves land, sea and air.** The same row type carries
> `MaxCrossCountrySpeed` and `MaxSpeedWater` *and* `IsAfterburner`,
> `LoiteringTime` and a `PlaneFlyPreset` reference.
>
> **Visually, tracks are a scrolling texture and wheels are rotated transforms** —
> and one `BoneContainer` holds `Wheels`, `Suspension` *and* `Tracks` side by side,
> so a vehicle is "tracked" if and only if the artist assigned a track transform in
> the prefab. `Tracks.HasTracks` is the only test in the game, and it is a
> presence check.
>
> **The suspension is driven by noise, not by the ground.** `SuspensionElement`
> holds `_lastRandomPosition`, `_updateTime` and `_maxStroke` — it never raycasts
> under a wheel.

> **Source and its limits.** **[DUMP]** from the retail Il2CppDumper output; see
> [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) for the full method note.
> Method bodies are native and absent, so control flow is **[inferred]**; type
> names, fields, offsets, signatures, `const` values, ECS filters and Unity
> attributes are literal.

Related: [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §9a (the shared
ground-movement component set this note builds on),
[`broken_arrow_squads.md`](broken_arrow_squads.md) §2 (infantry through the same
navigation stack),
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) (the
visual layer in depth, across four builds of this genre — §7 below only records
what the structured dump adds to it),
[`broken_arrow.md`](broken_arrow.md) §5.

---

## 1. What `Mobility` actually contains

**[DUMP]** One database row type for every moving thing in the game:

```csharp
public class Mobility : IDataBaseModel {
    public const float MS_TO_KM_PER_HOUR  = 3.6;
    public const float KM_PER_HOUR_TO_MS  = 0.2777778;

    public int    Id, UnitId?;                     // via UnitPropulsions
    public string Name, ModelFileName;
    public bool   IsDefault;
    public bool   IsAmphibious;                    // 0x29
    public bool   IsAirDroppable;                  // 0x2A
    public int    Weight;                          // 0x2C
    public int    HeavyLiftWeight;                 // 0x30
    public float  TurnRate;                        // 0x34
    public float  Acceleration;                    // 0x38
    public float  MaxCrossCountrySpeed;            // 0x3C
    public float  MaxSpeedRoad;                    // 0x40
    public float  MaxSpeedReverse;                 // 0x44
    public float  MaxSpeedWater;                   // 0x48
    public float  Agility;                         // 0x4C
    public float  ClimbRate;                       // 0x50
    public bool   IsChangeAltitude;                // 0x54
    public float  LoiteringTime;                   // 0x58
    public bool   IsAfterburner;                   // 0x5C
    public float  AfterBurningLoiteringTime;       // 0x60
    public int    FlyPresetId;                     // 0x64
    public PlaneFlyPreset FlyPreset;               // 0x68
}
```

joined to units by a pure link table:

```csharp
public class UnitPropulsions : IDataBaseModel {
    public int Id { get; set; }
    public int UnitId { get; set; }
    public int MobilityId { get; set; }
}
```

**[inferred] Three readings.**

**There is no chassis field.** Not one. Whatever separates a T-90 from a Humvee in
this game, it is not a type tag — it is the values in `TurnRate`,
`MaxCrossCountrySpeed` and `MaxSpeedRoad`.

**The table is domain-agnostic.** `MaxCrossCountrySpeed` sits four fields away from
`IsAfterburner`, and `FlyPreset` — the entire Dubins/turn-rate airframe profile
from [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §2.1 — hangs off the
same row. So "mobility" means *how this unit moves*, whatever domain it moves in,
and the aircraft profile is a nullable extension of it rather than a parallel
system. That is a cleaner factoring than it first looks: the fields a jet ignores
are the fields a tank ignores, and neither needs to know the other exists.

**`UnitPropulsions` being a many-to-many join, not a foreign key on `Units`,** means
a unit can carry more than one mobility profile. **[inferred]** The obvious use is
`IsAmphibious` — a land profile and a water profile for the same hull — which is
also why `MaxSpeedWater` sits on `Mobility` rather than on a separate table.

The two unit-conversion constants are worth noting on their own: **`Mobility`
speeds are authored in km/h and converted to m/s at load.** Designers type the
number that appears on the unit card.

---

## 2. Terrain decides which speed applies

**[DUMP]** The runtime component set is the generic one shared with infantry
([`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §9a), and it has five
ceilings where `Mobility` has four:

```csharp
public struct MaxSpeedComponent {
    public bool  IgnoreModifiers;          // 0x0
    public float CurrentValue;             // 0x4
    public float MaxSpeedModifier;         // 0x8
    public bool  IgnoreSpeedCorrection;    // 0xC
    public readonly float MaxCrossCountrySpeed;  // 0x10
    public readonly float MaxSpeedRoad;          // 0x14
    public readonly float MaxSpeedReverseMove;   // 0x18
    public readonly float MaxSpeedWater;         // 0x1C
    public readonly float MaxSpeedForest;        // 0x20
    public Nullable<float> MaxSpeedLimit;        // 0x24
    public float Value { get; }
    public void .ctor(float maxFields, float maxRoad, float maxWater, float maxSpeedForest, float maxReverse);
}
```

**[inferred] `MaxSpeedForest` has no `Mobility` column**, so it is derived at
construction — a forest penalty applied to cross-country speed rather than
authored per vehicle. The constructor taking `maxSpeedForest` as its own argument
while the database does not supply one is the tell.

What the vehicle is standing on is sampled on a timer, not per frame:

```csharp
public struct TerrainTypeComponent {
    public TerrainType Value;      // 0x0
    public float       CheckDelay; // 0x4
    public bool        IsLastCheck;// 0x8
}

public enum TerrainType : byte {
    None=0, Vegetation=1, Forest=2, Buildings=3, HighSlopes=4, Water=5,
    Road=6, LowWater=7, Concrete=8, Smoke=15,
    FlagBridgeOne=16, FlagBridgeTwo=32, FlagBridgeThree=64, FlagBridgeFour=128
}
```

**[inferred] So the whole tracked/wheeled feel is this loop**: sample the terrain
under the unit on a delay, pick the matching ceiling out of
`MaxSpeedComponent`, and let `AccelerationComponent` walk the current speed toward
it. A truck with `MaxSpeedRoad` far above its `MaxCrossCountrySpeed` visibly
slows the moment it leaves tarmac; a tank with the two close together barely
changes. **Nobody wrote "wheeled vehicles are worse off-road" — it falls out of two
numbers and a terrain sample.**

Note `Smoke = 15` sharing the enum with ground surfaces, and the four
`FlagBridge*` values being **bit flags** (16/32/64/128) while the surfaces are
ordinals (0–8). **[inferred]** The low byte is "what am I standing on", the high
nibble is "which bridges am I on" — packed into one byte because a unit can be on
a bridge *and* on concrete, but cannot be on forest *and* water.

---

## 3. Roads are a pathfinding cost, not a movement mode

**[DUMP]** The graph is layered and costs are per query:

```csharp
public enum NavMeshAreas {
    None=0, Everything=-1, Walkable=1, NotWalkable=2, Jump=4,
    Road=8, Forest=16, HighSlopes=64, Water=512, Buildings=1024
}

[Flags] public enum NavMeshMetaLayers : byte {
    NotWalkable=0, Walkable=1, Road=2, Forest=4, HighSlopes=8, Water=16, Buildings=32
}

public struct LayerData : IComparable<LayerData> {
    [ProtoMember(1)] public readonly float Cost;            // 0x0
    [ProtoMember(2)] public readonly int   SingleLayerMask; // 0x4
    public void .ctor(byte layer, float layerCost);
}

public struct PathQuerySettings {
    public const int DEFAULT_ITERATION_COUNT = 20000;
    public readonly bool UseLayerCosts;         // 0x0
    public readonly bool SphereNodeSearch;      // 0x1
    public readonly int  SearchMask;            // 0x4
    public readonly int  MaxIteration;          // 0x8
    public readonly PathAlgorithm AlgorithmToUse;               // 0xC
    public readonly ReadOnlyMemory<LayerData> CostOverrides;    // 0x10
}
```

and the per-unit overrides ride on the pathfinding component:

```csharp
public class PathFindingComponent {
    public bool AutoRepath, ForceRepath, IsObstacleRepath, NavMeshOnlyRequest;
    public readonly ReadOnlyMemory<LayerData> CostOverrides;   // 0x38
}
```

with a global thumb on the scale:

```csharp
public const float COST_OVERRIDES_ROAD_BIAS = 1.2;    // NavigationConstants
```

**[inferred]** `LayerData` being `[ProtoMember]`-tagged means cost overrides are
**serialised and sent over the wire** — they are part of unit definition data, not
a local heuristic. So a unit type can be authored to hate forest or love roads,
and the path it picks reflects the vehicle rather than the map alone. That is the
second half of the wheeled/tracked expression: a truck both *drives* slower
off-road and *routes* around it.

`ReadOnlyMemory<LayerData>` rather than a dictionary, with `LayerData : IComparable`,
**[inferred]** says the overrides are a small sorted span scanned linearly — the
right shape for three or four entries consulted inside a graph search, and exactly
the "no hash lookups in the hot loop" discipline CLAUDE.md asks for.

### 3.1 Road following is a separate, geometric step

**[DUMP]** From `NavigationSystem` (ground and water vehicles only):

```csharp
private const float ROADS_CHECK_WIDTH                 = 3.6f;
private const float WIDE_ROADS_FASTMOVE_OFFSET        = 2.8f;
private const float FASTMOVE_NODESKIP_MAX_SQ_DISTANCE = 1600f;   // 40 m
public static Nullable<Vector3> GetRoadWidePoint(in PathComponent pathCom,
    MapMetaData mapMeta, Nullable<int> indexOverride);
private static void SkipPathPoint(ref PathComponent pathCom);
```

**[inferred] `ROADS_CHECK_WIDTH = 3.6` is a lane.** The path says *which road*;
`GetRoadWidePoint` then decides *where in it* — offsetting the vehicle onto its own
side so a column does not drive down the centre line, and `WIDE_ROADS_FASTMOVE_OFFSET
= 2.8` widens that offset in fast-move mode so a convoy can overtake.

`FASTMOVE_NODESKIP_MAX_SQ_DISTANCE = 1600` with `SkipPathPoint` drops path nodes
within 40 m during a fast move, so vehicles stop zig-zagging between graph
vertices on a straight road. **[inferred] Both are the same idea as the aircraft's
`TargetAheadFactor`** ([`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §3.3)
— *the planner produces a corridor, the follower decides the line through it* —
arrived at independently for a completely different vehicle class.

---

## 4. What the vehicle does with a speed and a heading

**[DUMP]** The integrated state, shared with infantry:

```csharp
public struct SpeedComponent {
    public Vector3 Direction;   // 0x0   ← travel direction, separate from facing
    public float   Value;       // 0xC
    public bool    IsReverse;   // 0x10
    public Vector3 Vector { get; }
}

public struct AccelerationComponent {
    public bool    Enabled;         // 0x0
    public bool    IsDeceleration;  // 0x1
    public Vector3 Direction;       // 0x4
    public float   Acceleration;    // 0x10
    public float   Deceleration;    // 0x14
}

public struct MaxRotationSpeedComponent { public float Value, Modifier; public float ModifiedValue { get; } }
public struct RotationSpeedComponent    { public Vector3 RotationAxis; public float Angle; }
public struct MovingComponent {}   // empty tag, added by BaseMoveCommand.InitMovement
```

**[inferred]** `SpeedComponent.Direction` existing separately from the transform's
facing, plus `IsReverse` and `MaxSpeedReverseMove`, is the whole reason a ground
vehicle is not a Dubins car: **it can move backwards without turning round.** An
aircraft cannot express that — its speed is a scalar along the nose
([`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §3.1b).

Turning while stationary is its own system, and its filter is the definition of
"a vehicle":

```csharp
[Without(new[] { typeof(MovingComponent) })]
[WithEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag) })]
[With(new[] { typeof(TransformComponent), typeof(RotationSpeedComponent), typeof(MaxRotationSpeedComponent) })]
public class RotateUnitSystem : AEntitySetSystem<float> {
    private const float MAX_ALLOWED_ANGLE_DIFF = 1;
    private static bool SetRotationSpeed(in Entity unitEntity, Quaternion targetRotation);
}
```

**[inferred] `[Without(MovingComponent)]` is the pivot-on-the-spot case** — a
stationary vehicle given a facing order turns in place; a moving one is handled by
`NavigationSystem`'s angle gates instead. The 1° deadband is why parked columns do
not twitch.

And the gates themselves, against infantry for contrast:

| | `ALLOWED_ROTATION_ANGLE_DELTA` | `TURN_ANGLE_TOLERANCE` |
|---|---|---|
| `NavigationSystem` (ground/water) | **30°** | 45° |
| `InfantryNavigationSystem` | 10° | 45° |

**[inferred]** A vehicle tolerates being 30° off its steering point before
correcting; men tolerate 10°. With a shared 45° "this corner is too sharp"
threshold and a per-vehicle `TurnRate`, that produces a tank swinging wide and a
squad tracking the path tightly — **the same code, four numbers apart.**

`Agility` on `Mobility` has no obvious consumer in the navigation systems.
**[inferred]** It is most likely the input to `ShiftAcceleration(current, target,
agility, deltaTime)` in `HelicopterNavigationSystem`, alongside
`SLOWPOKE_AGILITY_BORDER = 15` — i.e. an air-domain field on the shared table,
mirroring `MaxSpeedWater` being a sea-domain field. Not established.

---

## 5. Avoidance: predictive, amortised, and settled by rank

**[DUMP]**

```csharp
[With(new[] { typeof(PathFindingComponent), typeof(TerrainTypeComponent), typeof(AvoidanceDataComponent) })]
[With(new[] { typeof(TransformComponent), typeof(SpeedComponent), typeof(AccelerationComponent), typeof(MaxSpeedComponent) })]
public class DynamicAvoidanceSystem : EcsSetSystem<float> {
    private const float UNIT_SEARCHBUFFER_MAX_UPDATETIME = 0.2;
    private const float COLLISION_PREDICTION_TIME_RADIUS = 5;
    private const float FRONTAL_COLLISION_MAX_ANGLE      = 15;
    private const float LINE_COLLISION_MAX_ANGLE         = 60;
    private const float TARGET_CHECK_SIZE_MULTIPLIER     = 2.2;
    private static bool UnitHasPriority(in Entity unitEntity, in Entity targetEntity);
}

public struct AvoidanceDataComponent {
    internal readonly bool IsInfantry, IsHelicopter;
    internal readonly float Length;
    internal float TimeSinceLastUnitSearch;
    public readonly FastList<CollisionPredictionData> PossibleCollisions;
    public Vector2 FlatPosition, FlatSpeedDirection;
    public float   FlatSpeedValue;
    public bool    NeedDeceleration, EmergencyTurn, OutrunTurn;
}

public struct CollisionPredictionData {
    public Entity Target; public bool IsStatic, IsFrontal, IsLineCollision, IsMinimalSpeed;
    public float CollisionTimeEstimate;
}
```

with the tuning in `NavigationConstants`:

```csharp
public const float AVOIDANCE_EVADE_CHECK_ANGLE           = -7.5;
public const float AVOIDANCE_EVADE_MAX_ANGLE             = -7.5;
public const float AVOIDANCE_OUTRUN_MAX_ANGLE            = -12.5;
public const float AVOIDANCE_OUTRUN_MIN_SPEED_DIFF_RATIO = 1.3;
public const float AVOIDANCE_OUTRUN_MIN_SPEED_DIFF       = 2.777778;   // exactly 10 km/h
```

**[inferred] Three things worth taking.** Neighbour search is **amortised** — a
unit re-queries at most 5×/s and steers off a cached list, which is the same
"amortise the expensive query, answer the cheap one every frame" pattern as
target search (2 Hz) and terrain sampling. Collision is **time-based, not
distance-based**: `CollisionTimeEstimate` against a 5-second horizon, so a fast
vehicle starts avoiding earlier without any per-speed tuning. And ties are broken
by a **static priority** (`UnitHasPriority`) rather than by reciprocal velocity
obstacles — **one unit yields and the other does not**, which sidesteps RVO's
oscillation problem by fiat rather than by damping it.

`OutrunTurn` with a 1.3× speed ratio and a **10 km/h absolute floor** is
overtaking: you only pull out to pass if you are meaningfully faster *both*
proportionally and absolutely. **[inferred]** The absolute floor is what stops two
slow trucks endlessly swapping places at 4 versus 5 km/h — the same class of
hysteresis as [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §5.2's
`SHORTWAY_LENGTH_MIN_DIFF_RATIO`.

Stationary vehicles become NavMesh obstacles rather than steering problems:

```csharp
[WithEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag), typeof(UnitInfantryFlag) })]
[With(new[] { typeof(UnitComponent), typeof(ObstacleComponent) })]
public class StaticObstaclesSystem : AEntitySetSystem<float>
```

**[inferred]** A parked column carves the navmesh so others path around it, which
is a much better answer than making everyone steer around everyone.

---

## 6. The visual layer: one container, both propulsion kinds

This is where the tracked/wheeled distinction finally appears — and it appears as
*presence of a transform*, not as a type.

**[DUMP]**

```csharp
public class BoneContainer {
    [SerializeField] public bool  UseContainer;      // 0x10
    [SerializeField] private BoneSettings _settings; // 0x18
    [SerializeField] public float CurrentSpeed;      // 0x20
    [SerializeField][Range(-1, 1)] public float Turn;  // 0x24
    [SerializeField][Range(0, 1)]  public float Shake; // 0x28
    [SerializeField] private Transform  _root, _body;
    [SerializeField] private Wheels     _wheels;     // 0x40
    [SerializeField] private Suspension _suspension; // 0x48
    [SerializeField] private Tracks     _traks;      // 0x50   (their spelling)
    [SerializeField] private AnimationCurve _acceleration;  // 0x58
    private BoneContainer.RecoilDataDictionary _recoils;
    private float _lastSpeed;   private bool _isBraking;
    public void Bake(Transform mainObject);
}

public class BoneSettings {
    [SerializeField] public float WheelSpeed;          // 0x10
    [SerializeField] public float SuspensionSpeed;     // 0x14
    [SerializeField] public float TracksSpeed;         // 0x18
    [SerializeField] public float TurnSpeedMod;        // 0x1C
    [SerializeField] public float BodyShake;           // 0x20
    [SerializeField] public float BodyShakeAggressive; // 0x24
    [SerializeField] public float BodyMass;            // 0x28
}
```

**[inferred] `Wheels`, `Suspension` and `Tracks` are all three present on every
vehicle**, and a chassis simply leaves the irrelevant ones unassigned. Seven
numbers in `BoneSettings` tune the whole thing. `_isBraking` plus `_lastSpeed`
plus an `_acceleration` `AnimationCurve` means the body pitches on acceleration
and squats on braking from a designer-drawn curve rather than a spring solve.

### 6.1 Wheels are transforms; tracks are a texture offset

**[DUMP]**

```csharp
public class Wheels {
    [SerializeField] private Transform[] _left;   // 0x10
    [SerializeField] private Transform[] _right;  // 0x18
    [SerializeField] public  bool Turn;           // 0x21
    public void Move(float delta, float speed, float turn, float wheelSpeed);
}

public class Tracks {
    [SerializeField] public Transform Left;    private Material _left;    private Vector2 _trackLeft;
    [SerializeField] public Transform Right;   private Material _right;   private Vector2 _trackRight;
                     public Transform Extra1;  private Material _extra1;  private Vector2 _trackExtra1;
                     public Transform Extra2;  private Material _extra2;  private Vector2 _trackExtra2;
    private bool _init, _ready;
    public bool HasTracks { get; }
    public void Move(float delta, float speed, float turn, float trackSpeed);
}
```

**Read the two `Move` signatures: they are identical.** `(delta, speed, turn,
xSpeed)`. Both propulsion kinds consume the same two inputs — how fast the hull is
going and how hard it is turning — and differ only in what they do with them.

**[inferred] Wheels rotate real bones**, held as separate left and right arrays so
`turn` can spin them at different rates, with a `bool Turn` per wheel set saying
whether that axle also *steers*. **Tracks scroll a material.** `Material _left`
paired with `Vector2 _trackLeft` is a texture-offset accumulator advanced by
`speed`, and the left/right split is what makes a tank's inner track slow and its
outer track speed up in a turn. There is no track geometry animation and no
per-link simulation at all.

`Extra1` and `Extra2` with their own materials and offsets — **[inferred]** a third
and fourth track surface, for vehicles with more than one run per side or with a
separate visible return run.

**`Tracks.HasTracks` is the only "is this tracked" test in the game**, and it is a
null check on a `Transform` assigned in the prefab. A designer never declares a
chassis type; they drag a mesh into a slot.

### 6.2 The suspension never touches the ground

**[DUMP]**

```csharp
public class Suspension {
    [SerializeField] public SuspensionElement[] Elements;  // 0x10
    [SerializeField] public int TurnAxis;                  // 0x18
    public void Move(float delta, float speed);
    public void Turn(float turn);
}

[Serializable] public class SuspensionElement {
    [SerializeField] public BoneContainer.SuspensionSide Side;  // 0x10
    [SerializeField] public Vector3 DefaultPosition;            // 0x14
    [SerializeField] public byte    AxisNumber;                 // 0x20
    [SerializeField] public Transform Bone;                     // 0x28
    [SerializeField] private float _maxStroke;                  // 0x30
                     private Vector3 _lastRandomPosition;       // 0x34
                     private float   _updateTime;               // 0x40
    public void Move(float delta, float speed);
}
```

**[inferred] `_lastRandomPosition` + `_updateTime` + `_maxStroke` is a noise
sampler, not a ground sampler.** Each road wheel walks toward a fresh random
offset within its stroke limit, re-rolled on a timer, at a rate scaled by speed.
It never raycasts the terrain beneath itself.

**This is the same conclusion
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) §2.7
reaches about Eugen's engine** — *"it is not sampling the ground under each wheel;
it is sampling a 2D noise field"* — reached here independently, on a different
engine, by a different studio. That note's §8 already argues this genre converges
on the same structures; this is another instance, and a fairly pointed one, because
sampling the terrain per wheel is the *obvious* implementation and both teams
rejected it. At RTS camera distance a plausible jiggle is indistinguishable from a
correct one, and it costs no raycasts.

`Suspension.Turn(float turn)` with a `TurnAxis` int is the steering deflection —
**[inferred]** applied to the suspension bones rather than to `Wheels`, so a
wheel's *spin* and its *steer* come from different systems, which is why `Wheels`
needs only a `bool Turn`.

### 6.3 Dust is gated by terrain type

**[DUMP]**

```csharp
private class UnitBaseTerrainVFX.VfxTerrainData {
    public Transform[]  Points;            // 0x10
    public VfxTerrain[] OverrideVfx;       // 0x18
    [Tooltip("If empty does not affect anything")]
    public TerrainType[] PlayOnlyOnTerrains;  // 0x20
}
```

**[inferred]** The same `TerrainType` enum that selects the speed ceiling (§2)
gates the movement VFX, so tracks throw dust on dirt and nothing on tarmac from
one authored array — and the empty-means-always convention keeps the common case
free of configuration.

---

## 7. What transfers

1. **Express a vehicle class as data ratios, not as a type.** Wheeled versus
   tracked is `MaxSpeedRoad / MaxCrossCountrySpeed` plus `TurnRate`. No branch, no
   enum, no second code path — and the behaviour that emerges is richer than a
   flag, because a half-tracked or a fast tank is just a different ratio rather
   than a new category somebody has to add. **Before adding a type discriminator,
   check whether the thing you want is a number.**

2. **Let presence, not declaration, decide capability.** `Tracks.HasTracks` is a
   null check on an artist-assigned transform. The content authors never fill in a
   chassis field and therefore can never fill it in wrongly.

3. **One table per *concern*, not per domain.** `Mobility` covers land, sea and air
   because "how does this thing move" is one question; the aircraft profile hangs
   off it as an optional reference. The fields a tank ignores are exactly the
   fields a jet ignores.

4. **Amortise the expensive query and answer the cheap one every frame.** Terrain
   type on a `CheckDelay`, neighbours at 5 Hz, target search at 2 Hz — while
   speed, heading and position integrate continuously. The pattern is everywhere
   in this codebase and is worth adopting wholesale.

5. **Predict collisions in time, not distance.** A 5-second horizon self-scales
   across vehicle speeds where a metre radius would need per-unit tuning. Same
   argument as authoring offsets in seconds
   ([`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §12.3).

6. **Break avoidance ties by rank, not by negotiation.** `UnitHasPriority` — one
   yields, one does not. Reciprocal schemes oscillate; a total order cannot.

7. **Give "should I overtake" an absolute floor as well as a ratio.**
   `AVOIDANCE_OUTRUN_MIN_SPEED_DIFF_RATIO = 1.3` **and** a 10 km/h minimum. A pure
   ratio lets two slow units swap places forever.

8. **Separate "which corridor" from "which line through it".** The path picks the
   road; `GetRoadWidePoint` picks the lane. The aircraft do the same thing with an
   arc-length carrot. **The planner should not be asked for precision it cannot
   maintain.**

9. **Fake the suspension.** Noise bounded by stroke, scaled by speed, re-rolled on
   a timer. Two shipped engines in this genre independently rejected per-wheel
   terrain sampling, which is strong evidence it is not worth the raycasts at
   strategic camera distances.

10. **Scroll a texture instead of animating a track.** Per-side material offset,
    driven by hull speed and turn rate, gives a correct-looking differential for
    two `Vector2`s and no bones.

---

## 8. What is not established

- **No method bodies.** How `Wheels.Move`, `Tracks.Move` and
  `SuspensionElement.Move` use their arguments is inference from field names and
  signatures. The UV-scroll reading of `Tracks` rests on `Material` + `Vector2`
  being present and no track bone array being present, which is strong but not
  read.
- **No authored values.** Every `Mobility` row, every `BoneSettings` number and
  the LOD band distances live in `BrokenArrowDB.bytes` and prefab assets. **The
  central claim of this note — that the wheeled/tracked difference is the
  road/cross-country ratio — is a claim about where the difference *lives*, not a
  measurement of how big it is.** Reading the database would settle it in minutes
  and is the obvious next step.
- **`Agility` has no confirmed consumer.** §4's reading is a guess.
- **`MaxSpeedForest`'s derivation is inferred** from a constructor argument the
  database does not supply.
- **Nothing was run or profiled.**

---

## Sources

**[DUMP]** Il2CppDumper v6.7.46 over
`C:\Program Files (x86)\Steam\steamapps\common\broken_arrow\GameAssembly.dll` and
`BrokenArrow_Data\il2cpp_data\Metadata\global-metadata.dat` (IL2CPP metadata v31,
dated 2025-08-06).

The visual layer is read against
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md), which
covers R.U.S.E., Wargame: Red Dragon, WARNO and Broken Arrow across twelve years —
§6.2 above corroborates its §2.7 noise-suspension finding from a second engine.
