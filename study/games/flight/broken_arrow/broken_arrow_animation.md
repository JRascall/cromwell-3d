# Broken Arrow — animation: a verb set, not a clip library

How does a game with hundreds of units on screen animate them, given the
simulation is a C# ECS and the visuals are Unity GameObjects? The answer is two
completely different systems joined by a **fifteen-property interface**, and
neither of them is what a modern crowd-rendering paper would suggest.

> **There is no GPU animation of any kind.** No vertex animation textures, no bone
> animation textures, no compute skinning, no indirect instanced characters. Six
> independent negative searches confirm it. Infantry use a stock CPU `Animator`;
> **everything else in the game — every wheel, track, rotor, wing, turret, aerial
> and suspension arm — is C# writing `Transform`s and material properties.**
>
> **Twenty-five behaviours cover every unit type.** There is no tank behaviour, no
> helicopter behaviour, and no rotor behaviour. A rotor is `Torque`. Landing gear
> is `RotateBySpeed`. It is a **generic verb set recombined per prefab**, authored
> in the Inspector as `[SerializeReference] IAnimationBehaviour[]`.
>
> **The simulation→visual channel is four floats.** `CurrentSpeed`, `MaxSpeed`,
> `CurrentAltitude`, `TerrainType`, plus an LOD tier and a state enum. Everything
> continuous is a pure function of those. Discrete events ride a separate channel
> of twenty C# events.
>
> **Physics reports for the living and simulates only for the dead.** Live units
> carry a `Rigidbody` purely so `CollisionCatcher` can forward `OnCollision*` into
> the ECS, which computes separation itself. Mass and drag are defined only for
> corpses.

> **Source.** **[DUMP]** from the retail Il2CppDumper output; see
> [`broken_arrow_aircraft.md`](broken_arrow_aircraft.md) for the full method note.
> Method bodies are native and absent, so control flow is **[inferred]**; type
> names, fields, offsets, signatures, `const` values and Unity attributes are
> literal. A sample of the load-bearing declarations was re-read directly against
> the dump.

Related: [`broken_arrow_vehicles.md`](broken_arrow_vehicles.md) (the
`BoneContainer` locomotion rig this note's §5 sits on),
[`broken_arrow_squads.md`](broken_arrow_squads.md) §8 (infantry animation and the
lockstep fix),
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) (the same
question across four builds of this genre, including Eugen's operator lists),
[`lod_systems.md`](../../../topics/world/lod_systems.md) §5–6 (the VAT/BAT
arithmetic and the three cheaper levers — §7 below is a direct answer to it).

---

## 1. The negative result, stated properly

**[DUMP]** These searches return **zero** hits across all 1,757,307 lines:

`VertexAnim`, `AnimationTexture`, `BakedAnimation`, `AnimationBaker`, `VatBake`,
`_VAT`, `GpuSkin`, `SkinningCompute`, `WheelCollider`, `ArticulationBody`,
`RigBuilder`, `TwoBoneIK`.

`DrawMeshInstanced*` / `RenderMeshIndirect` / `DrawProceduralIndirect` appear only
as HDRP's own `s_ClearDrawProceduralIndirectKernel`. Every `GraphicsBuffer` in the
binary belongs to HDRP. Every `ComputeShader` is either HDRP's or one of five
game-owned ones, none of them animation:

```csharp
private ComputeShader _elevationShader;   // ElevationFeedback
private ComputeShader _fowShader;         // FowFeedback   (a debug visualiser)
private ComputeShader _computeShader;     // BloodBurn     (decals)
public  ComputeShader Shader;             // MiniMapConfig
public  ComputeShader BlueShader;         // FOWDebug
```

`SkinnedMeshRenderer` appears in exactly **two** game types:

```csharp
public SkinnedMeshRenderer[] SkinnedMeshRenderers;  // BrokenArrow.Lods.BaLod — a toggle list
private SkinnedMeshRenderer _mesh;                  // ParachuteBehaviour
```

No `PlayableGraph` is constructed by game code anywhere. The only constraint type
used is the built-in `UnityEngine.Animations.LookAtConstraint`.

**[inferred] So skinning is 100% stock Unity, on the CPU, and only infantry are
skinned at all.** Vehicles and aircraft are rigid-body hierarchies with bones
moved by C#.

### 1.1 `Bake()` bakes bones, not vertices

**[DUMP]**

```csharp
public class AnimationManager : MonoBehaviour {
    [SerializeField] private BoneContainer _container;
    public void Bake() { }
    public void SetAnimationData(AnimationManagerBridge animationManagerBridge) { }
}
public class BoneContainer { public void Bake(Transform mainObject) { } }

public void Suspension..ctor(Transform[] child);
public void Tracks    ..ctor(Transform[] child);
public void Wheels    ..ctor(Transform[] child);
```

each of those constructors carrying exactly two cached name predicates
(`Func<Transform,bool>`), and `Wheels` holding
`[SerializeField][HideInInspector] private bool _init;`.

**[inferred] `Bake()` is an editor-time hierarchy cache.** One
`GetComponentsInChildren<Transform>()` on the prefab, partitioned by name
predicate into serialized `Transform[]` fields, with `_init` as the
already-baked marker. No vertex data is involved and runtime never searches the
hierarchy. **A name that sounds like the expensive thing doing the cheap thing is
worth flagging** — it is exactly the identifier trap
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) §11
warns about.

---

## 2. The ECS↔visual boundary

**[DUMP]** Two systems and two components make up the entire animation namespace:

```csharp
[With(new[] { typeof(AnimationHubComponent) })]
[Without(new[] { typeof(DeadComponent), typeof(AirdropComponent) })]
public class AnimationHubSystem : AEntitySetSystem<float> { }

[With(new[] { typeof(TerrainTypeComponent) })]
[Without(new[] { typeof(AirdropComponent) })]
[With(new[] { typeof(LODGroupComponent) })]
[With(new[] { typeof(SpeedComponent) })]
[With(new[] { typeof(UnitAnimationComponent) })]
public class UnitAnimationSystem : AEntitySetSystem<float> {
    private static readonly float REACTION_SPEED;
    private static void ShakeAnimation(ref TerrainTypeComponent, ref UnitAnimationComponent);
}

public struct AnimationHubComponent { private AnimationHub <Hub>k__BackingField; }

public struct UnitAnimationComponent {
    public float LerpTurn;
    private bool _enable;
    private readonly AnimationManager _animationManager;
    // Speed { set; }   Turn { set; }   ShakeEffect { set; }   ← write-only
    public void HandleShot(Vector3 direction, Transform shellSpawn);
    public void Death();
    public void Update(float delta);
}
```

**[inferred] `Speed`, `Turn` and `ShakeEffect` being write-only properties is the
contract in miniature: the simulation pushes, the visuals never pull back.**

### 2.1 The continuous channel is four floats

**[DUMP]** `IAnimationHubData` — what every behaviour is handed — is fifteen
read-only properties and no methods:

```csharp
public interface IAnimationHubData {
    GameConfig      GameConfig      { get; }
    string          GameObjectName  { get; }
    float           CurrentSpeed    { get; }
    float           MaxSpeed        { get; }
    float           CurrentAltitude { get; }
    LODGroupEnum    CurrentLod      { get; }
    Transform       HubTransform    { get; }
    AnimatorHubState State          { get; }
    TerrainType     TerrainType     { get; }
    float           DeathDelay      { get; set; }
    int             UnitId          { get; }
    bool            IsUnitLoaded    { get; }
    bool            IsUnitAirdrop   { get; }
    bool            HasHangarAnimations { get; }
    SettingQuality  EffectsQuality  { get; }
}
```

**[inferred] Four values do the real work — `CurrentSpeed`, `MaxSpeed`,
`CurrentAltitude`, `TerrainType` — plus an LOD tier and a state enum.** Every
continuously animated thing in the game is a pure function of those. The
asymmetry is deliberate: on `AnimationHub` itself these are `{ get; set; }`, on
the interface handed to behaviours they are `{ get; }` only.

That is a very small surface for a very large amount of motion, and it is what
makes the whole thing testable: an artist can scrub four sliders and see the
entire rig respond.

### 2.2 The discrete channel is twenty events

**[DUMP]**

```
Action<TerrainType> ExitTerrainTypeEvent / EnterTerrainEvent
Action<Vector3, Transform> ShotEvent
Action UnitLoadedEvent / UnitAirdropEvent
Action LoadStartEvent / LoadEndEvent / UnloadStartEvent / UnloadEndEvent
Action<string> EntranceOpenEvent / EntranceCloseEvent
Action<bool> UnitHideEvent / AfterburnerEvent / RadarSwitchEvent / StaticPositionEvent
Action ReloadEvent / UnitStartAimEvent / UnitEndAimEvent
Action<Transform> WeaponStartAimingEvent / WeaponEndAimingEvent
                  WeaponStartReloadingEvent / WeaponEndReloadingEvent
```

with senders including `SendShootEvent(Vector3, Transform)`,
`SendAfterBurnerEvent(bool)`, `SendRadarSwitchEvent(bool)`, and two `static`
doorways from the ECS: `SetUnitLoaded(in Entity, bool)` and
`SetUnitAirdrop(in Entity, bool)`.

**[inferred] Splitting continuous state from discrete events is the single
cleanest decision here.** Polling "did I fire this frame" out of a property bag
would be a boolean somebody has to clear; an event fires once and carries its
payload (*which* barrel fired, in *which* direction). Conversely, eventing "my
speed changed" would be a flood. Each channel carries what it is good at.

### 2.3 Five state arrays, polymorphically serialized

**[DUMP]**

```csharp
[SerializeReference] private IAnimationBehaviour[] _universalState;
[SerializeReference] private IAnimationBehaviour[] _demoState;
[SerializeReference] private IAnimationBehaviour[] _gameState;
[SerializeReference] private IAnimationBehaviour[] _preDeathState;
[SerializeReference] private IAnimationBehaviour[] _deathState;

public enum AnimatorHubState { None = -1, Demo = 0, Game = 1, Death = 2, PreDeath = 3 }

public interface IAnimationBehaviour {
    SettingQuality StartQuality { get; }
    void OnInit(IAnimationHubData animationHubData);
    void OnEnable();  void OnDisable();
    void OnStart(IAnimationHubData animationHubData);
    void OnUpdate(float timeDelta);
    void OnDestroy(); void OnValidate();
    void Merge(IAnimationBehaviour internalBehaviour);
    event Action CompletedEvent;
}
```

**`[SerializeReference]` rather than `[SerializeField]` is the load-bearing
detail.** It is Unity's polymorphic-serialization attribute, and it is the only
thing that makes a designer-authored array of *mixed concrete types* editable in
the Inspector at all. The entire architecture rests on it.

**[inferred]** `CompletedEvent` feeding `_deathBehavioursCompletedCount` and
`OnBehaviourCompleted()` makes the death array a **completion-counted sequence** —
the hub tears the unit down once every death behaviour has signalled, so a
long explosion and a short mesh-swap coexist without a hardcoded delay.

`Merge` plus `MergeInternalAnimationHub()` and `_wasMergedInternalHub` exist
because turrets and weapons are **separate prefabs with their own hubs**, folded
into the parent's arrays at spawn — which is what makes the loadout-swappable
turrets of [`broken_arrow_vehicles.md`](broken_arrow_vehicles.md) possible.

---

## 3. The verb set

**[DUMP]** Twenty-five concrete behaviours plus one abstract base, all
`[Serializable]`, almost all in `BrokenArrow.Client.Ecs.AnimationBehaviors`.
Selected, with the fields that say what they do:

**`RotateBySpeed`** — the workhorse. Lerps a bone between two poses by speed ratio.
```csharp
[SerializeField][Tooltip("If current unit lod higher effect will not be updated")]
private LODGroupEnum _lodGroup;
[SerializeField] private Transform _target;
[SerializeField] private Vector3 _IdlePosition;   // note the capitals — hand-written late
[SerializeField] private Vector3 _FinalPosition;
private float _currentSpeed, _maxSpeed;
```

**`Torque`** — constant-rate spin about an arbitrary axis.
```csharp
[SerializeField] private Transform _target;
[SerializeField] private Vector3   _direction;
[SerializeField] private float     _speed;
```
**[inferred] This is the rotor, the propeller, the radar dish and the cooling fan.**
`rotor`, `propeller` and `blade` return zero game hits — a helicopter rotor is
`Torque` with `_direction = (0,1,0)`.

**`MathConnect`** — a bone chasing a target through a spring-damper, plus per-shot
impulses.
```csharp
private const float DELAY_TO_ACTIVATE = 0.5;
[SerializeField] private ObjectFollower _settings;
[SerializeField] private Transform _root, _target;
[SerializeField] private bool _freezeXPos, _freezeYPos, _freezeZPos;
[SerializeField] private MathConnect.WeaponShotForces _shotForces;  // Dictionary<Transform,float>
private Vector3 _shotVelocity, _calmPointLocal, _calmPoint;
```

**`AxisRandom`** / **`PositionRandom`** — retarget a bone to a random angle or
offset every `[minTime, maxTime]`; the idle turret sweep, radar scan and gunner
head-look.
```csharp
public enum AnimMethod { Lerp = 0, WildLerp = 1, Slerp = 2, Towards = 3 }
```

**`FloatEffect`** — six-DOF sinusoidal drift with a **per-instance `_timeOffset`**,
so a formation does not bob in lockstep. **`WaterFloatEffect`** is the same idea
gated on `TerrainType` becoming water.

**`Afterburner`** — wing/nozzle rotation *and* an effects burst, driven by
`AfterburnerEvent(bool)`:
```csharp
[SerializeField] private Transform _wingLeft, _wingRight;
[SerializeField] private float _maxAngle, _wingsDelay;
[SerializeField][Tooltip("Duration in seconds to get max angle")] private float _duration;
[SerializeField] private SpawnVFX.VfxData[] _activeVfx;
[SerializeField] private string _sfx;
```

**`AircraftTrails`** — wingtip vortices past `_angleToStartTrail`, caching
`ValueTuple<Transform,Vector3,Quaternion>[]` local TRS so the hierarchy is
resolved once.

**`HelicopterDustVFX` : `UnitBaseTerrainVFX`** — rotor downwash:
```csharp
private const float LIFE_TIME = 3;
[SerializeField] private float _maxAltitude;
private const string VFX_POWER_PARAM_NAME = "Power";
```
**[inferred]** downwash strength = f(`CurrentAltitude` / `_maxAltitude`), written
into a VFX Graph float named `"Power"`. This is the one place `CurrentAltitude`
visibly earns its slot in the interface.

**`ProceduralDirt`** — vehicles accumulate grime:
```csharp
private const string DIRT = "_DIRT";
private const float DIRT_SPEED  = 0.01;
private const float CLEAN_SPEED = 0.1;
```
**[inferred]** Dirtying is **ten times slower than cleaning**, which only makes
sense as "gets dirty slowly, washes off fast" — driving through water.

**`ShellCasingDrop`** — and this one is a small essay in itself:
```csharp
[SerializeField] private float _mass, _linearForce, _linearDrag,
                               _angularForce, _maxAngularVelocity, _angularDrag;
[SerializeField][Tooltip("How much each axis can be randomized")] private Vector3 _randomFactor;
[SerializeField][Tooltip("In seconds")] private float _lifeTime;
[SerializeField][Tooltip("Delay before drop in seconds")] private float _dropDelay;
[SerializeField] private bool  _showDropDirection;   // editor gizmo
[SerializeField] private float _endPointSize;        // editor gizmo
```
**Mass, linear force, drag, angular force, max angular velocity, angular drag —
the entire vocabulary of a `Rigidbody`, reimplemented as serialized floats.** They
deliberately did not use PhysX for shell casings. See §6.

**`TurretFly`** — the turret-toss, with `[Range(0,1)] public float Chance;` and
`public bool RemoveTurretIfNotThrow;` — **the turret pops off probabilistically**,
with the other branch handled explicitly.

**`AudioPlayer`** — an FMOD one-shot bound into the same array:
```csharp
[SerializeField] private string _eventPath;
[SerializeField] private bool _isShotPlay, _isTimerPlay;
```
**[inferred] Sound is a behaviour in the animation list, not a separate system**,
so an artist wiring an animation wires its sound in the same Inspector array.
`DecalProjection`, `ShowVFX`, `SpawnVFX` and `SetVFXProperty` are there for the
same reason. **The "animation array" is really an effects array.**

**`AxisRepeater`** is the only `[Obsolete]` behaviour — a straight axis copy,
superseded by `MathConnect` doing the same job with damping. **[inferred]** Left in
because prefabs still reference it.

**`SpawnCorrector`** — an escape hatch applying a rotation/offset fixup to named
bones at spawn, so a mis-authored FBX can be corrected in-scene without a
re-export.

**[inferred] The roster is the finding.** Twenty-five generic verbs — spin,
lerp-by-speed, spring-follow, wander, drift, swap mesh, spawn VFX, play sound —
cover every vehicle, aircraft, helicopter, ship and building in the game. **A tank
is not a type; it is a list.** That is the same conclusion
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) reaches
about Eugen's `DepictionOperator` lists, and §8 of that note already argues the
convergence is meaningful. This is a third data point on a different engine.

### 3.1 `ObjectFollower`, the one piece of real math

**[DUMP]**

```csharp
public class ObjectFollower {
    public const float FREQUENCY_DEFAULT = 2.04;
    public const float DAMPER_DEFAULT    = 0.21;
    public const float REACTION_DEFAULT  = -10.1;
    [SerializeField] private float _frequency, _damper, _reaction;
    private Vector3 _xp, _result, _yd;
    private float _w, _z, _d, _k1, _k2, _k3;
    public Vector3 Update(float deltaTime, in Vector3 pos, Nullable<Vector3> velocity);
    private bool IsNaN(Vector3 vector);
    private bool IsInfinity(Vector3 vector);
}
```

**[inferred]** `frequency / damper / reaction → k1, k2, k3` with the intermediate
`w, z, d` is the standard **second-order dynamics** formulation. The response
constant being **negative** (`-10.1`) is the interesting part: it makes the
follower **anticipate** — the bone leads the motion before following it, rather
than lagging. That is what gives a gun barrel or an aerial the whip that reads as
weight.

The `IsNaN` / `IsInfinity` guards say the integrator blew up on them at some
point, which it will: a stiff second-order system with a variable `deltaTime` is
exactly the thing that explodes on a frame hitch.

---

## 4. Infantry: the only real `Animator`

**[DUMP]** `RuntimeAnimatorController` appears **three times in the entire binary**:

```csharp
public RuntimeAnimatorController HangarAnimatorController;         // SoldierAnimationManager
public RuntimeAnimatorController DefaultHangarAnimatorController;  // InfantryConfig
public RuntimeAnimatorController DefaultBattleAnimatorController;  // InfantryConfig
```

**All three are infantry.** `SoldierAnimationManager` carries
`[RequireComponent(typeof(Animator))]`; nothing on the vehicle path does.

The details are in [`broken_arrow_squads.md`](broken_arrow_squads.md) §8 — the
hand-rolled random clip picker, the `TrySet*` priority list, the four
lockstep-breaking animator parameters, and `FireEnd()`/`ReloadEnd()` as
`AnimationEvent` targets so **the reload completes when the clip says it does**.

Worth adding here: `SoldierAnimatorComponent.SaveLastAnimatorState(bool reset)` is
`async` (`UniTaskVoid`). **[inferred]** Re-enabling an `Animator` cannot restore
its pose in the same frame — Unity needs an evaluation first — so restoration is
deferred a frame. That is a real, non-obvious cost of the disable-wholesale LOD
strategy, and they paid it explicitly.

---

## 4a. Hands, weapons and seats: attachment, not solving

The obvious question about a game with hundreds of soldiers is how IK scales. The
answer is that **it doesn't need to, because there is essentially none.**

### 4a.1 The entire IK system is one constraint

**[DUMP]** A search across all 1,757,307 lines for `IKSolver`, `TwoBoneIK`,
`Fabrik`, `CCDIK`, `RigBuilder`, `MultiAimConstraint`, `MultiParentConstraint`,
`ParentConstraint`, `AimConstraint` and `LookAtConstraint` returns **exactly one
game-code hit**:

```csharp
public struct SoldierAnimatorComponent {
    public readonly Animator          Animator;    // 0x0
    public readonly LookAtConstraint  WeaponAimer; // 0x8   ← the whole IK system
    …
}
```

Everything else the search returns is `UnityEngine.Animations`' own module
declarations, which ship in the binary whether a game uses them or not. There is
no Animation Rigging package, no foot IK, no hand IK, no full-body solver, and no
per-soldier pose correction of any kind.

**[inferred] One `LookAtConstraint` per soldier, aiming the weapon at the shared
target — and that is the complete list.** It scales because a `LookAtConstraint` is
a single bone rotation evaluated by Unity's native constraint system, not a solve;
and because past a LOD tier the animators are switched off wholesale
([`broken_arrow_squads.md`](broken_arrow_squads.md) §8), taking the constraints
with them.

### 4a.2 The weapon aligns to the hand, not the hand to the weapon

**[DUMP]** Two sockets on the soldier, tooltips verbatim:

```csharp
[RequireComponent(typeof(Animator))]
public class SoldierAnimationManager : MonoBehaviour {
    [Tooltip("Weapon position on the back")]      public Transform Spine;       // 0x20
    [Tooltip("Position of the weapon in hand")]   public Transform WeaponPlace; // 0x28
    public Animator Animator;
    public void FireEnd();     // AnimationEvent target
    public void ReloadEnd();   // AnimationEvent target
}
```

and two matching alignment transforms **on the weapon**:

```csharp
public class SoldierWeapon {
    public GameObject WeaponGameObject;
    public Transform  WeaponTransform;
    public Transform  FirePosition;          // 0x30  ← muzzle
    public Transform  WeaponHandPosition;    // 0x38
    public Transform  WeaponBackPosition;    // 0x40
    private readonly Quaternion _zeroRotation; // 0x48
    public void SetWeaponToBack();
    public void SetWeaponToHand();
}
```

**[inferred] This is the whole mechanism, and the direction matters.** The *weapon*
carries the alignment transforms; the *soldier* carries the sockets.
`SetWeaponToHand()` reparents the weapon so its `WeaponHandPosition` coincides
with the soldier's `WeaponPlace`, applying `_zeroRotation`. **The hand does not
move to the weapon — the weapon moves to the hand.**

Three consequences follow, and they are the reason this works at scale:

1. **The grip is baked into the animation clip.** The animator posed the fingers
   once, in the clip, and every rifle in the game must be modelled with a
   consistent grip origin so that one pose fits all of them. The cost per soldier
   per frame is **zero** — it is a parent-child relationship, not a computation.
2. **Weapon swapping is a teleport.** `ChangeWeaponInHand(ref SoldierComponent, bool specialWeapon)`
   moves the mesh between the spine and hand sockets with no transition, masked by
   whatever clip is playing. At RTS camera distance it is invisible.
3. **A `_zeroRotation` field exists** because the artist's weapon prefab will not
   have been authored with an identity orientation, and one cached quaternion is
   cheaper than asking every artist to re-export.

**[inferred] The general rule: at strategic camera distances, solve the attachment
problem in the content pipeline rather than at runtime.** IK exists to reconcile a
pose with a world position that was not known when the clip was authored. If you
can arrange for the world position to be *known* — by making the weapon come to a
fixed socket — the reconciliation disappears.

### 4a.2a Two-handed weapons: the off hand is not attached to anything

A rifle needs two hands and there is exactly one socket, so the obvious question is
what holds the support hand.

**[DUMP] Nothing does.** A search for `LeftHand`, `RightHand`, `OffHand`,
`SecondHand`, `HandIK`, `IKHint`, `ElbowHint`, `GripPosition`, `ForeGrip` and
`SupportHand` returns **no game code at all** — every hit is Unity's XR input
layouts or its own `Avatar` / `AvatarTarget` enum. `SoldierWeapon` has one
`WeaponHandPosition`, singular, and there is no second attachment transform
anywhere.

**[inferred] Both hands are posed by the clip and never move.** The animator posed
a complete two-handed grip once, around a notional rifle held at a fixed place
relative to the chest. The actual weapon is then parented into the space those
hands already define. The support hand is not *holding* anything in any
computational sense — it is simply posed where a foregrip is expected to be, and
the weapon is placed so that a foregrip is there.

**That buys zero runtime cost and charges a content-pipeline constraint instead:
every two-handed weapon in the game must share essentially the same grip-to-foregrip
geometry.** Give one rifle a longer handguard and the support hand floats; give
another a shorter one and it clips through. Nobody can fix that at runtime, because
there is no solver to fix it with — it has to be fixed in the model, by the artist,
for every weapon in the roster. **[inferred]** That is almost certainly why
`SoldierWeapon` caches a `_zeroRotation`: the alignment is fiddly enough to need a
per-weapon correction quaternion, but only *one*, applied at attach time.

### 4a.2b Aiming without breaking the grip

The remaining problem is that a `LookAtConstraint` rotating the weapon would swing
it out of hands that cannot follow. **[DUMP]** The constraint is built onto a
dedicated object:

```csharp
public void .ctor(Animator animator, GameObject weaponHolder, SoldierAnimationStates[] soldierAnimationStates);
```

and the body has its own aim rotation, authored separately from its movement one:

```csharp
[Tooltip("Soldier move rotation speed")]      public float RotationSpeed;          // 0x3C
[Tooltip("Shoot and Aim rotation speed")]     public float ShootAimRotationSpeed;  // 0x40
public float AimingAnimationDuration;                                              // 0x70
```

driven per soldier by:

```csharp
private void CalculateSoldierRotationToTarget(float deltaTime, in Entity weaponEntity,
    ref SoldierComponent soldierCom, ref TransformComponent soldierTransform);
```

**[inferred] So aiming is split across two mechanisms, and the split is what
protects the grip.** Azimuth is handled by **turning the whole soldier** toward the
weapon's target at `ShootAimRotationSpeed` — which rotates the root transform and
therefore preserves the clip pose *exactly*, hands, weapon and all. The
`LookAtConstraint` on the weapon holder is left with only the residual: elevation
(shooting up at a helicopter, or down a slope) plus whatever azimuth error remains
while the body catches up.

**[inferred]** That residual does technically break the grip — the weapon rotates a
few degrees relative to hands that stay put. At a strategic camera distance it is
invisible, and the design bets on exactly that. It is the same bet as §6.2's
noise-driven suspension and §4a.3's three-pose seats: **spend nothing, be
approximately right, and rely on the camera never getting close enough to
disagree.**

Note the ordering this implies. A soldier told to engage something behind him must
first *turn*, at a rate a designer set, before his weapon points anywhere useful —
which is why `AimingAnimationDuration` exists and why
[`broken_arrow_squads.md`](broken_arrow_squads.md) §4.4's aiming queue promotes the
next shooter into `SoldierState.Aim` **ahead of the shot**. The pre-aim window is
not just cosmetic scheduling; it is the time the body needs to rotate so that the
constraint has only a small correction left to make.

### 4a.2c Feet: there is no foot IK, the whole body is lerped onto the terrain

**[DUMP]** Foot placement is handled by a system that never looks at a foot:

```csharp
[With(new[] { typeof(TransformComponent) })]
[Without(new[] { typeof(LoadedComponent) })]
[Without(new[] { typeof(AirdropComponent), typeof(DeadComponent), typeof(DeathComponent) })]
[WithEither(new[] { typeof(UnitGroundVehicleFlag), typeof(UnitWaterVehicleFlag),
                    typeof(UnitInfantryFlag), typeof(SoldierComponent) })]
public class VerticalOrientationSystem : AEntitySetSystem<float> {
    private const float RAYCAST_DEFAULT_OFFSET       = 5;
    private const float RAYCAST_DISTANCE             = 1000;
    private const float WATER_DIVE_OFFSET            = 1.45;
    private const float VERTICAL_TELEPORT_DELTA      = 3.5;
    private const float VERTICAL_LERP                = 12.5;
    private const float NORMAL_LERP                  = 2.5;
    private const float NORMAL_SAMPLE_SPEED          = 3;
    private const float NORMAL_SAMPLE_PERIOD         = 1;
    private const float EDGE_HEIGHT_DELTA_MULTIPLIER = 1;
    private readonly IRaycastBatchService _raycastBatchService;
    private readonly MapMetaData _mapMeta;
    private float _normalSampleTimer;
    private bool  _isEvenProcessing;                 // ← alternate-frame processing

    private Vector3 CalculateTerrainNormal(in Vector3 position, in Vector3 speed);
    private Vector3 GetNormalSamplePoint(int x, int y, float centerHeight, TerrainType bridgeTerrain);
    private float   GetTerrainHeightSafe(Entity entity, TerrainType terrain, float height,
                                         float maxHeightDelta, ref VerticalOrientationComponent verticalComp);
    private float   GetSoldierHeightSafe(Entity soldierEntity, float soldierHeight, float maxHeightDelta);
}

public struct VerticalOrientationComponent {
    public float   RaycastTimer;         // 0x0
    public bool    IgnoreNormal;         // 0x4
    public Vector3 CurrentTerrainNormal; // 0x8
    public Vector3 NewTerrainNormal;     // 0x14
    public float   TerrainPointY;        // 0x20
    public float   NewTerrainPointY;     // 0x24
    public Nullable<float> WaterPointY;  // 0x28
    public float   WaterDiveOffset;      // 0x30
}
```

**[inferred] The unit of ground contact is the entire body, not the foot.** Sample
a terrain height and a terrain normal, lerp the entity's Y toward `TerrainPointY`
at `VERTICAL_LERP = 12.5`, and lerp its up-vector toward the smoothed normal at the
much lazier `NORMAL_LERP = 2.5`. **A soldier standing on a slope is not standing on
it — he is a rigid figure whose root has been dropped to the surface.** His feet
are wherever the clip put them.

Six details make it work anyway, and each is worth taking:

**`VERTICAL_TELEPORT_DELTA = 3.5`** — beyond a 3.5 m height difference, **snap
instead of lerp**. Walking off a cliff or a bridge edge should be instantaneous;
lerping would have the unit sail out into space for a fraction of a second. A
smoothing rule needs a discontinuity clause, and this is it.

**Two lerp rates, five times apart.** Height tracks fast (12.5) because being
visibly above or below the ground is unacceptable; the *normal* tracks slowly
(2.5) because tilt is cosmetic and a fast normal lerp makes vehicles twitch on
every pebble. **The thing that must be right tracks quickly; the thing that must
merely look plausible tracks slowly.**

**`CalculateTerrainNormal(in Vector3 position, in Vector3 speed)` takes velocity.**
**[inferred]** The normal is sampled with a lead in the direction of travel, so a
vehicle begins pitching *before* it reaches a slope rather than snapping when it
arrives — with `NORMAL_SAMPLE_SPEED = 3` as the lead factor. That is the same
lookahead idea as the aircraft's `TargetAheadFactor`, applied to terrain
conformance.

**`NORMAL_SAMPLE_PERIOD = 1`** — normals are resampled at **1 Hz** and interpolated
between, while height is sampled far more often. Another instance of this
codebase's habit of picking a rate per quantity rather than per system.

**`_isEvenProcessing`** — **[inferred]** alternate-frame processing, so half the
entities are updated each frame. With a 12.5 lerp rate the visual difference is
nothing, and it halves the cost of the one system that must run on *every* ground
entity in the game, soldiers included.

**`GetSoldierHeightSafe` is a separate function from `GetTerrainHeightSafe`**, and
both take a `maxHeightDelta` clamp. **[inferred]** That clamp is what stops a
soldier standing beside a wall or a building edge being snapped up onto it when
his sample point catches the roof — a single pixel of the grid disagreeing with
his neighbours would otherwise teleport him a storey up.
`EDGE_HEIGHT_DELTA_MULTIPLIER = 1` is the tuning knob on it.

And `IgnoreNormal` on the component **[inferred]** is almost certainly how infantry
stay upright while vehicles conform: a man on a 20° slope leans in real life, but a
rigid figure tilted 20° with clip-posed feet reads as *sliding down the hill*,
whereas an upright figure with slightly clipped boots reads as fine.

### 4a.2d And the foot-sliding problem is solved by not having it

The usual reason to want foot IK is not terrain conformance but **skating** — feet
sliding when the clip's stride does not match the movement speed. **[DUMP]** Broken
Arrow's answer is in the tooltips:

```csharp
[Tooltip("Speed of soldiers when running at a constant speed")]  public float RunSpeed;
[Tooltip("Speed of soldiers when sprinting at a constant speed")] public float SprintSpeed;
```

and in a signature that takes speed as a plain scalar with no acceleration model:

```csharp
private void CalculateSoldierPositionAndRotation(float deltaTime, ref SoldierComponent soldierCom,
    ref TransformComponent soldierTransform, Quaternion resultAngle, float speed, bool isLoading);
```

**[inferred] A soldier moves at one of two or three authored constant speeds, and
the run and sprint clips were authored for exactly those speeds.** Match the
movement to the clip rather than the clip to the movement, and the feet cannot
slide, because the stride length is correct by construction. No IK, no
root-motion extraction, no playback-rate scaling.

**The price is paid somewhere visible, though.** The *squad* moves at a
terrain-dependent speed ([`broken_arrow_vehicles.md`](broken_arrow_vehicles.md)
§2) while its *soldiers* move at fixed clip speeds toward slots that are
themselves moving. When the squad is slower than `RunSpeed`, each man reaches his
slot, stops, waits for it to move ahead, and runs again — which is exactly the
constant shuffling and catching-up that infantry in this genre visibly do. The
machinery for it is right there:

```csharp
[Tooltip("If distance to squad more that, soldier change run to sprint (only for try swap to idle)")]
public float DistanceForSprint;
[Tooltip("Distance the soldier must change run to idle or aim state (0.1 good only for test map [sqrMagnitude]")]
public float DistanceForSoldierStop;
private bool IsSprint(in Vector3 destination, in Vector3 position, float distOffset = 0);
```

**[inferred] So the shuffle is not a bug, it is the visible cost of never sliding a
foot** — and given the alternative is per-soldier IK on hundreds of men, it is
plainly the right trade at this camera distance. It is worth naming because the
symptom reads as sloppiness and the cause is a deliberate, well-chosen constraint.

### 4a.3 Crewing a vehicle: three poses, and a seat is data

This is the direct answer to "how do a soldier's hands get onto the machine gun on
a Humvee". **[DUMP]**

```csharp
// Namespace: BrokenArrow.Client.Ecs.Transports
public class SeatData {
    public readonly Vector3       LocalPosition;  // 0x10
    public readonly Quaternion    LocalRotation;  // 0x1C
    public readonly SeatAnimation Animation;      // 0x2C
    public Entity Unit;                            // 0x30
}

public enum SeatAnimation : byte { None = 0, Stand = 1, Sit = 2 }

public struct CargoContainerComponent {
    public readonly float      MaxCargoWeight;
    public readonly uint       MaxCapacity;
    public readonly FastList<Entity> UnitsInside;
    public readonly SeatData[] AnimationSeats;     // 0x10
    public readonly Vector3[]  UnloadPositions;    // 0x18
    public bool TryGetFreeSeat(out int seatIndex);
}

[IsReadOnly] public struct UnitVisibleInContainerComponent {
    public readonly Entity Container;  // 0x0
    public readonly int    SeatIndex;  // 0x8
    public void FreeSeat();
}

[Without(new[] { typeof(DeadComponent), typeof(DeathComponent),
                 typeof(RemoveUnitComponent), typeof(LoadedComponent) })]
[With(new[] { typeof(TransformComponent), typeof(UnitVisibleInContainerComponent) })]
public class UnitVisibleInContainerSystem : EcsSetSystem<float> { }
```

**[inferred] A seat is a local position, a local rotation, and one of three
poses.** `None`, `Stand`, `Sit` — **that is the complete vocabulary for a mounted
human in this game.** The gunner in a Humvee turret is a soldier entity parked at a
seat transform playing the `Stand` clip; a passenger in the back is the same thing
playing `Sit`. Nobody's hands are placed on anything. The weapon is part of the
vehicle, the man is posed beside it, and the camera is far enough away that the
alignment only has to be *approximately* right — which an artist guarantees once,
at authoring time, by placing the seat transform.

Note the filter: `UnitVisibleInContainerSystem` is `[Without(LoadedComponent)]`.
**[inferred]** `LoadedComponent` sits on the *squad*; `UnitVisibleInContainerComponent`
sits on the individual *soldier* entities that are being displayed. So the system
that drives visible passengers iterates bodies, while the system that suppresses
a mounted squad's movement and shooting iterates squads — the same two-tier split
[`broken_arrow_squads.md`](broken_arrow_squads.md) §1 describes, reused here.

`TryGetFreeSeat(out int seatIndex)` plus `FreeSeat()` means seats are a **pool**:
a transport with four visible seats shows the first four men who board and hides
the rest, which is why `CargoContainerComponent` carries `MaxCapacity` separately
from `AnimationSeats.Length`.

**[inferred] This is the same conclusion
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) §4 reaches
about Eugen**, whose vehicle crew are *"statues — one
`TCosmeticFreezeSkeletalAnimationOperatorDesc` holding a single frame, no
shadow"*. Broken Arrow offers three options where Eugen offers one frozen frame,
but the principle is identical: **crew are posed, not solved.** Two studios, two
engines, twelve years apart, and neither considered putting a hand on a grip at
runtime.

### 4a.4 Buildings: nobody is rendered at all

**[DUMP]**

```csharp
public class BuildingInitializer : MonoBehaviour {
    public BuildingInitializer[] Neighbors;   // 0x58
    public Transform[] Doors;                 // 0x60
    [Space]
    public Transform ShootPosition;           // 0x68   ← singular
    [Space]
    public uint Capacity;                     // 0x70
    public int  Durability;                   // 0x74
}
public class BuildingSegmentComponent {
    public Vector3[] DoorLocations;   // 0x58
    public Vector3   ShootPosition;   // 0x9C
    private const float INNER_DOORS_DEPTH = 25;
}
```

**One `ShootPosition` per building segment.** Not one per window, not one per
soldier — one.

**[inferred] Garrisoned infantry are not drawn.** The squad carries
`LoadedComponent`, which removes it from `InfantrySoldierMoveSystem` and
`SoldierWeaponSystem` by `[Without(LoadedComponent)]` — so it costs zero movement
and zero animation work — and its fire emanates from the segment's single
`ShootPosition`. There are no soldiers at windows, no per-firing-port placement,
and therefore no hands to put anywhere.

That is also why the damage model needs `HOUSE_SOLDIER_DAMAGE_MODIFIER = 1.05` and
a separate `SpottedPenaltyBuildings` ([`broken_arrow_squads.md`](broken_arrow_squads.md)
§7.1, [`broken_arrow_vision.md`](broken_arrow_vision.md) §6): with nobody rendered,
**every gameplay consequence of being inside a building has to be stated as a
number**, because none of it can emerge from where the bodies are.

### 4a.5 The pattern

**[inferred]** Across all four cases — a rifle in a hand, a gunner on a vehicle, a
passenger in a truck, a squad in a house — the answer is the same shape:

| Case | Mechanism | Runtime cost |
|---|---|---|
| Weapon in hand | reparent to a socket, grip baked into the clip | zero |
| Weapon on back | reparent to the spine socket | zero |
| Support hand on a rifle | posed by the clip; nothing is attached | zero |
| Vehicle gunner / passenger | `SeatData` — local TRS + one of three poses | zero |
| Garrisoned squad | not rendered; one `ShootPosition` | zero |
| Feet on terrain | whole body lerped to a sampled height, alternate frames | a fraction of one raycast |
| Feet not sliding | movement speed matched to the clip's stride | zero |
| Aiming azimuth | rotate the whole soldier at `ShootAimRotationSpeed` | one quaternion |
| Aiming residual | one `LookAtConstraint` | one bone |

**Every hard case was converted into an authoring decision, and the only runtime
solve in the entire infantry rig is a single look-at.** That is what "IK at scale"
looks like when the camera is 200 metres up: you do not make the solver fast, you
arrange for there to be nothing to solve.

---

## 5. Vehicles: an `Animator` only for doors

**[DUMP]** The only vehicle-side `Animator` reference is inside one behaviour:

```csharp
public class AnimatorConnect : IAnimationBehaviour {
    private const string TERRAIN_EXIT_SUFFIX   = "_exit";
    private const string TERRAIN_ENTER_SUFFIX  = "_enter";
    private const string ENTRANCE_OPEN_SUFFIX  = "_open";
    private const string ENTRANCE_CLOSE_SUFFIX = "_close";
    [SerializeField] private Animator _animator;
    [SerializeField] private string _loadStartTrigger, _loadEndTrigger,
                                    _unloadStartTrigger, _unloadEndTrigger,
                                    _radarActivateTrigger, _radarDeactivateTrigger,
                                    _staticPositionTrigger, _startMovingTrigger,
                                    _reloadTrigger, _aimStartTrigger, _aimEndTrigger;
    private Dictionary<string,int> _triggersDict;   // name → StringToHash
}
```

**Every one of those eleven strings is a discrete event** — ramp down, doors open,
radar deploy, deploy-to-static, reload, aim. **Not one is speed, turn or lean.**

**[inferred] So a vehicle has an `Animator` only if its prefab has a clip-authored
mechanism** — a loading ramp, a radar mast, an artillery spade, a missile-box
elevation. Wheels, tracks, suspension, body lean and recoil never touch it; they
are `Transform` and `Material` writes from `BoneContainer.Update(float delta)`.

The locomotion rig itself is covered in
[`broken_arrow_vehicles.md`](broken_arrow_vehicles.md) §6. Two additions from the
structured dump:

**[DUMP]** `BoneContainer` exposes its inputs as ranged sliders:

```csharp
[SerializeField] public float CurrentSpeed;
[SerializeField][Range(-1, 1)] public float Turn;
[SerializeField][Range(0, 1)]  public float Shake;
private IEnumerator Acceleration();
private IEnumerator Braking();
private void ShakeBody(float delta);
```

alongside `AnimationManager.StartFakeUpdate()` / `FakeUpdate()` and an
`_updateButtonName`. **[inferred] That is an editor preview loop**: an artist drags
`Turn` from −1 to 1 and watches the whole rig respond without entering play mode.
The `[Range]` attributes exist for a human, not for validation.

**Recoil is a curve, not a clip:**

```csharp
[Serializable] public struct BoneContainer.Recoil {
    public AnimationCurve BodyRecoil;
    public float          BodyRecoilAmplitude;
    public AnimationCurve WeaponRecoil;
}
[SerializeField][Tooltip("Key - Shell Spawn")]
private BoneContainer.RecoilDataDictionary _recoils;   // UnitySerializedDictionary<Transform, Recoil>

public void OnShot(Vector3 direction, Transform shellSpawn);
private UniTaskVoid ShotProcessing(Vector3 direction, Transform shellSpawn, CancellationToken ct);
private Dictionary<Transform, CancellationTokenSource> _tokens;
```

**[inferred]** Firing walks `WeaponRecoil` (barrel slide) and
`BodyRecoil × BodyRecoilAmplitude` (hull rock) over the curve's duration in a
`UniTaskVoid` coroutine — **keyed per shell-spawn transform, with its own
cancellation token**, so a fast-firing autocannon *restarts* its recoil rather
than stacking coroutines. That per-barrel token dictionary is a small thing that
prevents a very visible bug.

Turret aim is a float, not an animation:

```csharp
public struct TurretComponent {
    public float CurrentHorizontalAngle;      // 0x14
    public float HorizontalDeltaAngle;        // 0x18
    public readonly float IdleAngle, LeftAngle, RightAngle, TotalAimingSectorAngle;
}
[With(new[] { typeof(TurretComponent) })]
public class TurretRotationSystem : EcsSetSystem<float> { }
```

**[inferred]** integrated toward target by `HorizontalDeltaAngle` per frame,
clamped to the traverse arc, and written into the turret bone's local rotation —
via the entity's own `TransformComponent`, which carries a `HierarchyLevel` and a
`_localRotationCache` precisely for nested rigs.

---

## 6. Physics: reports for the living, simulates for the dead

**[DUMP]** Live units *do* carry physics components:

```csharp
public struct PhysicsComponent {
    public readonly Rigidbody       Body;
    public readonly Collider        Collider;
    public readonly CollisionCatcher CollisionDetector;
    public readonly FastList<BodyCollisionData> CollisionList;
}

[RequireComponent(typeof(Collider))]
[RequireComponent(typeof(Rigidbody))]
[RequireComponent(typeof(EntityBinder))]
public class CollisionCatcher : MonoBehaviour {
    private void OnCollisionEnter(Collision collisionInfo) { }
    private void OnCollisionStay (Collision collisionInfo) { }
    private void OnCollisionExit (Collision collisionInfo) { }
}
```

**…but PhysX only reports. The ECS resolves:**

```csharp
[Without(new[] { typeof(DeadComponent) })]
[With(new[] { typeof(TransformComponent), typeof(SpeedComponent) })]
[With(new[] { typeof(UnitComponent), typeof(PhysicsComponent), typeof(CollisionComponent) })]
public class BodyCollisionsSystem : EcsSetSystem<float> {
    private const float MAX_SPEED_LIMIT = 300;
    private readonly float _elasticityModifier;
    private static Vector3 GetBoxSurfaceNormal(BoxCollider box, in Vector3 checkPoint);
}
```

**[inferred] `GetBoxSurfaceNormal(BoxCollider, in Vector3)` is the tell.** They take
the collider as **geometry**, compute the separation normal themselves, and apply
an elasticity-scaled push-apart in ECS code. The `Collision*` components are empty
tag structs cleared each frame by a `CollisionsCleanupSystem`. `AddForce` does not
appear as game code anywhere.

Movement is kinematic transform assignment through a write-through wrapper:

```csharp
public class TransformComponent {
    private readonly Transform _unityTransform;
    private Vector3    _globalPositionCache;
    private Quaternion _globalRotationCache, _localRotationCache;
    private const float POSITION_MARGIN = 0.001;
    private const float ROTATION_MARGIN = 0.001;
    public bool PositionChanged { get; set; }
    public bool RotationChanged { get; set; }
}
```

**[inferred]** 1 mm / 0.001-quaternion epsilons so a write that moves nothing
perceptibly never touches the native `Transform` — which in Unity dirties the
whole hierarchy and is the single most expensive per-object cost after skinning.

**Physics is death-and-debris only:**

```csharp
[Header("Crash")]
public float DEATH_MASS;
[Tooltip("Linear drag coefficient")]  public float DEATH_DRAG;
[Tooltip("Angular drag coefficient")] public float DEATH_ANGULAR_DRAG;
[Tooltip("Engine power (forward) multiplier while falling")] public float DEATH_ENGINE_POWER_MULT;
[Tooltip("Multiplier for power of explosion impulse (only for dead hit)")] public float DEATH_BLAST_POWER_MULT;
```

and the corpse mover:

```csharp
public class PhysFly : MonoBehaviour {          // namespace: …Ecs.Animations
    private Rigidbody _rb;
    private int    _limit;
    private string _impactEvent;
    private float  _lastHit;
    public void Init(in Vector3 lineForce, in Vector3 angularForce, float mass = 1,
                     string impactEvent = "", IEnumerable<GameObject> objectsToDestroy);
    private void FixedUpdate() { }
}
```

**[inferred] Every one of these is `DEATH_*`-scoped, and `PhysFly` lives in the
Animations namespace, not a physics one. The mass and drag of an aircraft are
only defined for the corpse — a flying plane has no mass in this game.** Building
collapse (`BuildingFalling._shardsChildrenRigidbody`) and thrown turrets
(`TurretFly`) are the other two clients, and shell casings do not even get that.

---

## 7. Read against the crowd-animation literature

[`lod_systems.md`](../../../topics/world/lod_systems.md) §6 sets out the ladder
before reaching for vertex textures:

1. **Bone-count LOD** — skin the distant mesh to fewer bones (AC Unity: ~300 near,
   11 far).
2. **Update-rate LOD** — evaluate the pose every N frames and interpolate.
3. **Budgeted update rate** — state a millisecond budget, rank by significance.

and concludes *"only when (1)–(3) are exhausted and you still need a single draw
call does VAT earn its memory."*

**[inferred] Broken Arrow uses none of the three, and no VAT either.** Its answer
is **binary**: past a LOD tier, the squad's animators are switched off wholesale
(`RefreshInfantryAnimatorState()`, `DisableSoldiersAnimator()`), leaving posed
statues. No bone LOD, no update-rate interpolation, no budget.

That is a genuinely different position and worth naming rather than treating as an
oversight. Three things make it defensible here:

- **The LOD unit is the squad, not the soldier** — one `CullingGroup` entry per
  squad rather than per man cuts the population eightfold before any of this.
- **Binary gating is exact and free**; update-rate LOD is approximate and still
  costs. `[Without(LoadedComponent)]` removing a mounted squad from the query set
  entirely is the same instinct taken further.
- **At RTS camera distance a frozen pose is hard to distinguish from a slow one**,
  where in a third-person game it would be obvious.

**[inferred] The generalisable rule is not "skip the ladder" but "the ladder is
priced by camera distance".** Every rung on that list buys smoothness that only a
close camera can see. A strategic camera should ask whether it needs smoothness at
all before deciding how to pay for it.

---

## 8. What transfers

1. **Split the simulation→visual channel in two: continuous state as a small
   read-only property bag, discrete events as events.** Four floats plus twenty
   events runs this entire game's visuals. Polling "did I fire" is a flag somebody
   forgets to clear; eventing "my speed changed" is a flood.

2. **Make the visual layer a list of small verbs, not a type per unit.** Twenty-five
   behaviours cover every vehicle, aircraft and ship in the game. A tank is a
   *list*, and adding a new unit is authoring rather than coding. **Three engines
   in this genre independently converged on this** — see
   [`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) §8.

3. **Put audio and VFX in the same list.** They are triggered by the same events,
   at the same moments, by the same person. Separating them into a parallel system
   guarantees they drift out of sync.

4. **One hub, one update, instead of N `MonoBehaviour.Update`s.** The plain
   `[Serializable]` classes have no Unity overhead; a single ECS system walks the
   arrays. **This is the real performance argument for the design and it is
   independent of the animation content.**

5. **Give each behaviour its own LOD cutoff and quality floor.**
   `_lodGroup` plus `StartQuality` on every behaviour means the LOD system never
   becomes a central registry of everything in the game.

6. **Second-order dynamics with a negative response constant** is the cheapest way
   to make a follower feel weighted rather than lagged — and guard the integrator,
   because a variable `deltaTime` will blow it up.

7. **Keyed, cancellable coroutines for repeatable effects.** A recoil per barrel
   with its own `CancellationTokenSource` restarts cleanly instead of stacking.

8. **Let physics report and resolve it yourself** when you need determinism.
   `Rigidbody` + `CollisionCatcher` for detection, `GetBoxSurfaceNormal` plus an
   elasticity constant for response. You keep collision without inheriting a
   solver you cannot replicate across clients.

9. **Reserve real physics for things that no longer matter.** Corpses, debris and
   thrown turrets can be non-deterministic because nothing reads them back.

---

## 9. What is not established

- **No method bodies.** How `AnimationHubSystem.Update` sequences its work, how
  `ObjectFollower.Update` composes `k1/k2/k3`, and how `Wheels.Move` applies its
  arguments are all inference from names and signatures.
- **No authored values.** Every `BoneSettings` number, every behaviour's serialized
  fields and the LOD band distances live in prefabs and assets, not the binary.
- **The `Bake()` reading is inference** from constructor signatures and cached name
  predicates; no vertex-data path was found, which is the strongest form of the
  claim available.
- **Nothing was run or profiled.** No claim about actual animation cost, or how
  many animated units the game sustains.
- **§7's argument is a reading, not a measurement.** That Broken Arrow skips the
  LOD ladder is literal; that it is *right* to is judgement.

---

## Sources

**[DUMP]** Il2CppDumper v6.7.46 over
`C:\Program Files (x86)\Steam\steamapps\common\broken_arrow\GameAssembly.dll` and
`BrokenArrow_Data\il2cpp_data\Metadata\global-metadata.dat` (IL2CPP metadata v31,
dated 2025-08-06).

Read against
[`vehicle_animation.md`](../../../topics/animation/vehicle_animation.md) (Eugen's
operator lists across twelve years) and
[`lod_systems.md`](../../../topics/world/lod_systems.md) §5–6 (the VAT/BAT
arithmetic and the deformation-LOD ladder §7 answers).
