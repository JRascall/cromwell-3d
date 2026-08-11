# Broken Arrow — seeing and searching: a 6-byte grid, and a cadence made of set membership

How does a unit find an enemy? How often does it look? Does it cast rays, or read
a grid? And how does any of that work for a helicopter at 8 metres, which is below
the treetops but is not a ground unit?

> **Line of sight is both a grid and raycasts, layered.** A baked
> **six-byte-per-pixel** terrain array is marched with **Bresenham at one metre per
> step**, off the main thread, accumulating opacity. Real physics raycasts are
> issued **only where the grid march already said vision was clear** — cull cheap,
> confirm expensive.
>
> **The march returns a histogram, not a number.** `int[] CountOfTerrainTypes` —
> how many metres of each terrain the sightline crossed. That is why *seeing* and
> *being able to shoot* are separate sets, and why three unrelated rules
> (visibility, shootability, laser designation) each get their own forest
> threshold from **one** trace.
>
> **Altitude needs no special code.** One tooltip — *"At what height from the
> ground and above, do not use visibility modifiers"* — makes the march
> height-aware, and then `FOREST_HEIGHT = 25` against a helicopter's low altitude
> of **8 m** and high altitude of **40 m** produces nap-of-the-earth gameplay with
> **no helicopter-specific vision code at all**.
>
> **Search cadence is not a timer. It is membership in an ECS set.**
> `TargetSearchSystem` is `[Without(TargetSearchDelayComponent)]`, so a weapon that
> just searched is *physically not in the query*. A 2 Hz search costs nothing on
> the other 58 frames — no iteration, no branch, no cache line touched.
>
> **Helicopters never choose between air and ground targets**, because nothing
> chooses. Each weapon entity searches independently against its own ammunition's
> target mask, and the KD-tree partition *is* the filter.

> **Source.** **[DUMP]** from the retail Il2CppDumper output; see
> [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) for the full method note.
> Method bodies are absent, so control flow is **[inferred]**; type names, fields,
> offsets, signatures, `const` values, ECS filters and Unity attributes are
> literal. Load-bearing declarations were re-read directly against the dump.
>
> One correction worth recording: an earlier pass of this research listed
> `PlaneVSHeliToleranceAltitude`, `PlaneVSHeliAdvanceMult` and an `AnimationCurve
> PlaneVsHeliStopAltitude`. **None exists in the retail build** — they came from
> the alpha dump. The only plane-versus-helicopter tunables that shipped are
> `PlaneVsHeliDiveAngle` and `PlaneVsHeliNoseUpExtraHeight`.

Related: [`broken_arrow_squads.md`](broken_arrow_squads.md) §4 (who targets what —
the weapon entity, not the unit),
[`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) §6.3,
[`broken_arrow_vehicles.md`](broken_arrow_vehicles.md) §2 (the same `MapPixel`
grid serving terrain speed),
[`spatial_queries.md`](../../../topics/agents/spatial_queries.md) (CryEngine's
"raycasts are the dominant cost of the whole AI system" and the sort-then-test
rule this note is a worked example of),
[`nuclear_option_command.md`](../nuclear_option/nuclear_option_command.md).

---

## 1. The grid

### 1.1 Six bytes per cell

**[DUMP]**

```csharp
// Namespace: BrokenArrow.Client.Ecs.Navigation
[IsReadOnly]
public struct MapPixel {
    public readonly float             Height;      // 0x0
    public readonly TerrainType       TerrainData; // 0x4   (byte)
    public readonly NavMeshMetaLayers MetaNavMask; // 0x5   (byte)
    public void .ctor(TerrainType type, float height, byte metaMask);
}
```

That is the entire world representation for terrain queries: **a 4-byte height, a
1-byte terrain type, a 1-byte navigation mask.**

```csharp
public class MapMetaData {
    public const TerrainType BRIDGE_FLAG_FILTER = 240;          // 0xF0
    private const float FLAT_PIXEL_HEIGHT_EPSILON = 0.001;
    private readonly MapPixel[] _pixels;      // 0x30   ← one flat array
    private readonly byte[]     _flatPixels;  // 0x38
    private readonly int        _stride;      // 0x40
    private readonly Vector2    _delta, _heightSampleDelta;
    private readonly int _deltaXInt, _deltaYInt, _deltaXShift, _deltaYShift;
    private readonly int _maxX, _maxY;
    public ref MapPixel GetPixel(int x, int y);
    public ref MapPixel GetPixelWithFlatSafe(Vector3 position, out bool isFlat);
    private static bool IsPowerOfTwo(int value);
    private static int  Log2Int(int value);
    private int WorldToPixelX(float worldX);
    private int WorldToPixelY(float worldZ);
}
```

**[inferred] Three things.** One flat row-major array indexed by arithmetic — no
quadtree, no chunking, exactly the layout CLAUDE.md's DOD section prescribes for a
spatial query layer. World→pixel is a **bit shift**, which is the only reason to
compute `Log2Int` of a power-of-two delta and cache it. And the `ref`-returning
accessors mean the six-byte struct is never copied in the hot path.

`BRIDGE_FLAG_FILTER = 240` masks the high nibble, so `TerrainType`'s low nibble
(0–15) carries the surface and the high nibble carries four bridge flags:

```csharp
public enum TerrainType : byte {
    None=0, Vegetation=1, Forest=2, Buildings=3, HighSlopes=4, Water=5,
    Road=6, LowWater=7, Concrete=8, Smoke=15,
    FlagBridgeOne=16, FlagBridgeTwo=32, FlagBridgeThree=64, FlagBridgeFour=128
}
```

**[inferred]** A unit can be on a bridge *and* on concrete, but cannot be on forest
*and* water — so the packing is exactly right for the question being asked.

The parallel `byte[] _flatPixels`, built by `PostProcessFlatPixels` against a 1 mm
epsilon and read by `GetPixelWithFlatSafe(..., out bool isFlat)`, is **[inferred]** a
precomputed "this neighbourhood is level" bit so height sampling can skip bilinear
interpolation over most of the map. That is CLAUDE.md's derived-cache pattern with
its escape hatch: the fast path only skips work that provably does nothing.

### 1.2 It is decoded from a texture

**[DUMP]** `MapSettings` carries `Vector2 MapSize`, `Vector2 TextureSize` and
`[HideInInspector] public Texture2D BakeTexture`, and `MapMetaData` has
`InitAsync(MapSettings)` plus `private MapPixel ConvertPixel(Color unityPixel)`.

**[inferred] The entire terrain database is decoded from a baked RGBA texture at
map load** — height, terrain type and nav mask packed into colour channels. The
developers call the cell a *pixel* throughout, including in a designer-facing
tooltip (*"Vision cost of map pixel"*), which is the lineage showing.

### 1.3 The trace primitives

**[DUMP]**

```csharp
public bool CustomRaycast(Vector3 source, Vector3 target, out MapPixelHit hit, CheckPixelDelegate checkDelegate);
public bool SamplePixel(Vector3 source, float maxDistance, CheckPixelDelegate, out MapPixelHit);
public bool SampleNavMesh(Vector3 source, float maxDistance, NavMeshMetaLayers searchMask, out MapPixelHit);
public bool PhysicalLinecast(Vector3 source, Vector3 target, out MapPixelHit hit);
public bool NavmeshLinecast(Vector3 source, Vector3 target, out MapPixelHit hit, NavMeshMetaLayers metaMask);
public void ScanLine<T>(Vector3 source, Vector3 target, ScanAreaDelegate<T> scanLogic, ref T dataAgregator);
public void ScanArea<T>(Vector3 source, float maxDistance, ScanAreaDelegate<T> scanLogic, ref T dataAgregator);
private bool BresenhamLinecast(int sourceX, int sourceY, int targetX, int targetY,
                               out MapPixelHit hit, CheckPixelDelegate pixelCheck);
```

**The march is Bresenham, named outright.** `CheckPixelDelegate` is
`bool Invoke(in MapPixel pixel, int x, int y)` — a stop predicate;
`ScanAreaDelegate<T>` is the accumulating variant taking `ref T`.

A naming trap worth flagging: **`MapMetaData.PhysicalLinecast` is a *grid*
linecast, not a physics one.** "Physical" means "against terrain solidity" and it
returns a `MapPixelHit`.

---

## 2. What the vision march actually computes

**[DUMP]**

```csharp
public struct FilterTask {
    public Entity  Caster;                       // 0x0    ─┐
    public Entity  Target;                       // 0x8     │
    public Vector3 CastStartPosition;            // 0x10    │ inputs
    public Vector3 CastEndPosition;              // 0x1C    │
    public bool    IgnoreTerrainCosts;           // 0x28    │
    public bool    IgnoreForestCostOnly;         // 0x29    │
    public bool    IgnoreForcedVisibilityDistance;// 0x2A   │
    public float   MaxVisionDistance;            // 0x2C   ─┘
    public bool    IsFinished;                   // 0x30   ─┐
    public Vector3 VisionEndPosition;            // 0x34    │
    public float   OpacityDistance;              // 0x40    │ outputs
    public bool    TargetVisible;                // 0x44    │
    public int[]   CountOfTerrainTypes;          // 0x48   ─┘
}
internal const int FILTER_STEP_LENGTH = 1;   // FogOfWarConfig
```

with the cost table authored per terrain type, tooltips verbatim:

```csharp
[Serializable] public class VisibleTerrainType {
    [SerializeField] public TerrainType TerrainType;
    [Tooltip("Vision cost of map pixel. ( bigger numbers = worse visibility through terrain type )")]
    [SerializeField] public float Opacity;
    [Tooltip("Divides enemy vision distance by this when unit is on the given terrain type")]
    [Range(1, 10)] [SerializeField] public float Cover;
    [Tooltip("At what height from the ground and above, do not use visibility modifiers")]
    [SerializeField] public float Height;
    [Tooltip("How much more expensive each next meter will be than the previous one")]
    public float Multiplier;
}
```

and flattened at init into a struct the worker thread actually touches:

```csharp
[IsReadOnly] public struct TerrainVisionData {
    public readonly float Opacity;  // 0x0
    public readonly float Cover;    // 0x4
    public readonly float Height;   // 0x8
    public readonly float Multiplay;// 0xC
}
public static TerrainVisionData[] TerrainsData;   // on FogOfWarSystem
private void GenerateVisionData() { }
```

**[inferred] The authored/runtime split is deliberate and exactly right.**
`VisibleTerrainType` is a managed class with `[Tooltip]`s, existing for the
Inspector. `TerrainVisionData` is a 16-byte readonly struct in an array **indexed
directly by the terrain byte**, so the marcher's inner loop is one array read with
no dictionary and no virtual call. Authoring ergonomics and hot-loop layout are
different problems and they gave each its own type.

**[inferred] The march**, at one metre per step: read `MapPixel.TerrainData`,
increment `CountOfTerrainTypes[terrain]`, add that terrain's `Opacity` escalating
by `Multiplier` per metre already spent in it, and stop when the budget is
exhausted — writing the stop point to `VisionEndPosition`, the distance to
`OpacityDistance`, and setting `TargetVisible` if it reached the target first.

### 2.1 The histogram is the design

**[DUMP]** `CountOfTerrainTypes` is an `int[]` **indexed by terrain type**, not a
scalar opacity total. The payoff is in a signature:

```csharp
private void UpdateVisible (Entity target, FilterTask filterTaskResult);
private void UpdateSee     (Entity target, in FogOfWarComponent unitFow, FilterTask filterTaskResult);
private void UpdateShootable(in FogOfWarComponent unitFow);          // ← no FilterTask
public static bool FogOfWarHelper.IsTargetShootable(FilterTask task, bool unitCheck);
public float MaxShootableForestDistance;   // FogOfWarConfig
```

**`UpdateShootable` does not take a `FilterTask`.** It reads the one retained in
`FogOfWarComponent.PotentialSee` — which is a `Dictionary<Entity, FilterTask>`
precisely so the whole result survives — and re-evaluates on a *different tick*
without re-marching.

**[inferred] So `VisibleTo` and `Shootable` are a real gameplay rule, not an
optimisation.** Seeing a target is `TargetVisible`. *Engaging* it additionally
requires that the sightline crossed fewer than `MaxShootableForestDistance` metres
of **forest specifically** — a question a scalar opacity total could never answer.
The same histogram serves the laser designator:

```csharp
[Tooltip("Max forest range that laser can cross.")]                                  public float LaserForestMaxRange;
[Tooltip("Range around the designator unit in which forest is ignored when lasing.")] public float LaserOwnForestRange;
```

**Three systems, three different forest thresholds, one trace.** That is the piece
worth stealing: **when a query result will be interrogated by several rules, return
the decomposition rather than the verdict.** It costs one array per task and saves
two entire traces.

---

## 3. Grid first, colliders second

**[DUMP]** The observer object:

```csharp
public sealed class FogOfWarUnit {
    private readonly RaycastBatchService _raycastBatchService;
    private readonly FogOfWarFilter      _filter;
    private RaycastCommand[]  _castBuffer;
    private CastData[]        _castDataBuffer;
    private readonly float _scanPeriod;        private float _lastScan;
    private readonly float _scanPeriodGround;  private float _lastScanGround;
    private bool _awaitingRaycastResult, _filterTaskCreatingTick, _shootableUpdatingTick;
    private readonly Stack<FilterTask> _filterTasks;
    private bool IsUnitScanTick();
    private void UpdateGroundPoints(ref FogOfWarComponent fowCom);
    private void AddFilterTasks(Vector3 unitEyePosition);
    private void OnFilterResult(FilterTask[] results);
    private void OnRayCastResult(Entity ownEntity, Span<RaycastHit> results);
}
```

and the compiler-generated closures inside `UpdateGroundPoints`:

```csharp
private sealed class FogOfWarUnit.<>c__DisplayClass41_1 {
    public FogOfWarTargetPoint groundPoint;
    internal void <UpdateGroundPoints>g__pointFilteringCallback|1(FilterTask[] results) { }
}
private sealed class FogOfWarUnit.<>c__DisplayClass41_2 {
    public float basicCastOptics;
    internal void <UpdateGroundPoints>g__pointRaycastCallback|0(Entity ownEntity, Span<RaycastHit> results) { }
}
```

**[inferred] The nesting is the evidence.** `_2` (the raycast closure) captures
`_1` (the filter closure), meaning the raycast closure is *created inside the
filter callback's scope* — so **the physics query is issued only where the terrain
march already returned clear vision.**

And the two results meet in one object:

```csharp
public sealed class FogOfWarTargetPoint {
    public bool IsVisionClear;                 // 0x11
    public bool IsShootable;                   // 0x12
    public Nullable<Vector3> RayHitPoint;      // 0x14   ← physics
    public Nullable<float>   RayHitDistance;   // 0x24   ← physics
    public Nullable<float>   FilterHitDistance;// 0x2C   ← grid
    private int _targetColliderId;             // 0x40
}
```

**[inferred] Unit-versus-unit spotting uses the grid only — no raycast at all.**
That is the O(observers × targets) work, and it never touches a collider. Raycasts
are reserved for ground points, where the exact collider matters.

### 3.1 Raycasts are batched into one job

**[DUMP]**

```csharp
public interface IRaycastBatchService {
    void RequestRaycast(Entity ownEntity, RaycastCommand  command,  RaycastBatchCallbackDel callback);
    void RequestRaycast(Entity ownEntity, RaycastCommand[] commands, RaycastBatchCallbackDel callback);
}
public sealed class RaycastBatchCallbackDel : MulticastDelegate {
    public virtual void Invoke(Entity ownEntity, Span<RaycastHit> result);
}
private struct RaycastBatchService.RaycastBatchData {
    public Entity OwnEntity; public int StartIndex; public int Count; public RaycastBatchCallbackDel Callback;
}
public class RaycastBatchService : IRaycastBatchService {
    private RaycastCommand[] _commandsBuffer;
    private RaycastHit[]     _hits;
    private void EnsureCastBufferCapacity(int requiredCapacity);
    private bool TryCopy<T>(T[] src, NativeArray<T> dst, int count);   // only instantiated for RaycastCommand
    public void Update();
}
```

**[inferred]** Every caller in the frame appends into one shared command array,
recording only `(StartIndex, Count, Callback)`. `Update()` copies into a
`NativeArray<RaycastCommand>`, schedules **the whole frame's raycasts as a single
batched job**, and fans results back as `Span<RaycastHit>` windows into `_hits` —
zero per-callback allocation, buffers grown but never reallocated per frame.

The reason this exists is that individually-called `Physics.Raycast` cannot be
jobified. **Batching converts N scattered blocking queries into one parallel job**,
which is the same "amortise the expensive thing" instinct as §5's cadence.

### 3.2 The threading contract

**[DUMP]**

```csharp
public class FogOfWarFilter {
    private readonly ConcurrentQueue<ValueTuple<Stack<FilterTask>, Action<FilterTask[]>>> _input;
    private readonly ConcurrentQueue<KeyValuePair<Action<FilterTask[]>, FilterTask[]>>    _callBackQueue;
    private readonly MapMetaData    _metaData;
    private readonly FogOfWarConfig _config;
    public  void AddTask(Stack<FilterTask> tasks, Action<FilterTask[]> callback);
    public  void Update();
    public  void ExecuteCallbacks();
    private void Run();
    public  void SingleWork(ref FilterTask task);
    private void Filter(ref FilterTask task);
}
```

**[inferred] The contract is "worker reads immutable, main thread writes".** The
worker touches only `MapMetaData` (immutable after load), `TerrainsData`
(immutable after `GenerateVisionData`) and smoke (§6, analytic and thread-safe by
construction). Callbacks are drained on the main thread, so `FogOfWarComponent` is
never written concurrently. **No locks anywhere in the type** — two
`ConcurrentQueue`s and a discipline.

`AddTask` takes a whole `Stack<FilterTask>` plus one callback: **one enqueue per
observer per tick, not per target.** `SingleWork(ref FilterTask)` is the
synchronous escape hatch for a caller that cannot wait a frame. And `Filter` takes
`ref`, so the 76-byte struct is mutated in place and never copied.

Workers are per-team:

```csharp
[TupleElementNames(new[] { "TeamData", "Worker" })]
private readonly ValueTuple<TeamData, IFogOfWarWorker>[] _teamsFogWorkers;

public class FogOfWarWorker : IFogOfWarWorker {
    private readonly HashSet<FogOfWarUnit> _teamUnits;   // observers
    private readonly HashSet<FogOfWarUnit> _enemyUnits;  // candidates
}
```

---

## 4. Altitude, and why helicopters need no special code

### 4.1 Three bands, doubly debounced

**[DUMP]**

```csharp
public enum GlobalAltitude : byte { Ground = 0, Low = 1, High = 2 }

public struct AltitudeComponent {
    public float UnitAltitude;      // 0x0   AGL
    public float GroundAltitude;    // 0x4   terrain height
    public float GlobalAltitude;    // 0x8   absolute
    public Nullable<float> PlaneHighAltOverride;  // 0xC
    public GlobalAltitude GlobalAltitudeLevel;    // 0x18  ← the quantised band
    public float UpdateAltitudeLevelTime;         // 0x1C
    public float ResetUpdateTime { get; set; }
}

[With(new[] { typeof(AltitudeComponent), typeof(TransformComponent) })]
[Without(new[] { typeof(DeadComponent) })]
[WithEither(new[] { typeof(UnitHelicopterFlag), typeof(UnitAircraftComponent) })]
[WithoutEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag), typeof(UnitInfantryFlag) })]
public class AltitudeSystem : EcsSetSystem<float> {
    private const float HIGH_TO_LOW_ENTER_VALUE = 0.3;
    private const float LOW_TO_HIGH_ENTER_VALUE = 0.4;
    private void UpdateAltitudeLevel(Entity entity, float deltaTime, ref AltitudeComponent altitudeCom);
}
```

**Only helicopters and aircraft are in the set.** Ground, water and infantry are
`Ground` by omission.

**[inferred] The two constants are a Schmitt trigger** — you enter Low below 30%
of `PlanesConfig.HighAltitude` and only return to High above 40%. On top of that,
`UpdateAltitudeLevelTime` / `ResetUpdateTime` / `UpdateResetTime()` add a **time**
debounce over the **value** debounce. Two independent dampers on one three-valued
output is a strong hint they got burned by band flicker in testing — and it
matters because the band feeds range selection, so flapping would make a SAM's
engagement envelope flicker.

**[inferred]** The band is also *networked as one byte*
(`SupplyInitData.GlobalAltitudeLevel` is `[ProtoMember(8)] public byte`) rather
than as a float altitude — quantise once, replicate the quantised value.

### 4.2 The band selects a range, not an algorithm

**[DUMP]** Three optics ranges per sensor and three engagement ranges per
ammunition, in exact parallel:

```csharp
public class Sensors : IDataBaseModel {
    public float OpticsGround { get; set; }
    public float OpticsLowAltitude { get; set; }
    public float OpticsHighAltitude { get; set; }
}
// Ammunitions
public float MinimalRange, GroundRange, LowAltRange, HighAltRange { get; set; }
public float NonSeadProjectileRangeOverride, SeadProjectileRangeOverride { get; set; }
```

surfaced to the player as three separate infocard rows (`TextAmmoRangeGround`,
`TextAmmoRangeLowAlt`, `TextAmmoRangeHighAlt`), and consumed by two functions
whose signatures give away the broad/narrow split:

```csharp
public static float GetAmmoShootingDistance(Entity shooterUnitEntity, Entity targetEntity, Ammunitions ammoInfo);
public static float GetAmmoMaxShootingDistance(in Entity shooter, Ammunitions ammoInfo);
```

**[inferred]** The first takes a **target** and returns one number — it switches on
the target's band. The second takes **no target** and returns the max across
bands — it is the KD-tree query radius. **Search with the generous radius, reject
per-candidate with the exact one.** That is CLAUDE.md's "cull cheaply before
testing expensively", implemented as two functions with deliberately different
signatures.

### 4.3 The height trick

The mechanism that handles everything not on the ground is one tooltip:

> `Height` — *"At what height from the ground and above, do not use visibility modifiers"*

**[DUMP]** `TerrainVisionData.Height` carries it into the worker's hot table, and
`MapMetaData` exposes exactly the query needed:

```csharp
public void GetTerrainAtHeight(in Vector3 point, out TerrainType terrain, out float terrainHeight);
```

**[inferred] The march is 3D.** At each step it has the pixel's ground `Height` and
the sightline's own Y (interpolated between the two endpoints). If the ray is
above `pixel.Height + TerrainsData[terrain].Height`, that step contributes **zero
opacity** — the march still walks, it just stops charging. And because `Height` is
**per terrain type**, a forest canopy and a building roofline are different
altitudes.

Then the numbers do the rest:

```csharp
public const float FOREST_HEIGHT                    = 25;
public const float HELICOPTERS_HOVER_ALTITUDE       = 1;
public const float HELICOPTERS_CARGO_ALTITUDE       = 2.5;
public const float HELICOPTER_PREFFERED_ALTITUDE_LOW  = 8;
public const float HELICOPTER_PREFFERED_ALTITUDE_HIGH = 40;
```

| | |
|---|---|
| Low helicopter at **8 m** | **below** the 25 m canopy — inside forest opacity, as observer *and* as target |
| High helicopter at **40 m** | **above** it — sees over everything, is seen by everything |
| Hovering/unloading at **1–2.5 m** | effectively a ground unit |

**[inferred] Nap-of-the-earth flying is a consequence of `8 < 25 < 40`, not of a
branch.** There is no helicopter-specific vision code anywhere in `FogOfWarUnit` —
one `AddFilterTasks`, one `CreateFilterTaskForTarget`, one `Filter`. The only
thing that changes is where the eye is.

**This is the note's best single idea.** A height-aware grid march made three unit
domains — ground, rotary, fixed-wing — share one occlusion algorithm, and turned
what would have been three code paths into four authored numbers.

Helicopters *do* get a special **navigation** path against the same grid, and it
is the only user of `ScanLine<T>`/`ScanArea<T>` in the binary:

```csharp
private struct HelicopterNavigationSystem.FlyHeightData {
    public Nullable<MapPixel> MaxSurfacePixel;
    public Nullable<MapPixel> MaxVolumedPixel;
    public Nullable<float>    MaxBridgeHeight;
}
private readonly LayerMask _castMask, _volumedObstaclesMask;
private void HelicopterPixelHeightScan(in MapPixel pixel, ref FlyHeightData dataAgregator, int x, int y);
private void HelicopterRaycastCallback(Entity callbackEntity, Span<RaycastHit> results);
```

**[inferred]** Sweep the grid for the tallest surface, tallest volumed obstacle and
highest bridge, then confirm with batched raycasts. **The same grid-then-colliders
pattern, arrived at independently in a second subsystem.**

---

## 5. Cadence: a set, not a timer

**[DUMP]**

```csharp
[With(new[] { typeof(WeaponComponent), typeof(ParentComponent) })]
[Without(new[] { typeof(GroundTargetComponent), typeof(TargetSearchDelayComponent),
                 typeof(NetworkWeaponRemoteComponent) })]
public class TargetSearchSystem : EcsSetSystem<float> { }

public struct TargetSearchDelayComponent { public float TimeRemained; }

public static class TargetSearchHelper {
    public const int MAX_MS_DELAY    = 500;
    public const int MAX_AA_MS_DELAY = 200;
    private static void DelayNextSearch(in Entity weaponEntity, WeaponComponent weapon);
}
// WeaponComponent
public readonly bool  IsAntiAir;          // 0xDA
public readonly float TargetSearchDelay;  // 0xDC
```

**[inferred] This is the architectural point.** Cadence is not a timer a system
polls — **a weapon that just searched carries `TargetSearchDelayComponent` and is
therefore not in the query at all.** A 2 Hz search costs nothing on the other 58
frames: no iteration, no branch, no cache line touched. Compare the naive version,
where every weapon is visited every frame to be told "not yet".

**The searching entity is the weapon, not the unit** — see
[`broken_arrow_squads.md`](broken_arrow_squads.md) §4. A multi-turret vehicle, or
a helicopter with a cannon and an ATGM rack, runs one independent search *per
weapon*, each with its own delay component.

**[inferred]** `TargetSearchDelay` has no source column in the `Weapons` database
row and neither does `IsAntiAir` — both are computed in the `WeaponComponent`
constructor, presumably from whether the ammunition's `TargetType` mask includes
`Aircraft`/`Helicopter`. And the constants are named `MAX_*`, which with the
per-unit `RandomComponent` (a pre-seeded 100-entry queue, network-synced by index)
suggests `DelayNextSearch` writes a **randomised fraction** of the cap — so weapons
drift apart in phase rather than locking into a beat. There is no
frame-bucket or index-modulo scheme anywhere in the dump; **the stagger is random
phase, re-rolled every search.**

### 5.1 Why AA runs at 5 Hz

**[inferred] It is a closure-rate argument, not an importance argument.** A jet at
~300 m/s covers 150 m in a 500 ms window and 60 m in 200 ms. The search interval
has to be short enough that a target cannot cross the engagement annulus between
two searches. Ground targets move at metres per second, so 500 ms is free. **The
split is a physical requirement wearing the costume of a tuning constant** — and
it generalises: *sample rate should be derived from the fastest thing you must not
miss, not from how much you care.*

### 5.2 The full cadence ladder

**[DUMP]**, gathered across systems:

| Clock | Rate | Where |
|---|---|---|
| ordinary weapon target search | ≤500 ms | `TargetSearchHelper.MAX_MS_DELAY` |
| anti-air weapon target search | ≤200 ms | `TargetSearchHelper.MAX_AA_MS_DELAY` |
| in-flight missile re-target | 200 ms | `SeekerSystem.TARGET_CHECK_PERIOD` |
| projectile hit-detection tree query | 500 ms | `HitDetectionSystem.TREE_SEARCH_DELAY` |
| FoW scan — air and ground, separately | two periods | `FogOfWarUnit._scanPeriod` / `_scanPeriodGround` |
| FoW scan — anti-projectile SAM | scaled | `FogOfWarSystem.ANTI_PROJECTILE_SAM_SEARCH_PERIOD_MULTIPLIER` |
| unit "in smoke" state | 1 Hz | `UnitInSmokeLocationSystem._everySecond` |
| weapon-flash decay | 1 Hz | `WeaponFlashSystem._everySecond` |
| terrain type under a unit | on a delay | `TerrainTypeComponent.CheckDelay` |
| neighbour list for avoidance | 5 Hz | `DynamicAvoidanceSystem.UNIT_SEARCHBUFFER_MAX_UPDATETIME` |
| AI enemy grouping | 2.5 s | `AiTargetDetectionSystem.GROUPS_UPDATE_INTERVAL` |

**[inferred] Eleven distinct rates, none of them a global tick.** Each is derived
from what the quantity being sampled actually does. That is the opposite of a
frame-budget scheduler, and it means **the frame cost is bounded by construction
rather than by a budget check** — no system has to decide at runtime what to skip.

**[inferred]** The anti-projectile SAM multiplier is the subtle one: **it is applied
to *detection*, not to *targeting*.** A CIWS is useless if it acquires at the same
rate as a rifleman, and the fix was made at the sensor rather than the trigger —
which you would not guess from the targeting constants alone, because the
multiplier lives in the fog-of-war file.

---

## 6. Smoke, buildings, and what shares the grid

**Smoke shares the config row but not the representation.** **[DUMP]**

```csharp
public static class SmokeHelper {
    public static readonly List<SmokeComponent> AllMapSmokes;
    public static float SmokeOpacity { get; }
    public static float SmokeCover   { get; }
    public static bool  IsInsideSmoke(in Vector3 position, out float opacity);
    public static float GetDistanceViaSmokes(in Vector3 observer, in Vector3 target,
                                             out float FOWOpaciedDistance, bool mainThread = True);
    private static ValueTuple<float,float> GetSmokeDistances(in Vector3 observer, in Vector3 target, in SmokeComponent);
}
private struct SmokeHelper.SmokeLine { public float StartLen, EndLen; public readonly float Opacity; }
public class SmokeComponent {
    public readonly Vector3 Position; public readonly float Radius, MaxOpacity, MaxCover, FadeDuration;
    public float SmokeTimeRemaining, FadeInDuration, FadeOutDuration, Opacity, Cover;
}
```

**[inferred]** `TerrainType.Smoke = 15` exists **only so a designer can author
smoke's opacity in the same table as forest**; `SmokeHelper.Init` finds that row
and caches its two floats. But smoke is never written into `MapPixel` — it is a
list of **analytic spheres**, and `GetSmokeDistances` returning a `(float, float)`
is a ray-sphere entry/exit interval.

That is the right call for three reasons: smoke is dynamic and continuously
fading, so rasterising it into a shared array every frame would mean writing to a
structure many threads read; a fade would quantise to pixel resolution; and
intersecting a handful of spheres against one segment is allocation-free and
**thread-safe by construction** — which is why the signature carries
`bool mainThread = True` and the worker calls it with `false`.

Note the split in evaluation rates: **being *inside* smoke is re-checked at 1 Hz**
(`UnitInSmokeLocationSystem._everySecond`), while **seeing *through* smoke is
evaluated per march.** The first is a slowly-changing property of a unit; the
second is a property of a specific sightline.

### 6.1 Trees are destructible, and the occlusion grid does not appear to notice

**[DUMP]**

```csharp
// Namespace: BrokenArrow.Client.Ecs.TreeDestruction
[With(new[] { typeof(TreeDestroyComponent), typeof(MovingComponent), typeof(TransformComponent) })]
public class TreeDestroySystem : EcsSetSystem<float> {
    private const float MIN_REMOVE_DISTANCE = 12;
    private readonly TreeDestructionService _treeDestructionService;
}

public interface ITreeDestruction {
    …
    BoundingSphere[] GetRemovedBounds();
}

public class TreeDestructionService : IService, IInitializable, IDisposable {
    public UniTask     VehicleRemoveTreeInCollider(Collider collider);
    public UniTaskVoid VehicleRemoveTreeInBounds(Bounds bounds, Vector3 position);
    public UniTask     RemoveTreeInRadius(BoundingSphere boundingSphere);
    public UniTask     RemoveTreeInRadiusSilent(BoundingSphere boundingSphere);
    public BoundingSphere[] GetRemovedBounds();
}

public struct UpdateDelay {
    public float MaxDelay; public int MaxFrame;
    public bool TimeDelay(float elaspedTime);
    public bool FrameDelay();
}
```

**[inferred]** Trees come down two ways: **moving units flatten them**
(`[With(TreeDestroyComponent, MovingComponent)]` with a 12 m radius), and
**explosions clear them** (`RemoveTreeInRadius`). The `Silent` variant is
presumably for late-join or replay catch-up, so a joining client does not get a
hundred tree-fall effects at once. `UpdateDelay` carrying **both** a time budget
and a frame budget is a nice touch — removal is amortised on whichever runs out
first.

**The interesting part is `GetRemovedBounds()` returning `BoundingSphere[]` — which
is exactly the representation smoke uses**, and smoke *is* consulted by the vision
march (`GetDistanceViaSmokes`, `mainThread = false`). So the data structure that
would allow dynamic forest occlusion exists.

**[inferred, and stated carefully] It does not appear to be wired to vision.** No
fog-of-war or targeting type holds an `ITreeDestruction` or `TreeDestructionService`
reference — the only field of either type in the dump is on `TreeDestroySystem`
itself. **The caveat matters:** `TreeDestructionService : IService` sits in a
service-locator pattern, so a consumer could resolve it on demand inside a method
body the dump cannot show. What can be said firmly is that **nothing in the
visibility path holds it as a dependency**, where `FogOfWarUnit` holds its filter
and raycast service explicitly.

If that reading is right, the gameplay consequence is checkable in ten seconds
in-game: **drive a tank through a wood and the trees fall, but the sightline is
occluded exactly as before**, because `MapPixel.TerrainData` was baked at load and
still says `Forest`.

**[inferred] And there is a good reason it would be built this way.** §3.2's whole
threading contract is that the worker reads `MapMetaData` *because it is immutable
after load*. Writing tree removals back into the pixel grid would break the one
invariant that lets the fog-of-war filter run lock-free across teams. Smoke dodged
this by staying analytic; trees could have used the identical trick, and the
`BoundingSphere[]` suggests somebody considered it. **A dynamic occluder is cheap
to add to an analytic list and ruinous to add to a shared baked grid** — which is
the general lesson, and it is why the smoke design is the one to copy.

**Buildings live in both worlds.** **[DUMP]** `TerrainType.Buildings = 3` and
`NavMeshMetaLayers.Buildings = 32` put them in the grid, while
`BuildingSegmentComponent` carries `public int ColliderID;` — and
`FogOfWarTargetPoint` carries `TargetColliderID`. **[inferred] That is the join:**
the grid says "a building blocks this", and only the collider can say "*that*
segment, at *that* point, with 40% `FakeHealth` left".

`MapMetaData.GetInterpolatedTerrainHeight(..., bool includeBuildings, bool includeBridges, ...)`
**[inferred]** lets the same pixel array answer "ground height" and
"ground-plus-structures height" depending on who asks — a tank wants the former, a
helicopter and a sightline want the latter.

A nice detail: building doors are **found by scanning the grid**, not authored.
`BuildingSegmentComponent.CheckForBuilding(in MapPixel, int, int)` matches
`CheckPixelDelegate` exactly and feeds `CalculateDoors(..., MapMetaData mapMeta)`.
The same predicate is deduplicated across three call sites — aircraft waypointing,
the LOS visual tool, and building init — so **"is this pixel a building?" is the
most-reused predicate in the codebase.**

Finally, `FogOfWarFilter` is not really a fog-of-war component. **[DUMP]**
`SeekerSystem` holds one too, alongside the same `IRaycastBatchService`, the same
`Stack<FilterTask>`, the same in-flight guards and a 200 ms check period.
**[inferred] It is the engine's general terrain-LOS service, and fog of war is one
client.**

---

## 7. Radar is a dial, not a channel

**[DUMP]**

```csharp
public struct RadarAbilityComponent {
    public readonly float LowAltOpticsModifier;       // 0xC
    public readonly float HighAltOpticsModifier;      // 0x10
    public readonly float LowAltWeaponRangeModifier;  // 0x14
    public readonly float HighAltWeaponRangeModifier; // 0x18
    public readonly float RadarSwitchCooldown;        // 0x1C
    public readonly bool  IsStatic;                   // 0x20
    public GFloat RadarSwitchCooldownTimer;
}
[With(new[] { typeof(RadarAbilityComponent) })]
public class RadarSystem : EcsSetSystem<float> {
    public static HashSet<Entity> ActiveRadars { get; set; }
}
// Weapons DB
public bool IsRadarDependent { get; set; }
```

**[inferred] Radar is not a second detection channel with its own range and
cadence. It is a band-selective multiplier on the single fog-of-war channel** —
the same `DistanceLowAlt` / `DistanceHighAlt` that optics feed. `ActiveRadars` as a
static `HashSet<Entity>` is the global registry the FoW and missile code consult.

`IsRadarDependent` is then a **second, independent gate applied after detection**:
a weapon may not engage a target it can see unless a radar is lit. It gets its own
removal reasons rather than folding into `TargetNotVisible`:

```
RadarWeaponWithoutRadar = 15,  ProjectileWithoutRadar = 16,
MissileTargetRadarDisabled = 29,  MissileTargetHasNoRadar = 30
```

**[inferred]** That last pair is the tactical point of the whole radar toggle: a
SEAD missile in flight loses lock the instant the emitter shuts down. And because
radar also carries `LowAltWeaponRangeModifier` / `HighAltWeaponRangeModifier`,
**switching your radar on physically extends your SAM's reach as well as its
sight** — so going dark is a real range sacrifice, not merely a stealth trade.

### 7.1 A dead enum worth noting

**[DUMP]**

```csharp
public enum SensorType {
    Visual = 5, InfraRed = 10, TV = 20, NightVision = 30,
    RadarA = 100, RadarG = 110, RadarU = 120, Laser = 300, Beam = 310
}
```

**`SensorType` appears nowhere else in the binary.** A grep returns only its own
declaration lines — no field, no property, no parameter, no dictionary key, and
the `Sensors` database row has no type column, only the three optics floats.

**[inferred] It is a vestigial schema from a design that intended per-sensor
detection channels — IR versus radar versus TV, with the `RadarA`/`RadarG`/`RadarU`
split presumably Airborne/Ground/Universal — and what shipped collapsed all of it
into three scalar ranges keyed by target altitude band.** (Caveat: the dump emits
fields and signatures, not locals, so a use confined to a method body would be
invisible — but with no storage anywhere holding a `SensorType`, there is nothing
for such a local to read.)

Compare `SeekerType`, which is very much alive — `Ammunitions.Seeker`,
`SeekerComponent.Type`, `MissilesHelper.DoesSeekerRequireShooterGuidance(SeekerType)`,
and a localisation dictionary. **[inferred] The lesson is the ordinary one: an enum
in a shipped binary proves an intention, not a feature.**

---

## 8. How a helicopter picks between air and ground targets

**It doesn't, because nothing does.**

**[DUMP]**

```csharp
private const UnitType GROUND_UNIT_TYPES = 38;   // Infantry(2) | Vehicle(4) | Ship(32)

public class UnitsPositionsTree {
    private const float MIN_NODE_SIZE = 50;
    private Dictionary<UnitType, KdSearchTree<Entity>> _searchTreesDict;
    public ReadOnlySpan<ValueTuple<Entity,float>> GetNearbyUnits(UnitType type, GalaxyVector3 fromPosition,
                                                                 float maxDistance, bool sphereSearch = True);
}
```

**`GROUND_UNIT_TYPES = 38` excludes `Helicopter = 8`.**

**[inferred] The KD-tree partition *is* the target-class filter.** The ammunition's
`TargetType` mask does not filter candidates after a query — it selects **which
trees to walk**. An infantry rifle never touches the aircraft tree at all. That is
CLAUDE.md's "do less work" rule at its most literal: not a cheaper test, but no
test.

So a Hind's cannon and its ATGM rack are two separate entities in
`TargetSearchSystem`, each with its own delay component, each producing its own
`TargetComponent`, each walking whichever trees its ammunition can engage. An
AA-capable weapon on the same airframe simply has `IsAntiAir` set and ticks at 5 Hz
while its stablemate ticks at 2 Hz. **There is no unit-level arbitration to
write.**

The only unit-level structure is a pre-bucketed index so the aircraft systems do
not re-scan:

```csharp
public struct UnitBattleInfoSetComponent {
    private Memory<WeaponsByAmmoId> _strafeWeapons, _directAttackAirGroundWeapons,
                                    _precisionWeapons, _lowDragBombWeapons, _highDragBombWeapons;
    public Span<WeaponsByAmmoId> DirectFireAirWeapons { get; }
}
```

And engagement radii are authored **per target class**, three numbers per unit,
replicated as three `ushort`s:

```csharp
public struct AggressiveRadiusData { public int Grounds, Helicopters, Planes; public int GetMaxValue(); }
internal struct AggressiveRadiusComponent {
    public AggressiveRadiusData BaseRadius, DynamicRadius;
    public int GetRadius(UnitType type);
}
```

**[inferred]** So "how far will this unit reach out to engage" is three separate
authored answers — a SAM can be told to guard a wide air envelope and a narrow
ground one from the same component.

The plane-versus-helicopter case does get bespoke *manoeuvre* code, and note what
the signatures do and do not carry:

```csharp
private void VersusGroundTarget(Entity planeEntity, Entity targetEntity, float minRange, float maxRange);
private void VersusPlane       (Entity planeEntity, Entity targetEntity, float weaponsRange);
private void VersusHelicopter  (Entity planeEntity, Entity targetEntity);          // ← no range at all
private const float HELI_ATTACK_FLY_ANGLE = 45;
private readonly float _heliAttackFlyDot;    // cached cos(45°)
```

with the only two shipped tunables:

```csharp
[Range(5, 30)]
[Tooltip("Angle to target helicopter after which plane will start to dive for attacking (Plane will try to maintain current altitude before that)")]
public float PlaneVsHeliDiveAngle;
[Range(0, 50)]
[Tooltip("Extra height added to low altitude when checking if its time to nose-up to prevent ground-crashing")]
public float PlaneVsHeliNoseUpExtraHeight;
```

**[inferred] `VersusHelicopter` taking no range is the tell** — attacking a
helicopter is pure geometry, because the helicopter is low enough that the problem
is *diving at it without hitting the ground*, not reaching it. Hence a dive-angle
gate and a nose-up safety margin, and nothing about weapons.

`_heliAttackFlyDot` caching `cos(45°)` in the constructor is the right instinct
and appears throughout this codebase: **precompute the dot, never call `acos` in a
per-frame path.**

---

## 9. What transfers

1. **Make the cheap query's result a decomposition, not a verdict.**
   `int[] CountOfTerrainTypes` costs one array per trace and lets three unrelated
   rules — visibility, shootability, laser range — be answered later from stored
   data with no second trace. **This is the highest-leverage idea in the note.**

2. **Cull with the grid, confirm with colliders. Never the reverse.** Two
   independent subsystems here (fog of war, helicopter navigation) arrived at the
   same layering. The O(N×M) work must never touch a collider.

3. **Make cadence a set membership, not a timer.** `[Without(DelayComponent)]`
   means a throttled entity is not visited at all, where a timer poll still costs a
   branch per entity per frame. **This is the single most transferable ECS trick
   here.**

4. **Derive sample rate from the fastest thing you must not miss.** AA at 5 Hz is a
   closure-rate calculation, not a priority judgement. Eleven distinct rates in
   this game, each derived from its own quantity, and no global scheduler deciding
   what to skip — the frame cost is bounded by construction.

5. **Batch every physics query in the frame into one job.** Individually-called
   raycasts cannot be jobified; a shared command buffer with `(start, count,
   callback)` slices converts N blocking queries into one parallel one.

6. **Separate the authored type from the hot-loop type.** `VisibleTerrainType`
   (managed, tooltips, Inspector) versus `TerrainVisionData` (16-byte readonly
   struct indexed by a byte). Authoring ergonomics and cache layout are different
   problems.

7. **Make the occlusion march height-aware and get three unit domains for free.**
   One per-terrain-type canopy height turned what would have been separate ground,
   rotary and fixed-wing vision code into four authored numbers.

8. **Keep dynamic occluders analytic.** Smoke shares the designer-facing cost table
   but stays a list of spheres — smooth to fade, cheap to intersect, and
   thread-safe without touching shared state.

9. **Double-debounce a quantised state that gates other systems.** Value hysteresis
   (0.3/0.4) *and* time hysteresis on the altitude band, because it feeds range
   selection and flicker there is visible as an engagement envelope flickering.

10. **Give a failure its own reason code.** `RadarWeaponWithoutRadar` distinct from
    `TargetNotVisible`, and `MissileTargetRadarDisabled` distinct from
    `MissileTargetHasNoRadar`. The distinctions are what make "why didn't my AA
    fire" answerable.

11. **Let the spatial index carry the type filter.** A `Dictionary<UnitType,
    KdSearchTree>` means a mask selects which trees to walk rather than filtering
    results — no work rather than cheaper work.

---

## 10. What is not established

- **No method bodies.** The march's exact accumulation, the height comparison, the
  delay randomisation and the range selection are all inference from field sets and
  signatures. The ECS filters and `const` values are literal and carry most of the
  weight.
- **No authored values.** `FogOfWarUnit._scanPeriod` / `_scanPeriodGround`,
  `ANTI_PROJECTILE_SAM_SEARCH_PERIOD_MULTIPLIER`, `PlanesConfig.HighAltitude` /
  `LowAltitude`, every `Sensors` and `Ammunitions` range, and the whole terrain
  cost table live in assets and `.cctor` bodies. **Only the `const`s carry literal
  numbers**, which is why §4.3's argument leans on `FOREST_HEIGHT = 25` and the two
  helicopter altitudes — those three happen to be `const`.
- **The stagger mechanism is inference.** That `DelayNextSearch` randomises within
  the cap follows from the `MAX_*` naming plus `RandomComponent`, but was not read.
- **No system declares `[With(TargetSearchDelayComponent)]`**, so the decrement site
  is not attributable from filters alone.
- **The `SensorType` negative claim** is bounded by what a dump can show: no
  storage of that type exists anywhere, but a use confined entirely to a method
  body would be invisible.
- **Nothing was run or profiled.** No claim about actual cost, thread utilisation,
  or how many observer/target pairs the system sustains.

---

## Sources

**[DUMP]** Il2CppDumper v6.7.46 over
`C:\Program Files (x86)\Steam\steamapps\common\broken_arrow\GameAssembly.dll` and
`BrokenArrow_Data\il2cpp_data\Metadata\global-metadata.dat` (IL2CPP metadata v31,
dated 2025-08-06).

Read against
[`spatial_queries.md`](../../../topics/agents/spatial_queries.md), whose stated
CryEngine rule — cheap filters, then score, then the raycast, and stop at the
first pass — this targeting stack implements almost line for line.
