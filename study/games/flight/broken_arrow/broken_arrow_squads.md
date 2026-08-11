# Broken Arrow — infantry squads: one agent, N bodies, and a weapon that is neither

How does a modern-warfare RTS run infantry at scale — move a squad, aim it, shoot
it, shoot *at* it, and draw it — without simulating nine men? Read from the retail
build alongside [`broken_arrow.md`](broken_arrow.md), whose §4.2 lists
`InfantryNavigationSystem`, `InfantrySoldierMoveSystem` and `InfantrySprintSystem`
and says nothing about what they do.

The answer is a **three-tier entity model**, and almost every interesting property
falls out of where each decision was placed:

> **The squad is the unit.** One entity: one path, one `HealthComponent`, one
> `BoxCollider`, one set of eyes, one avoidance agent, one entry in the spatial
> tree, one FMOD instance. Everything the battle systems touch, they touch here —
> and they touch it with **exactly the same code that drives a tank**.
>
> **The soldier is a body.** A real entity, but not a unit: no health, no
> collider, no path, no vision, no target. It owns a transform, an `Animator`, a
> formation slot, a weapon prop, and `DeathPriority` — the rank saying when it
> falls over.
>
> **The weapon is the combatant.** This is the part that is genuinely unusual. A
> weapon is a first-class entity with its own target, aim timer, burst and reload
> state. A squad owns one weapon entity **per weapon type**, not per man — a
> 7-man squad with 5 rifles, an LMG and an RPG has **three** weapon entities and
> can engage **three different enemies**. Soldiers queue behind each one to play
> the animation.
>
> The feedback that makes the abstraction convincing is a single quantity:
> `UsingAliveSoldierCount` divides the shared weapon's cycle time, so **a squad
> losing men loses firepower continuously** without anyone simulating a man.

> **Source and its limits.** Everything tagged **[DUMP]** was read from an
> Il2CppDumper v6.7.46 dump of the retail `GameAssembly.dll` + `global-metadata.dat`
> (metadata v31, dated 2025-08-06). See
> [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) for the full method note.
>
> **IL2CPP compiles method bodies to native code and the dump does not contain
> them.** Every method here is `{ }`. Type names, field names and offsets, method
> signatures, `const` values, DefaultEcs `[With]`/`[Without]`/`[WithEither]`
> filters and Unity `[Tooltip]`/`[Header]`/`[Range]` attributes are **literal**;
> anything about control flow is **[inferred]** and tagged.
>
> **The ECS filters are the best evidence in this note.** A system's `[With]`
> attribute states exactly which entity kind it iterates, and that is a fact about
> the architecture rather than a guess about it. Where this note says "per squad"
> or "per weapon", a filter usually says so outright.
>
> This note was assembled from four parallel sweeps of the dump; a sample of the
> load-bearing declarations was re-read directly against the file. One
> agent-reported claim did not survive that check and is corrected in §3.

Related: [`broken_arrow.md`](broken_arrow.md),
[`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) (the same locomotion
question for fixed wing, and §9a there is the ground-movement component model
this note builds on),
[`broken_arrow_damage.md`](broken_arrow_damage.md) (armour and suppression
*formulas*, deliberately not re-derived here),
[`battle_scale.md`](../../../topics/scale/battle_scale.md),
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) (§10
reads the target-deconfliction designs against each other).

---

## 1. The three tiers

### 1.1 The squad

**[DUMP]**

```csharp
// Namespace: BrokenArrow.Client.Ecs.Infantry.Components
public struct InfantryUnitComponent {
    public readonly Entity[]      Soldiers;         // 0x0
    public readonly BoxCollider   SquadCollider;    // 0x8
    public float                  CurrentSquadSize; // 0x10
    public float                  LatestHealth;     // 0x14
    public float                  HealthPerSoldier; // 0x18
    public readonly ISquadFormation SquadFormation; // 0x20
    private bool <WeaponEntityDisable>k__BackingField; // 0x28
    private bool <IsAnimatorEnabled>k__BackingField;   // 0x29
    public uint                   AliveSoldierCount;   // 0x2C
    public readonly WeaponVfxDictionary WeaponVfxDictionary;    // 0x30
    public readonly FastList<WeaponAndSoldiers> SquadWeapons;   // 0x38

    public void ChangeWeaponCount(in SoldierComponent, int diff);
    public void DisableSoldiersAnimator();
    public void DisableInfantryWeapons();
}
```

### 1.2 The soldier

**[DUMP]**

```csharp
public struct SoldierComponent {
    public bool       IsAlive;              // 0x0
    public readonly int DeathPriority;      // 0x4
    public bool       IsMoving;             // 0x8
    public float      StartMovingDelay;     // 0xC
    public Vector3    DestinationPosition;  // 0x10
    public Quaternion DestinationRotation;  // 0x1C
    public SquadFormation Formation;        // 0x2C
    public UsedWeapon UsedWeapon;           // 0x48
    public readonly SoldierWeapon PrimaryWeapon;  // 0x50
    public readonly SoldierWeapon SpecialWeapon;  // 0x58
    public readonly bool IsSpecialUnderbarrel;    // 0x60
    public bool       IsAiming;             // 0x61
}
```

**Note what is absent: no health, no armour, no collider, no target, no path, no
ammunition, no reload timer, no line of sight.** `IsAlive` is written *by* the
casualty allocator (§7) rather than being something damage is applied to. The
only per-soldier combat state in the entire game is `IsAiming` and which of two
guns is in the man's hands.

Soldiers are genuinely separate entities — they carry their own
`NetworkSoldierLocalComponent` / `NetworkSoldierRemoteComponent` (both empty tag
structs, so they replicate as *positions*, not as damageable actors) — but they
are **not units**. `VerticalOrientationSystem` proves it by listing
`SoldierComponent` as a *sibling alternative* to the unit flags:

```csharp
[WithEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag),
                    typeof(UnitInfantryFlag), typeof(SoldierComponent) })]
```

**[inferred]** A soldier entity exists for exactly five reasons: to own a
transform, an `Animator`, a weapon prop, a formation slot, and a network
ownership tag. It exists so it can be *drawn*, not so it can *fight*.

`UnitType` confirms the boundary — there is no soldier member:

```csharp
[Flags] public enum UnitType {
    None=0, Infantry=2, Vehicle=4, Helicopter=8, Aircraft=16, Ship=32,
    Projectile=128, SEADMissile=256, CruiseMissile=512, BallisticMissile=1024
}
```

and the spatial index is keyed by it:

```csharp
public class UnitsPositionsTree {
    private const float MIN_NODE_SIZE = 50;
    private Dictionary<UnitType, KdSearchTree<Entity>> _searchTreesDict;
    public ReadOnlySpan<ValueTuple<Entity,float>> GetNearbyUnits(UnitType type,
        GalaxyVector3 fromPosition, float maxDistance, bool sphereSearch = True);
}
```

**[inferred] Soldiers therefore cannot be inserted into the spatial tree at all.**
Every target search, every area-effect sweep, every neighbour query in the game
*necessarily* returns squads. The squad-level model is not a policy applied
consistently by discipline — it is enforced by the type of the index.

### 1.3 The weapon

**[DUMP]** `WeaponComponent` is a `class`, not a struct — ~300 bytes of state,
deliberately on the heap and out of the DOD path:

```csharp
[DebuggerDisplay("Weapon: {WeaponInfo.HUDName}")]
public class WeaponComponent {
    public readonly Weapons WeaponInfo;
    private readonly Entity _weaponEntity;
    private GFloat _aimTime, _timeBetweenBursts, _magazineReloadTime;
    public GInt   AmmoInMagazine;
    public readonly Dictionary<int,int> AmmoReservedInMagazine;
    public float  TargetHorizontalAngle, TargetVerticalAngle, CurrentVerticalAngle;
    public readonly float IdleAngle, UpperVerticalAngle, LowerVerticalAngle;
    public readonly bool  IsPrecisionWeapon, IsCoaxial, IsAntiAir, ScoreAntiOverkill;
    public bool   CanTargetPriorityTarget;
    public readonly Entity TurretParrent;          // their spelling
    public BallisticDataStruct CachedShotData;
    public TrajectoryStruct    CachedTrajectoryStruct;

    public void GenerateRandomAimTime();
    public void GenerateRandomShotsPerBurst();
    public void GenerateRandomTimeBetweenBursts();
    public void GenerateRandomMagazineReloadTime();
    public Entity GetUnitEntity();
}
```

Three levels, joined by `ParentComponent { readonly Entity Root; readonly Entity Value; }`
— `Root` is the unit, `Value` the immediate parent. **weapon → turret → unit.**

And for infantry, the same shape with the soldiers hung off the side:

```csharp
public class WeaponAndSoldiers {
    public readonly Entity          WeaponEntity;
    public readonly WeaponComponent WeaponComponent;
    public readonly List<Entity>    Soldiers;
    [TupleElementNames(new[]{ "Current", "Max" })]
    public ValueTuple<int,int>      Count;
}

public struct InfantryWeaponComponent {          // sits ON the weapon entity
    public readonly int      WeaponSquadIndex;
    public readonly Entity[] UsedBySoldiers;     // ← points back at the men
    public int               AmmoPerSoldier;
    public readonly Queue<int> AimedSoldiersQueue;
    public int               NextShootSoldierIndex;
    public int               LastAimedSoldierIndex;
    public int               UsingAliveSoldierCount;
}
```

The soldier's own weapon reference is purely presentational — a `GameObject` and
an **index**, never an entity:

```csharp
public class SoldierWeapon {
    public GameObject WeaponGameObject;
    public Weapons    Weapon;              // database row
    public int        WeaponEntityIndex;   // index into the squad's weapon list
    public Transform  FirePosition, WeaponHandPosition, WeaponBackPosition;
    public void SetWeaponToBack();
    public void SetWeaponToHand();
}
```

**[inferred]** The database agrees: `SquadWeapons { WeaponId, Order, UnitId }` has
**no count column**. It is a set of weapon *types*; how many of each exists is
emergent from how many `SquadMembers` rows name that weapon.

The developers' own word for the arrangement is in a tooltip, and it is the best
single line in the dump for this note:

> `[Tooltip("Rotation speed for soldier the fake weapon, usualy need only for 'hit and run' logic")]`
> — `InfantryConfig.HorizontalTurretRotationSpeed`

**"The fake weapon."** Mechanically, an infantry squad is a vehicle with turrets.

---

## 2. Movement: one navigator, N followers

### 2.1 Only the squad has a path

**[DUMP]** The filter is the proof:

```csharp
[With(new[] { typeof(UnitInfantryFlag) })]
[With(new[] { typeof(RotationSpeedComponent), typeof(MaxRotationSpeedComponent) })]
[With(new[] { typeof(TransformComponent), typeof(SpeedComponent), typeof(AccelerationComponent) })]
[With(new[] { typeof(UnitComponent), typeof(PathComponent) })]
[Without(new[] { typeof(AirdropComponent), typeof(LoadedComponent) })]
public class InfantryNavigationSystem : EcsSetSystem<float> {
    private const float ANGLE_FRAME_OUTRUNNING    = 2;
    private const float ALLOWED_ROTATION_ANGLE_DELTA = 10;
    private const float TURN_ANGLE_TOLERANCE      = 45;
}
```

**The squad carries `SpeedComponent`, `AccelerationComponent`,
`RotationSpeedComponent`, `MaxRotationSpeedComponent` and `PathComponent` — the
identical generic ground-movement set a tank carries** (see
[`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §9a). Soldiers have none of
them.

**[inferred] This is the single biggest scaling decision in the infantry code.** A
nine-man squad costs **one** path query, **one** avoidance agent and **one**
steered transform, not nine. Everything else about infantry follows from having
made this choice first.

Soldiers appear in the entity set of only two systems in the entire game —
`VerticalOrientationSystem` (conforming to terrain) and `AirdropSystem`
(parachuting). Every other system that touches a soldier does so by iterating
`InfantryUnitComponent.Soldiers` *inside a squad update*:

```csharp
[Without(new[] { typeof(DeadComponent), typeof(DeathComponent),
                 typeof(RemoveUnitComponent), typeof(LoadedComponent) })]
[With(new[] { typeof(InfantryUnitComponent) })]          // ← iterates SQUADS
public class InfantrySoldierMoveSystem : EcsSetSystem<float> {
    private void CalculateSoldierRotationToTarget(float dt, in Entity weaponEntity,
        ref SoldierComponent, ref TransformComponent);
    private void CalculateSoldierPositionAndRotation(float dt, ref SoldierComponent,
        ref TransformComponent, Quaternion resultAngle, float speed, bool isLoading);
    private bool IsDestinationReached(in Vector3 destination, in Vector3 position);
}
```

**[inferred]** The squad's update writes each `SoldierComponent.DestinationPosition`
and `.DestinationRotation` as `squadTransform × formationSlot`, and
`CalculateSoldierPositionAndRotation` walks the body toward it at a constant
`float speed` — note it takes a scalar and there is no soldier-level
`AccelerationComponent`. **Soldiers do not path, do not avoid, and never negotiate
with one another.** They cannot separate from the squad because they are offsets
from it.

Note also that `CalculateSoldierRotationToTarget` takes a **`weaponEntity`**, not
a target position. A soldier faces where his shared weapon is aimed.

### 2.2 Formation: one hexagon, jittered once

**[DUMP]** There is exactly one formation shape in the retail build:

```csharp
public interface ISquadFormation {
    List<SquadFormation> GetFormations();
    SquadFormation       GenerateFormation();
    void                 UpdateDistanceBetweenSoldiers(float newOffset);
}

public abstract class SquadFormation : ISquadFormation {
    protected float _stepLength;
    protected readonly int _pointCount;
    protected void SetPosition(Vector3 position, int sequenceNumber);
    public Vector3 GetRandomOffset();
}

public class HexagonalSquadFormation : SquadFormation {
    private Vector3 _reeversPosition;                       // their spelling
    private void <GenerateFormation>g__CreateFormation|2_0(int steps, ref …);
}

public struct SquadFormation {          // the per-soldier slot
    public int     Index;               // 0x0
    public Vector3 PositionOffset;      // 0x4
    public Vector3 RandomPositionOffset;// 0x10
}
```

with spacing from config, tooltip verbatim:

```csharp
[Tooltip("Distance between soldiers in a formation cell")]
public float DistanceBetweenSoldiers;        // 0x7C
public float DistanceBetweenSoldiersHangar;  // 0x80
```

**[inferred]** `CreateFormation(int steps, …)` walking a `sequenceNumber` up to
`countOfPoint` is a hex-ring spiral — ring 0 the centre, each ring outward at
`_stepLength`. The slot is `squadPos + rot × (PositionOffset + RandomPositionOffset)`,
and **the random term is drawn once at spawn, not per frame**, so the hexagon
reads as an organic cluster while staying rock-stable. `UpdateDistanceBetweenSoldiers`
being a live mutator rather than a constructor argument means the squad can
breathe in and out at runtime.

### 2.3 Turn versus move: simultaneous, gated by two angles

This is the direct answer to "do they turn a bit then move, like a human?".
**[DUMP]** The same two constant names appear in three navigation systems with
different values:

| System | `ALLOWED_ROTATION_ANGLE_DELTA` | `TURN_ANGLE_TOLERANCE` | extra |
|---|---|---|---|
| `InfantryNavigationSystem` | **10°** | 45° | `ANGLE_FRAME_OUTRUNNING = 2` |
| `NavigationSystem` (ground/water vehicles) | **30°** | 45° | road logic, node skipping |
| `HelicopterNavigationSystem` | — | — | `MIN_LOOKROTATION_ANGLE = 30` |

**[inferred] There is no turn-then-move state machine.** No state enum, no timer,
no sequencing. Rotation and translation both run every frame; the *gate* is the
heading error:

- Beyond `TURN_ANGLE_TOLERANCE` (45°, identical for both) the corner is too sharp
  and translation is suppressed or heavily damped — you turn first.
- Below `ALLOWED_ROTATION_ANGLE_DELTA` you are "facing the waypoint" and no
  rotation is applied — a deadband.
- Between them, you turn and translate at once, arcing into the leg.

**So the player-visible difference between a squad that pivots crisply and a tank
that swings wide is two numbers — 10 versus 30 — and a database `TurnRate`, not
two code paths.** That is a genuinely economical way to get locomotion character.

`ANGLE_FRAME_OUTRUNNING = 2` is infantry-only and has no vehicle analogue.
**[inferred]** An anti-overshoot clamp: if `maxRotationSpeed × dt` would carry the
heading more than 2° past target, clamp to target. It exists only for infantry
precisely *because* infantry turn rates are high enough that a long frame would
otherwise overshoot and oscillate.

**And infantry are excluded from turn-on-the-spot entirely:**

```csharp
[Without(new[] { typeof(MovingComponent) })]
[WithEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag) })]
public class RotateUnitSystem : AEntitySetSystem<float> {
    private const float MAX_ALLOWED_ANGLE_DIFF = 1;
}
```

**[inferred]** A stationary squad has no body facing to command, because its
apparent facing is emergent from where the individual soldiers are aiming. So
`RotateCommand` has no infantry executor at all — a correction worth flagging,
since the obvious reading of this system's name is that it serves everything that
rotates.

### 2.4 Arrival, repathing and avoidance are shared with vehicles

**[DUMP]**

```csharp
public static class NavigationConstants {
    public const float FOREST_HEIGHT                     = 25;
    public const float REPATH_RATE                       = 10;
    public const float END_REACHED_DISTANCE              = 7.5;
    public const float COST_OVERRIDES_ROAD_BIAS          = 1.2;
    public const float INFANTRY_NAVIGATION_SIZE          = 3;
    public const float AVOIDANCE_EVADE_CHECK_ANGLE       = -7.5;
    public const float AVOIDANCE_OUTRUN_MAX_ANGLE        = -12.5;
    public const float AVOIDANCE_OUTRUN_MIN_SPEED_DIFF_RATIO = 1.3;
    public const float AVOIDANCE_OUTRUN_MIN_SPEED_DIFF   = 2.777778;   // 10 km/h in m/s
    public const float HELICOPTERS_HOVER_ALTITUDE        = 1;
    public const float HELICOPTER_PREFFERED_ALTITUDE_HIGH = 40;
    public const float HELICOPTER_PREFFERED_ALTITUDE_LOW  = 8;
}
```

`END_REACHED_DISTANCE = 7.5` matches `BaseMoveCommand.END_REACHED_SQ_DISTANCE = 56.25`
(= 7.5²) and `HelicopterNavigationSystem.MINIMAL_REACH_POINT_DISTANCE = 7.5`. **A
flat 7.5 m arrival radius for the entire game.**

`INFANTRY_NAVIGATION_SIZE = 3` — **[inferred]** the squad navigates as a **3 m-wide
blob**, not as N half-metre men.

The path itself carries a separate carrot:

```csharp
public struct PathComponent {
    public int      CurrentNodeIndex;      // 0x0
    public WorkNode CurrentNode;           // 0x8
    public Vector3  SteeringPoint;         // 0x30   ← look-ahead, decoupled
    public readonly NavPath Path;          // 0x40
    // five constructors: subgraph A*, straight line, NavMesh corridor,
    // NavMesh corridor spliced onto an existing path at the unit's position,
    // raw node list
}
```

**[inferred]** Steering aims at `SteeringPoint`; acceptance advances
`CurrentNodeIndex`. The two are decoupled *so a unit can cut corners* — the same
separation the aircraft achieve with `TargetAheadFactor`. The fourth constructor
is the repath-while-moving case, splicing a fresh corridor onto the existing path
at the unit's current position so a repath does not snap the unit back to a stale
node.

Avoidance is squad-level, predictive, and infantry opt in — `AvoidanceDataComponent`
carries an explicit `readonly bool IsInfantry`:

```csharp
public class DynamicAvoidanceSystem : EcsSetSystem<float> {
    private const float UNIT_SEARCHBUFFER_MAX_UPDATETIME = 0.2;
    private const float COLLISION_PREDICTION_TIME_RADIUS = 5;
    private const float FRONTAL_COLLISION_MAX_ANGLE      = 15;
    private const float LINE_COLLISION_MAX_ANGLE         = 60;
    private static bool UnitHasPriority(in Entity unitEntity, in Entity targetEntity);
}
```

**[inferred]** Three things worth extracting. The neighbour search is **amortised**
— a unit re-queries at most 5×/s and steers off a cached list. Collision is
**time-based, not distance-based** (`COLLISION_PREDICTION_TIME_RADIUS = 5` seconds).
And ties break on a **static priority** rather than reciprocal velocity obstacles:
one unit yields, the other does not. That sidesteps RVO's oscillation problem by
fiat, which is the same instinct as the incumbency guards in
[`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §5.2.

Unity's NavMesh is used only as a **pooled path-query service** —
`NavAgentsPoolComponent` holds a `FastList<NavMeshAgent>`, agents are borrowed to
compute a corridor and returned, and paths beyond 800 m go to the bespoke
`NavigationGraph` subgraph A*. **Steering and locomotion are entirely hand-rolled.**

### 2.5 What stops it looking robotic

Rigid slot-following would read as a marching lattice. Four devices prevent it,
and none of them is a cohesion force:

**[DUMP]**

```csharp
[Tooltip("Max value delay before the soldier starts moving. Each time a random value from 0 to this value will be selected")]
[Header("Move")]
public float StartMovingMaxDelay;   // 0x24

[Tooltip("If distance to squad more that, soldier change run to sprint (only for try swap to idle)")]
public float DistanceForSprint;     // 0x44
[Tooltip("Distance the soldier must change run to idle or aim state (0.1 good only for test map [sqrMagnitude]")]
public float DistanceForSoldierStop;// 0x48

[Header("Other")]
public float SoldierScaleMax;       // 0x74
public float SoldierScaleMin;       // 0x78
```

1. **Staggered starts** — each man rolls `0..StartMovingMaxDelay` per order, so
   the squad ripples into motion instead of snapping.
2. **Sprint-to-catch-up** — a soldier whose slot has drifted past
   `DistanceForSprint` sprints to close it. The developer's own parenthetical
   *"only for try swap to idle"* is the caveat that **this is a display-state
   decision, not a speed change in the simulation.**
3. **Baked position jitter** — `RandomPositionOffset`, §2.2.
4. **Size variation** — per-man uniform scale, so they are not clones.

Squad-wide sprint is a separate *ability* with its own duration and cooldown
(`SprintAbilityComponent`), and its speed is global:

```csharp
[Tooltip("Speed which all infantry squads will use when sprinting.")]
[Range(8, 20)] public float SprintSpeed;   // GameConfig
```

**[inferred]** There is **no leader, no follower, no cohesion term and no boid**.
Cohesion is structural — you cannot drift from a slot you are defined by. The
"cohesion behaviour" is entirely a *display* layer painted over rigid offsets, and
that is the trick: all the visual benefit of a loose squad, none of the simulation.

### 2.6 The formation dissolves for boarding

**[DUMP]** Getting into a transport is the one case where slot-following is
abandoned:

```csharp
public class SoldierLoadingData {
    public readonly Vector3[] DirectHelpPoints;   // 0x10
    public readonly Vector3[] OtherHelpPoints;    // 0x18
    [TupleElementNames(new[]{ "Name", "Position" })]
    public ValueTuple<string,Vector3> Entrance;   // 0x20
    public bool IsGoingToEntrance, WasLoaded;
}
// EntityLoadAnimationSystem
private bool IsDestinationReached(in Entity soldierEntity, in Vector3 entrance);
private … GetTransportLoadingHelpPoints(in BoundingBox, in Vector3 position,
                                        in Quaternion rotation, float offset = 3);
```

**[inferred] "Help points" are waypoints around the transport's bounding-box
corners**, so a man walks *around* the vehicle to the door rather than clipping
through it — and this system has its own **per-soldier** arrival test, the only
one in the game. Boarding is the single case where a soldier is individually
navigated, and it needed a bespoke system to do it.

---

## 3. Ordering many units: `CommandFormation`

Not to be confused with §2.2. **[DUMP]** When several units receive one move
order, a separate static class lays them out:

```csharp
public static class CommandFormation {
    public const float DEFAULT_SPIRAL_RADIUS      = 18;
    public const float COVER_SNAP_MAX_DISTANCE    = 8;
    private const float MIN_ANCHOR_DISTANCE       = 50;
    private const float MAX_ANCHOR_DISTANCE       = 250;
    private const int   PLATOONS_PER_COMPANY      = 3;
    private const float INTER_COMPANY_DISTANCE    = 30;
    private const int   UNITS_PER_PLATOON         = 4;
    private const float PLATOON_UNITS_DISTANCE    = 20;
    private const float PLATOON_CENTERS_DISTANCE  = 100;

    public static void GenerateRawFormation(ReadOnlySpan<Entity> units, Span<Vector3> rawSlots, out int jetsCount);
    public static void AssignSlots(ReadOnlySpan<Entity> units, ReadOnlySpan<Vector3> startPositions,
                                   ReadOnlySpan<Vector3> rawSlots, Span<Vector3> slots);
    private static bool IsFrontUnit(UnitCategoryType deckCategory);
    public static IEnumerable<ValueTuple<int,int>> Spiral(int width = 11, int height = 11);
    internal static void <AssignSlots>g__SetUnitsWeights|29_0(Span<int> unitWeights, …);
}
```

**[inferred] Two things are worth stealing.** Slot assignment is a **weighting
pass** over start positions versus raw slots rather than nearest-first — a greedy
cost assignment, so units do not cross each other's paths walking to their places.
And the layout is **doctrinal rather than geometric**: 4 units to a platoon, 3
platoons to a company, with `IsFrontUnit(UnitCategoryType)` sorting by deck role,
so ordering a mixed selection forward puts the tanks in front of the trucks
without the player arranging anything.

Slots then snap to cover, with a tooltip that shows the problem being solved:

```csharp
[Range(0, 8)] public float CoverSnapDistance;
[Tooltip("In situations where there is a single unit selected, snap distance will be multiplied by this")]
[Range(0, 1)] public float CoverSnapSingleUnitModifier;
```

**[inferred]** Snapping is helpful for a group and presumptuous for one unit — a
single selected unit should go where you clicked. One modifier, and the
interaction stops fighting the player.

---

## 4. Aiming and target selection

### 4.1 The target lives on the weapon entity

**[DUMP]**

```csharp
public struct TargetComponent {
    public Entity  Target;                    // 0x0   ← a bare entity, no sub-index
    public GFloat  LastDamageScore;           // 0x8
    public GFloat  LastHitChance;             // 0x10
    public bool    IsTargetOverkilledByOthers;// 0x18
    public TargetFireMode TargetingMode;      // 0x1C
    public bool    CanAim;                    // 0x23
}
```

Every consumer's filter says *weapon*:

```csharp
[WithEither(new[] { typeof(TargetComponent), typeof(GroundTargetComponent) })]
[Without(new[] { typeof(MagazineReloadingComponent), typeof(BurstCooldownComponent),
                 typeof(WeaponCycleComponent), typeof(NetworkWeaponRemoteComponent) })]
[With(new[] { typeof(WeaponComponent), typeof(AimedComponent) })]
public class ShootingSystem : EcsSetSystem<float> {
    protected override void Update(float deltaTime, ReadOnlySpan<Entity> weaponEntities);
    private static bool CheckInfantryConditions(Entity squadEntity, Entity weaponEntity);
}
```

That last signature is the whole design in one line: **`ShootingSystem` iterates
weapon entities and calls the squad-target gate as a single extra check on top of
the identical vehicle path.** The developers named the parameter `squadEntity`
themselves.

**[inferred] So neither the squad nor the soldier picks a target — each weapon
entity does.** A squad with three weapon types can be shooting three different
enemies. The player's order is a *unit*-level bias, not an assignment:
`PriorityTargetComponent { readonly Entity Value; }` on the unit, honoured
per-weapon via `WeaponComponent.CanTargetPriorityTarget`, and hold-fire is a
unit-level toggle passed down as `TrySetTarget(…, bool isUnitOnHoldFire, …)`.

### 4.2 The search, and how it is amortised

**[DUMP]**

```csharp
public static class TargetSearchHelper {
    public const int MAX_MS_DELAY    = 500;
    public const int MAX_AA_MS_DELAY = 200;
    private const UnitType GROUND_UNIT_TYPES = 38;      // Infantry|Vehicle|Ship
    private static ValueTuple<TargetInfo,TargetInfo> SelectTarget(in Entity weaponEntity, TargetLevel);
    private static void DelayNextSearch(in Entity weaponEntity, WeaponComponent weapon);
}
private struct TargetSearchHelper.<>c__DisplayClass25_0 {
    public TargetLevel currentTargetLevel;
    public float       scoreOnAimable;    public TargetInfo shootableTarget;
    public float       scoreOnUnaimable;  public TargetInfo outOfAnglesTarget;
}
```

**[inferred]** Target search runs at most **2 Hz for ground weapons and 5 Hz for
AA**, with a per-weapon `TargetSearchDelayComponent` staggering them so they do
not all re-search on the same frame. And `SelectTarget` keeps **two** running
bests — the best *aimable* candidate and the best *unaimable* one — so a vehicle
knows which way to rotate its hull to unmask a limited-traverse weapon
(`HullWeaponsRotationComponent.WeaponsUnamiableTarget`). This is CLAUDE.md's
"sort before the expensive test" rule with an extra channel for "and remember what
I nearly could have shot".

The vocabulary of failure is unusually granular and worth quoting for its own
sake — **[DUMP]** `TargetRemoveReason` has 33 members:

```
Unspecified, TargetDead, TargetInvalid, SecondaryWeaponDidntSwitchCantAim,
MarkedForRemoval, TargetTooFar, NoLos, TargetNotVisible, CantKinematicallyHit,
CantAim, NoGoodShell, TargetSwitch, WeaponDisabled, TargetOverkilled,
TargetIsInTransport, RadarWeaponWithoutRadar, ProjectileWithoutRadar, NetDead,
FailSafe, ShooterDead, RemoteLoss, RemoteStateChange, ShooterLoadedInTransport,
OwnerChange, MissileSafeRemove, MissileBeyondSeekerAngles,
MissileShooterDoesntHaveTarget, MissileIsNotSupported, SeekerLaserSpotDoestExist,
MissileTargetRadarDisabled, MissileTargetHasNoRadar, MissileTerminalGoingPitbul,
MissileReqestedDead, TargetDoesNotHaveUnitComponent = 100
```

paired with a `ReportedSystem` enum naming all 17 systems that can drop a target.
**[inferred]** Nobody writes 33 distinct reasons and a per-system attribution
channel for fun. This is a **diagnostic instrument for a class of bug that is
otherwise undebuggable** — "my AA did not shoot" has thirty-three possible causes
and the game can name which one. That is the same argument CLAUDE.md makes for
profiler zones, applied to a decision system rather than a frame budget.

### 4.3 A squad has one set of eyes

**[DUMP]** `FogOfWarComponent` sits on the **unit**:

```csharp
public struct FogOfWarComponent {
    public readonly float DistanceGround, DistanceLowAlt, DistanceHighAlt;
    public float AntiVisible, WeaponFlash, SpottedPenalty;
    public readonly HashSet<Entity>  VisibleTo;      // ← units
    public readonly HashSet<Entity>  Shootable;      // ← units
    public readonly Dictionary<Entity,FilterTask> PotentialSee;
    public readonly Vector3 UnitColliderCenter;
}
public static class FogOfWarHelper {
    public static bool TryGetEntityEyePoint(Entity entity, in FogOfWarComponent fowCom, out Vector3 eyePosition);
}
```

**One** eye point per entity, taken from `UnitColliderCenter`. The visibility work
item is a unit-to-unit pair (`FilterTask { Entity Caster; Entity Target; … }`)
processed off-thread by `FogOfWarFilter`.

**[inferred] So the answer to "can one soldier not see the target and pick another"
is no — the question cannot be posed.** There is no per-soldier vision, no
per-soldier LOS ray, and no per-soldier `Shootable` set. If the squad can see and
shoot something, every weapon entity in it can, subject only to its own range,
ammo target-mask and angle limits. The available failure modes are per *weapon*
(`NoLos`, `TargetNotVisible`, `CantAim`, `NoGoodShell`), never per man.

One nice consequence: `FogOfWarComponent.WeaponFlash` plus `Weapons.FlashPerShot`
means **shooting increases your own detectability**, and because it is a per-unit
accumulator, all of a squad's weapons pour flash into one visibility number.

### 4.4 The aiming queue — a visual scheduler over one simulated weapon

**[DUMP]**

```csharp
public static class InfantryHelper {
    public static bool TryGetNextAliveAndAvailableSoldier(int lastShootSoldier, Entity[] soldiers, out int soldierIndex);
    public static Quaternion GetRotationToTarget(in Entity weaponEntity, in Vector3 soldierPosition);
    public static void AddSoldierIndexToAimingQueue(ref InfantryWeaponComponent, int soldierIndexInWeapon);
    public static bool TryDequeueSoldierIndexFromAimingQueue(ref InfantryWeaponComponent, out Entity soldierEntity, out int soldierIndex);
}

public class SoldierWeaponSystem : EcsSetSystem<float> {
    private void CheckAndAimNextSoldier(in Entity infantryEntity, in Entity weaponEntity);
    private float CalculateWeaponTimeTillNextShot(in Entity weaponEntity, in InfantryWeaponComponent);
}
```

**[inferred] This is the mechanism that sells the whole abstraction.**
`CheckAndAimNextSoldier` promotes a man into `AimedSoldiersQueue` and puts him in
`SoldierState.Aim` *ahead of time*; when the shared weapon's shot timer fires,
`TryDequeueSoldierIndexFromAimingQueue` pops a **pre-aimed** soldier to spawn the
projectile. So the muzzle flash always comes from a man who is visibly already
aiming rather than one snapping around — and `InfantryConfig.AimingAnimationDuration`
is the lead time that makes it work.

There is **one aim direction per weapon entity**, not per soldier —
`WeaponComponent.TargetHorizontalAngle` / `TargetVerticalAngle` are single
scalars. Individual men rotate only for looks, via a `LookAtConstraint` and
`InfantryConfig.ShootAimRotationSpeed`, computed from
`GetRotationToTarget(weaponEntity, soldierPosition)` — **the target comes from the
weapon; the soldier's position is only the origin of the vector.**

Aim time is *rolled per engagement* (`GenerateRandomAimTime()` over
`[AimTimeMin, AimTimeMax]`), then scaled by stress and targeting crits.
`SetRemoteAimTime(float)` exists so the network replicates the **rolled** value
rather than re-rolling — otherwise clients would diverge.

### 4.4a Where the shot actually comes from: squad decision, soldier geometry

A squad's men are spread over several metres, and it has several weapon types with
different ranges. Does the spread matter?

**[DUMP]** Two helpers settle it:

```csharp
public static bool TryGetSoldierEffectSpawnPoint(in Entity infantryEntity,
                                                 in Entity weaponEntity,
                                                 out Transform shootingPoint);
public static Quaternion GetFireRotation(in Entity weaponEntity, Vector3 firePosition);
```

and each man's prop carries its own muzzle:

```csharp
public class SoldierWeapon {
    public Transform FirePosition;          // 0x30
    public Transform WeaponHandPosition;    // 0x38
    public Transform WeaponBackPosition;    // 0x40
}
```

**[inferred] The decision is squad-level; the geometry is per-soldier.** Whether
the weapon may engage is settled at the squad — one eye point, one `Shootable`
set, one visibility test (§4.3). But once the aiming queue pops a man (§4.4), the
projectile spawns from **that man's** `FirePosition`, and `GetFireRotation` takes
the actual muzzle position and derives the rotation to the shared target. So the
tracer comes from the right body at the right offset, and the trajectory is
computed from where that body is standing.

This is the same division of labour as everywhere else in this note: the
*expensive* question is asked once per squad, the *cheap* one is answered per man.
LOS is a raycast against terrain opacity and would cost nine times as much per
squad; a muzzle transform lookup is free.

**[inferred] Nor does the squad "fire everything" once LOS is granted.** Each
weapon entity holds its own target, its own aim timer and its own angle limits,
and range gating lives on the *ammunition*:

```csharp
public float MinimalRange { get; set; }   // 0xAC
public float GroundRange  { get; set; }   // 0xB0
public float LowAltRange  { get; set; }   // 0xB4
public float HighAltRange { get; set; }   // 0xB8
```

So a squad's RPG and its rifles have genuinely different envelopes and genuinely
independent target selection — the RPG can be engaging a distant APC while the
rifles work on infantry closer in, or be silent because nothing is inside its
band. `MinimalRange` in particular means a rocket team can be *too close* to fire
while the riflemen beside them keep shooting.

### 4.4b Weapons do not transfer, and `DeathPriority` is why they don't have to

**[DUMP]** The carrier list is fixed at construction:

```csharp
public struct InfantryWeaponComponent {
    public readonly Entity[] UsedBySoldiers;   // 0x8 — readonly, set in .ctor
    public int UsingAliveSoldierCount;         // 0x28
    public void .ctor(int weaponSquadIndex, in FastList<Entity> soldiersUsingThisWeapon);
}
```

**`UsedBySoldiers` is `readonly` and never grows.** The set of men who can carry
weapon X is decided at spawn from the `SquadMembers` rows; `UsingAliveSoldierCount`
only ever counts how many of that fixed set are still breathing, maintained by:

```csharp
public void ChangeWeaponCount(in SoldierComponent soldierComponent, int diff);
private static void RecalculateSoldierWeaponAmmo(in Entity infantryEntity,
    in SoldierComponent changedSoldier, bool isPrimaryWeapon);
```

**[DUMP] And there is no pickup mechanic at all.** A case-insensitive search of all
1,757,307 lines for `pickup`, `transfer weapon`, `redistribute`, `scavenge`,
`drop weapon` and `reassign weapon` returns **nothing** — the only hits are Unity's
`SRPLensFlareDistribution` enum.

**[inferred] So when the last carrier of a weapon type dies, the squad loses that
capability permanently.** The RPG does not pass to a rifleman. `WeaponAndSoldiers.Count`
reaches `(0, Max)`, `UsingAliveSoldierCount` reaches zero, and the weapon entity
goes quiet.

**And this is exactly what `DeathPriority` exists to manage.** §7's authored death
order is not flavour — it is the mechanism that makes the absence of a transfer
system survivable. Riflemen are killed first *by design*, so the specialist
weapons stay crewed for as long as the squad is combat-effective at all. **A
transfer mechanic and an authored death order solve the same problem, and the
death order is a single integer column in a spreadsheet.**

The design consequence is worth stating plainly: it means squad attrition degrades
capability along a **designer-chosen curve** rather than a random one. Lose three
of nine and you have lost three riflemen's worth of rate of fire and nothing else.
Lose eight and the last man standing is the one the designer decided should be
last — usually the one carrying the thing that makes the squad worth buying.

### 4.5 Rate of fire comes from the headcount

**[DUMP]**

```csharp
public static class WeaponHelper {
    public static float RateOfFire(Weapons weapon, int countOfWeapon);
    public static float RateOfFireSecond(Weapons weapon, float countOfWeapon,
        float timeBetweenBurstsModifier = 1, float magazineReloadTimeModifier = 1);
}
```

**[inferred]** `CalculateWeaponTimeTillNextShot(weaponEntity, infantryWeaponComp)`
taking the component that knows `UsingAliveSoldierCount` is only explicable as
dividing one weapon's cycle time by the number of living carriers. **Kill two of
five riflemen and the shared "rifle" fires three-fifths as often.** The squad's
damage output degrades continuously with casualties, with no per-soldier
simulation anywhere.

Ammunition is pooled on the **unit** (`AmmunitionBoxComponent`), and
`AmmunitionContainer._weaponsUsingThisAmmo` means weapons *compete* for rounds,
with `AmmoQuota` / `MaxAmmoToReserve` as the levers stopping one weapon eating
everything. `InfantryWeaponComponent.AmmoPerSoldier` is a display derivation, not
a per-man simulation.

Target-type gating lives on the **ammunition**, not the weapon — `Ammunitions.TargetType`
is a flags enum and `Weapons` has no equivalent field. **[inferred] What a weapon
can engage is the union of what its loaded ammunition can engage**, which is why
switching shell type changes what a gun will shoot at.

---

## 5. Fire distribution across units — and the contrast that matters

**[DUMP]** The anti-overkill layer lives on the **target**, keyed by shooter:

```csharp
public struct ShootPriorityComponent {
    public readonly bool ExcludeUnguidedOnThisUnit;
    public readonly Dictionary<Entity,ShooterTargetAimingInfo> Shooters;
    [TupleElementNames(new[]{ "damageScore","chanceToHit","timePassed" })]
    public readonly Dictionary<Entity,ValueTuple<GFloat,GFloat,GFloat>> IncommingMissiles;
    public readonly Dictionary<int,FireChannelData> ShootingChannels;
    public readonly GFloat UnitCost;
}

public class OverkillScoringSystem : ISystem<float>, IDisposable {
    private const int   THREATS_NUMBER_WARNING_TRESHOLD = 20;
    private const float OVERKILL_EXTRA_DISTANCE         = 10;
    private const float OVERKILL_FAILSAFE_RESET_TIME    = 0.5;
    [ThreadStatic] private static FastList<ValueTuple<float,float,bool>> _threatsBuffer;

    public static bool  IsTargetOverkilled(in Entity targetEntity, …);
    public static float CaclculateTargetDeathChance(in Entity targetEntity, in Entity weaponToExclude);
    private static float CalculateTargetDeathChanceRecursive(FastList<…> threats,
        float accumulatedDamage, float targetHealthScore, int indexToStart);
}
```

tuned by a block of designer switches — `PREVENT_GUIDED_OVERKILL`,
`PRIORITIZE_ONE_SHOT_TARGETS`, `MINIMAL_CHANCE_TO_KILL`,
`GUIDED_IGNORE_UNGUIDED_ON_PLANES`.

**[inferred]** Before committing, a weapon can read the accumulated damage every
other weapon *in the world* has already promised this target, and the metric is
recursive: **the probability the target dies without me**. That is a genuine
global optimisation.

**[inferred] And this is the sharpest cross-game contrast available.**
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) §3
records the opposite design, and states it flatly: *"No blackboard, no assignment
solver, no squad coordinator. Every shooter independently scores every target, and
the act of committing makes that target less attractive to everyone else."* Two
bytes per tracked unit, decentralised, degrading gracefully.

Broken Arrow builds the blackboard. It gets better fire distribution — genuinely
computing death probability rather than approximating it with a divisor — and pays
a dictionary per target, a thread-static threat buffer, a failsafe reset timer,
and a **warning threshold at 20 simultaneous threats** that says the recursion has
a practical ceiling somebody hit.

**The readings are both defensible and the difference is the unit count.** Nuclear
Option has dozens of shooters and a decentralised heuristic is free. Broken Arrow
has hundreds of weapon entities and wasting a volley on a dead tank is a visible
mistake in a genre where a volley is a purchase. **Centralise the coordination
when the resource being coordinated is expensive; approximate it when it is not.**

---

## 6. Being shot at

**[DUMP]** One collider in the entire infantry model, sized by headcount, tooltip
verbatim:

```csharp
[Tooltip("Base size of infantry squad hitbox to which soldiers numbers are added for extra size")]
[Range(1, 5)]
public uint BaseSquadSize;   // InfantryConfig
```

**[inferred]** A grep for `SquadCollider` across 1.75 M lines returns its
declaration and nothing else. There is no `SoldierCollider`, no per-man capsule.
As casualties accumulate `CurrentSquadSize` shrinks and the box shrinks with it,
so **a mauled squad is a physically smaller target** — a statistical "the
survivors are harder to hit", implemented as geometry.

`HitDetectionSystem` gives infantry its own larger pre-filter
(`INFANTRY_ADDITIONAL_PREFILTER_HITBOX_MULTIPLIER = 4` on top of
`UNITS_PREFILTER_HITBOX_MULTIPLIER = 5`) and resolves `ArmorSides` against the
*unit's* bounding box via `GetHitSide(in Entity target, in BoundingBox box, in GalaxyVector3 hitPoint)`.
**A squad has facing-dependent armour exactly like a tank**, resolved against the
squad box rather than whichever man is nearest. `GetUnitBox(Entity unit, Ammunitions ammoInfo)`
taking the ammunition as a parameter means the same squad presents a different box
to different shells.

Projectiles know nothing about any of it — `BallisticShellSystem` is
`[With(TransformComponent, BallisticShellComponent, SpeedComponent)]` and pure
kinematics.

---

## 7. Casualties are an arithmetic consequence

**[DUMP]**

```csharp
[With(new[] { typeof(InfantryUnitComponent), typeof(HealthComponent) })]
[Without(new[] { typeof(RemoveUnitComponent), typeof(DeathComponent), typeof(DeadComponent) })]
public class InfantryDeathHealSystem : EcsSetSystem<float> {
    private readonly Random _random;
    private void HealSquad      (in Entity unitEntity, int soldiersDiff, ref StackList<Entity> aliveSoldiers, bool isLoadedSquad);
    private void KillSquadSolders(in Entity unitEntity, int soldierDiff,  ref StackList<Entity> aliveSoldiers);
    private static void RecalculateSoldierWeaponAmmo(in Entity infantryEntity, in SoldierComponent changedSoldier, bool isPrimaryWeapon);
}
```

**Read `KillSquadSolders`: it takes an `int soldierDiff` — a count, not a victim.**
Nothing passes a hit position, a hit soldier or a damage source into casualty
selection. **[inferred]** The loop compares current health against `LatestHealth`,
converts the delta to a body count via `HealthPerSoldier`, and kills or heals that
many — one system for both directions, which is why they share the "diff"
vocabulary and why medics are free.

Who dies is **authored**:

```csharp
public class SquadMembers : IDataBaseModel {
    public int    DeathPriority   { get; set; }
    public string ModelFileName   { get; set; }
    public int    PrimaryWeaponId { get; set; }
    public int    SpecialWeaponId { get; set; }
    public int    UnitId          { get; set; }
}
```

**[inferred]** Riflemen die before the AT gunner; the man with the Javelin is last.
This is why losing two of nine does not halve a squad's anti-tank capability — a
designer chose the order, per squad, in a spreadsheet. The `Random _random` is
presumably a tie-break within equal priorities.

Then the knock-on chain closes:
**damage → squad HP → integer headcount → `ChangeWeaponCount` → `UsingAliveSoldierCount`
→ `WeaponAndSoldiers.Count("Current","Max")` → rate of fire**, plus
`StressModifiers.InfantryDmgModPerSoldier` with an `InfantryDmgModFloor` so a last
man standing is not useless. The UI is honest about the two readings of one
number: `InfocardConfig` carries separate `Health`, `HealthInfantry` and
`SoldierCount` sprites.

### 7.0a What the player actually sees as a squad thins

Pulling the visual consequences together, because they are spread across five
systems and the combination is the whole effect:

| As men die | What changes | Evidence |
|---|---|---|
| The man himself | plays `SoldierState.Death`, `IsAlive = false`, stays as a corpse **in place** | `SoldierState.Death`, `SoldierComponent.IsAlive` |
| Corpses | persist, then hide on a timer once the **whole squad** is gone | `[Tooltip("Delay for hide the corpses (All squad died)")] InfantryDeathTime` |
| The formation | **does not close ranks** — survivors keep their slots and gaps appear | `_pointCount` is `readonly`; see below |
| The hitbox | shrinks — `CurrentSquadSize` feeds the collider | `BaseSquadSize` tooltip, §6 |
| Rate of fire | drops continuously | `UsingAliveSoldierCount`, §4.5 |
| Capability | lost outright if a weapon's last carrier dies | §4.4b |
| Audio | thins — footstep density is a parameter | `IFmodInfantry.SetPeoplesCount(int count)` |
| The UI | shows live count against the spawn count | `WeaponInfoUpdateModel { int _count; int _maxCount; int InfantryRealMaxCount; readonly int _startInfMaxCount; }` |

**[inferred] On the formation not recompacting** — the argument is indirect but
fairly tight. `SquadFormation._pointCount` is `readonly`, so the lattice has a
fixed slot count decided at spawn. And the heal path needs a *search*:

```csharp
private void HealSquad(in Entity unitEntity, int soldiersDiff,
                       ref StackList<Entity> aliveSoldiers, bool isLoadedSquad);
internal int <HealSquad>b__7_0(SquadFormation x);     // returns int — a key selector
```

A revived soldier has to be *assigned* a slot by index. That only makes sense if
slots persist and dead men vacate them. If the formation recompacted on death,
healing would simply re-run `UseFormation` over the new alive set and no slot
search would be needed.

**[inferred]** Which is the right call visually as well as cheaply: a squad that
closed ranks after every casualty would appear to shuffle sideways under fire,
drawing the eye to exactly the moment you want to read as "they took losses".
Leaving gaps means the survivors stand still and the squad visibly thins — and it
costs nothing, because nobody re-plans anything.

### 7.1 Two things that are not there

**[DUMP]** `SoldierState` has fourteen members and **no prone or crouch**:

```csharp
Idle, Run, Sprint, Aim, Fire, Death, Reload, RunStress, RunPanic,
Parachute, InContainer, IdleInWater, RunInWater, SprintInWater
```

yet `InfantryConfig` ships `KneelRunSpeed`, `ProneRunSpeed` and `ProneSprintSpeed`
— all three carrying the **same copy-pasted tooltip**, *"Speed of soldiers when
sprinting at a constant speed"*. **[inferred] The stance speeds shipped and the
stance states did not.** Stance affects nothing about incoming fire, which is
consistent with everything else here: stance would be a per-soldier property, and
per-soldier properties do not participate in combat.

**And "cover" means concealment, not protection.** Tooltip verbatim:

```csharp
[Tooltip("Divides enemy vision distance by this when unit is on the given terrain type")]
[Range(1, 10)] public float Cover;    // VisibleTerrainType
```

**[inferred]** Woods do not make a squad harder to *hurt*; they make it harder to
*see*, which is upstream of being shot at at all. Suppression is likewise
squad-level and every field in `StressModifiers` degrades the squad's own output —
none makes it easier to hit. Note `BuffConfig`'s tooltip: *"If damage is bigger
than this (after applying AoE modifier), stress damage modifier is not applied to
(infantry) unit"* — **past a threshold you do not get suppressed instead of
killed.**

Area effects resolve **once per squad**: `HitDamageInfo.TargetUnit` is singular and
the AoE sweep enumerates `UnitsPositionsTree` by `UnitType`. **[inferred]** A shell
landing among nine men produces one damage event scaled from the squad's single
position; how many men that happens to equal is decided afterwards by
`HealthPerSoldier`. Members share one position for blast purposes. **"How many
members did the shell catch" is not a quantity the engine represents.**

The exception that proves the rule is `CloseQuartersCombatSystem` — and even there
`CloseQuartersCombatComponent.EnemyUnits` is a list of *units*, and damage derives
from the squad's surviving weapon mix rather than from duels.

---

## 8. Rendering: gated, not instanced

**[DUMP]** Each soldier is an ordinary GameObject:

```csharp
public sealed class InfantryData {
    public GameObject ViewObject;
    public Transform  SpineTransform, WeaponPositionTransform;
    public Renderer[] MeshRenderers;
    public SoldierWeapon PrimaryWeapon, SpecialWeapon;
    public SoldierAnimationManager AnimationManager;
    public SoldierAnimatorComponent AnimatorComponent;
}
```

built one at a time in an async loop from `SquadMembers` rows.

**GPUInstancer never touches units** — its only in-house type is a one-method
`IGPUInstancerManagerBridge { void SetCamera(Camera); }`, and every
`GPUInstancerPrefabManager` reference is scenery. `ProjectDawn.Impostor` has **no
namespace in the dump at all**; the only impostor fields in the game are hand-rolled
`ImpostorRenderer` on `BuildingInitializer`. **Soldiers never become billboards.**

So the scale answer is gating, and **the LOD unit is the squad**:

```csharp
public class BaLodGroup : MonoBehaviour {
    private GameObject _infContainer;      // singular
    private bool _infSoldiersActive;
    private bool _infHasComp;
    public void Initialization(GameObject lodRootObj, LODGroupComponent, bool isInfantry);
    private void SetInfantrySoldiersState();
}
public class LODGroupComponent {
    public LODGroupEnum Lod;  public bool VisibleByCamera, FOWVisible;
    public void RefreshInfantryAnimatorState();
    public void SaveLastAnimatorState(bool reset);
}
```

**[inferred] `RefreshInfantryAnimatorState()` being a method on the LOD component
is the whole design.** Animation state is a first-class output of the LOD tier.
The chain runs: `CullingGroup` band change → `LODGroupSystem` → `SetState` →
`SetInfantrySoldiersState()` → `DisableSoldiersAnimator()` / `DisableInfantryWeapons()`.
Past a tier the squad's `Animator`s are switched off wholesale, leaving posed
statues. **They do not draw soldiers more cheaply; they stop paying for animation.**
Because disabling an `Animator` loses its state, both sides carry an async
save/restore that yields a frame before reapplying.

Three culling axes funnel into one call — distance band, camera frustum, and fog
of war — so an enemy squad in the fog costs nothing. One `BaLodGroup` per squad
also cuts the culling-group population eightfold.

**Battle animation bypasses Mecanim state machines entirely.** A `SoldierState`
maps to an **array of clip names** pre-hashed at edit time
(`SoldierAnimationStates { StateName, AnimationNames[], AnimationHashes[] }`), and
`SetAnimation` picks one at random, remembering the index so a LOD re-enable can
restore rather than re-roll. Selection is `InfantryUnitSystem`'s priority list of
`TrySet*` methods — parachute, reload, fire, aim, sprint, run, idle, first match
wins. Mecanim proper is used only for the hangar viewer, which is trigger-driven.
IK is one built-in `LookAtConstraint` per man aiming the weapon; there is no foot
IK and no full-body solver.

### 8.1 The lockstep fix, in four animator parameters

**[DUMP]**

```csharp
public static class InfantryConstants {
    public const float ANIMATION_RELOAD_TIME = 3.12;
    public const float ANIMATION_FIRE_TIME   = 0.05;
    public static readonly int Reload, Fire, Aim, Idle, Parachute, Stress, Panic,
                               Aiming, Running, Reloading, InWater;
    public static readonly int IdleMirrorHash;       // 0x2C
    public static readonly int RandomValue;          // 0x30
    public static readonly int ReloadSpeedMultHash;  // 0x34
    public static readonly int IdleSpeedHash;        // 0x38
    public static readonly int IdleCycleOffsetHash;  // 0x3C
}
```

**[inferred]** These last four have no other plausible purpose.
`IdleCycleOffsetHash` starts every man's idle loop at a different phase;
`IdleSpeedHash` makes those phases *drift apart* rather than holding a fixed
offset; `IdleMirrorHash` mirrors half the squad so they are not all weighted on
the same foot; `RandomValue` feeds blend thresholds for fidgets. **That is the
textbook trio plus one, which means they hit the marching-clones problem and
solved it deliberately.** Combined with §2.5's start-delay, position jitter and
scale variation, six independent randomisations exist purely so nine men do not
look like one man copied.

### 8.2 Two details worth stealing

**[DUMP]** `SoldierAnimationManager` exposes `public void FireEnd()` and
`public void ReloadEnd()` — parameterless voids, i.e. **`AnimationEvent` targets**.
**[inferred] The reload completes when the clip says it does**: animation drives
simulation rather than the reverse.

And six separate procedural behaviours each carry the identical field and tooltip:

> `[Tooltip("If current unit lod higher effect will not be updated")]`
> `private LODGroupEnum _lodGroup;`

alongside `IAnimationBehaviour.StartQuality`, a minimum graphics preset below which
a behaviour does not run at all. **[inferred] Every effect declares its own budget
rather than being switched off by a central list** — which is what stops the LOD
system becoming a registry of everything in the game.

**[inferred] Notably absent: no distance-based update throttling anywhere.** No
tick rate, no frame slicing, no round-robin. The saving is entirely binary —
either the entity leaves the query set (`[Without(LoadedComponent)]` means a squad
in a truck costs *exactly zero* movement and animation work) or the animator is
off. Systems run parallel across squads at ≥32 per thread.

Even audio follows: one FMOD instance per **squad**, with
`IFmodInfantry.SetPeoplesCount(int count)`. **Nine sets of footsteps are a
parameter, not nine emitters.**

---

## 9. What transfers

Judged against RTS / FPS / third-person as CLAUDE.md requires:

1. **Decide what the *unit of simulation* is before anything else, and make the
   type system enforce it.** Squads are in the spatial tree; soldiers cannot be,
   because there is no soldier `UnitType`. Every downstream system inherits the
   decision for free and cannot accidentally violate it. **Enforcing an
   architectural rule through a type is worth more than documenting it** — the
   same argument as CLAUDE.md's cromwell/game separation being checked by the
   build rather than trusted.

2. **Put shared state on the thing that is shared.** Making the *weapon* an entity
   — with its own target, aim timer and reload — is what lets one squad engage
   three enemies, one tank run a coax and a main gun independently, and infantry
   reuse the entire vehicle combat stack. **The interesting entity is often
   neither the thing you draw nor the thing you select.**

3. **Preserve one honest feedback channel through the abstraction.**
   `UsingAliveSoldierCount` dividing the weapon's cycle time is the whole reason
   the model reads as real: casualties visibly cost firepower. A squad-level
   abstraction with no such channel feels like a health bar with men drawn on it.

4. **Get locomotion character from constants, not code paths.** Infantry pivot
   crisply and tanks swing wide because of `10` versus `30` against a shared 45°
   tolerance. Two numbers, one implementation. **Before writing a second movement
   system, check whether the first one with different data would do.**

5. **Rigid structure plus randomised presentation beats a soft simulation.**
   Fixed hex slots — so cohesion is impossible to lose — with six independent
   randomisations layered on top (start delay, position jitter, scale, idle phase,
   idle speed, mirror). All the visual benefit of loose formation behaviour, none
   of the simulation, and none of the failure modes.

6. **Schedule the animation ahead of the event.** `AimedSoldiersQueue` pre-aims
   the next shooter so the muzzle flash comes from a man already in an aim pose.
   **A cheap simulation reads as expensive when its presentation is scheduled
   rather than reactive** — and this generalises to any shared resource with
   multiple visual representatives.

7. **Gate binary, do not throttle.** Removing an entity from a query set is exact
   and free; running it at a lower rate is approximate and still costs. Broken
   Arrow has no distance throttling at all — `[Without(LoadedComponent)]` beats
   any tick-rate scheme.

8. **Make the LOD tier a first-class input to non-render systems.** Animator state
   is an output of `LODGroupComponent`; every procedural behaviour declares its own
   cutoff tier and minimum quality preset. LOD stops being a renderer concern and
   becomes a budget every system spends against.

9. **Enumerate why a decision failed.** Thirty-three `TargetRemoveReason` values
   and a `ReportedSystem` attribution turn "my AA did not fire" from an
   unfalsifiable complaint into a lookup. **The profiler-zone argument applies to
   decision systems, not just frame time.**

10. **Size coordination to the cost of the resource.** Broken Arrow's global
    overkill blackboard is right for hundreds of expensive weapons; Nuclear
    Option's two-byte decentralised counter is right for dozens of cheap ones.
    Neither is the better technique in the abstract.

---

## 10. What is not established

- **No method bodies.** Every claim about *how* a system sequences its work —
  the turn/move gate, the casualty loop, the aiming queue, the rate-of-fire
  division — is inference from signatures, constants and filters. The ECS filters
  are literal and load-bearing; the control flow between them is not.
- **No authored values.** `DistanceBetweenSoldiers`, `StartMovingMaxDelay`, the
  LOD band distances, `BaseSquadSize`, every `DeathPriority` — all live in
  `BrokenArrowDB.bytes` and ScriptableObject assets, not the binary. The shape is
  established; the numbers are not.
- **Nothing was run or profiled.** No claim here about cost, frame time, or how
  many squads the game supports.
- **The rate-of-fire division is the weakest strong claim.** That
  `CalculateWeaponTimeTillNextShot` takes the component holding
  `UsingAliveSoldierCount` is literal; that it divides by it is the natural
  reading, corroborated by `WeaponHelper.RateOfFire(Weapons, int countOfWeapon)`,
  but not read.
- **One correction already applied.** `RotateUnitSystem` was initially described
  as serving all ground locomotion; its filter is
  `[WithEither(UnitGroundVehicleFlag, UnitWaterVehicleFlag)]` and it excludes
  infantry. Assume other filters deserve the same scepticism.

**To go further**: `BrokenArrowDB.bytes` is a SQLite database with `SquadMembers`,
`SquadWeapons`, `Weapons` and `Units` tables, all reachable via
`IDataBaseModel` + `[PrimaryKey]`. Reading it would give real squad rosters — how
many men, which weapons, in what death order — and turn most of §7 from a
mechanism into a worked example.

---

## Sources

**[DUMP]** Il2CppDumper v6.7.46 over
`C:\Program Files (x86)\Steam\steamapps\common\broken_arrow\GameAssembly.dll` and
`BrokenArrow_Data\il2cpp_data\Metadata\global-metadata.dat` (IL2CPP metadata v31,
dated 2025-08-06). Method bodies are native and absent; types, fields, offsets,
signatures, `const` values, ECS filters and Unity attributes are literal.

Cross-referenced against
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md) §3 and
§7–8, which is **[CODE]** — decompiled C# with bodies — and therefore outranks
anything inferred here where the two disagree.
