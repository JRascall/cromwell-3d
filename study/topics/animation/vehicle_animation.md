# Vehicle animation at RTS scale — R.U.S.E., Wargame/WARNO, Broken Arrow

How do you make five hundred vehicles turn their turrets, rock on their
suspension, roll their tracks and carry visible crew, when the camera can be
twenty metres from one of them and two kilometres from the rest?

This is the note [`ruse.md`](../../games/strategy/ruse.md) and [`broken_arrow.md`](../../games/flight/broken_arrow/broken_arrow.md)
both stop short of. They cover how these games *stream* and *simulate*; this is
how they *move the parts*. It is read from four shipping builds on this machine
— R.U.S.E. (2010), Wargame: Red Dragon (2014), WARNO (2022) and Broken Arrow
(2025) — which is enough points to see what stayed and what was rebuilt.

**The one-line answer, and it is the same in all four: none of this is
animation.** There is not a single authored clip anywhere in a vehicle. A turret
is two bone rotations solved from a published aim vector; a chassis is a
spring-damper; a track is a wheel-count and a bone-name prefix; a rotor is an
RPM. What *is* authored is a **list of small operators**, each of which knows one
bone name and one number, applied in order to a reset pose every frame. Eugen
call that list a **depiction** and its members **cosmetic operators**. Steel
Balalaika, on a different engine fifteen years later, call it an
**`AnimationHub`** holding an `IAnimationBehaviour[]`. Nobody appears to have
copied anybody. The camera forces it.

> **Sources and their grades.**
>
> **[BUILD-WARNO]** — WARNO's mod workspace, `Mods\Test\{GameData,CommonData}`,
> 2,873 `.ndf` files of Eugen's own NDF **source, comments intact**. This is the
> best source in the note by a wide margin and most of §2–§4 comes from it.
>
> **[BUILD-WRD]** — Wargame: Red Dragon, `Data\WarGame\PC\510064564\NDF_Win.dat`,
> read with [`../tools/ruse/ruse_edat.py`](../../../tools/ruse/ruse_edat.py) +
> [`../tools/ruse/ruse_ndf.py`](../../../tools/ruse/ruse_ndf.py).
>
> **[BUILD-RUSE]** — R.U.S.E., `Data\PC\190852\ZZ_GladPatchableWin.dat`, same
> readers. Class and property *names* here are read from the `CLAS`/`PROP`
> string tables and are trustworthy; **values are not**, because the NDF value
> decoder is incomplete (see [`ruse.md`](../../games/strategy/ruse.md) §12). Every R.U.S.E. claim
> below is about *schema*, never about a number, and §10 says so again.
>
> **[BUILD-BA]** — Broken Arrow's IL2CPP identifier table, 203,203 names.
> **A name proves compilation, not use** — `broken_arrow.md` §4.1 is the worked
> example of how badly that misleads.
>
> **[TRACE-BA]** — Broken Arrow runtime stack traces and warnings from the
> shipped `GameLogs\` (49 logs). These demonstrably ran.
>
> **[inferred]** — our reading.

Related: [`ruse.md`](../../games/strategy/ruse.md) (the engine this all sits on),
[`broken_arrow.md`](../../games/flight/broken_arrow/broken_arrow.md) (§10's convergence argument, of which this
note is the strongest single instance), [`source2_animation.md`](../../games/valve/source2_animation.md)
and [`rigging_ik.md`](rigging_ik.md) (the same problems solved for one character
you stand next to), [`lod_systems.md`](../world/lod_systems.md) (§5 here is a fifth
meaning of LOD), [`crowd_scale.md`](../scale/crowd_scale.md) (the same expensive-near /
cheap-far bet).

---

## 1. The camera writes the specification

Everything below follows from one constraint, which is the same constraint
[`ruse.md`](../../games/strategy/ruse.md) §1.2 identifies for terrain: **the camera is allowed
anywhere from ground level to the whole map, with no loading screen, and the
unit count does not drop when it comes down.**

That rules out both easy answers.

- **Authored clips per vehicle** would need a clip per turret angle, which is
  not a thing, so turrets have to be procedural whatever else happens.
- **Skipping it entirely** is not available either, because at 20 m a static
  turret reads as a bug, and the deep zoom is the genre's selling point.

So the parts must be driven procedurally, cheaply, for hundreds of units, from
data an artist can author without a programmer. Every design decision in this
note is a consequence.

**[inferred]** The second constraint is subtler and shows up in §4: the crew
figures. A soldier standing in a hatch is a *skinned mesh*, and skinning is the
expensive part — not the maths that decides the pose. Which is why, as we will
see, Eugen's answer is to keep the mesh and throw away the animation.

---

## 2. Eugen's answer: the depiction is an operator stack

**[BUILD-WARNO]** A unit's visual is a `TDepictionTemplate`. The parts that
matter here:

```
TDepictionTemplate
(
    Selector              = ...    // which mesh, by camera distance
    DepictionAlternatives = [...]  // the meshes themselves, per LOD
    Operators             = [...]  // ordered list — this is the animation
    Actions               = MAP[]  // FX and sound, keyed by event tag
    SubDepictions         = [...]  // whole child depictions on named bones
    SubDepictionGenerators= [...]  // ... generated at runtime rather than authored
)
```

`Operators` is the whole animation system. Here is a real one, a US M1025
Humvee scout, verbatim from
`GameData/Generated/Gameplay/Gfx/Depictions/GeneratedDepictionVehicles.ndf`:

```
Gfx_M1025_Humvee_scout_US_Autogen is TacticVehicleDepictionTemplate
(
    CoatingName = 'M1025_Humvee_scout_US'
    Selector    = Selector_M1025_Humvee_scout_US
    Alternatives= Alternatives_M1025_Humvee_scout_US

    Operators =
    [
        DepictionOperator_CropFlattening,
        $/GFX/Sound/DepictionOperator_MovementSound_SM_Humvee,
        DepictionOperator_CriticalEffects,
        DepictionOperator_Chassis_MediumTank_NoCanonRecoil,
        DepictionOperator_Propulsion_Wheels_Generic,
        DepictionOperator_MovementFX_Standard,
        DepictionOperator_Turret_1_Aim,
        DepictionOperator_Turret_1_TurretRockingRecoil_LightMachineGun,
        DepictionOperator_M1025_Humvee_scout_US_Weapon1,
    ]

    Actions = MAP[ ( [ "weapon_effet_tag1" ], Weapon_HMG_12_7_mm_M2HB ) ]
            + DepictionAction_Stress_And_Wrecked
            + DepictionAction_MovementFX_Wheeled
            + DepictionAction_CriticalFX_Tank

    SubDepictions          = [] + HumanSubDepictions_M1025_Humvee_scout_US
    SubDepictionGenerators = [ TransportedInfantrySubGenerator(Mesh = $/GFX/DepictionResources/Modele_M1025_Humvee_scout_US) ]
)
```

Nine operators. That is the entire moving-parts description of a Humvee: it
squashes crops, makes engine noise, shows critical-damage effects, sits on a
suspension, rolls and steers its wheels, kicks up dust, aims a turret, rocks the
gun when it fires, and emits a muzzle flash. **Every one of those is a shared,
named object reused across hundreds of units**; only the last is unit-specific,
and it is generated.

572 vehicle depictions and 273 aerial ones are written this way. Across all of
them, **161 distinct operator instances** are referenced — so the average
operator is reused across several units, and the tail of one-offs is small.

### 2.1 Reset, then apply — the frame contract

The first operator in every reusable list is this, and the comment in
`DepictionOperators.ndf` says why:

```
// *** Reset (doit être le premier Operator de toute liste qu'on souhaite
//     réinitialiser à chaque frame)
DepictionOperator_Reset_Generic is TCosmeticSetToPoseOperatorDesc()
```

**[inferred]** So the contract is: set the skeleton back to the bind pose, then
let each operator write the bones it owns. No operator has to undo anything, no
operator has to know what ran before it, and the list is order-independent
except where two operators deliberately touch the same bone (turret aim then
turret recoil — aim sets the barrel's direction, recoil slides it along it).

This is the same design as a shader's fixed function stack, and it has the same
virtue: **an operator is a pure function of (published simulation state, bone),
so it can be written, tested and reused with no knowledge of the unit it is on.**

`TCosmeticSetToPoseOperatorDesc` is one of the 37 classes that
[`ruse.md`](../../games/strategy/ruse.md) §11.1 finds spanning R.U.S.E., Red Dragon and WARNO. The
reset-then-apply contract is twelve years old and unchanged.

### 2.2 The turret operator is two bones and two property names

**[BUILD-WARNO]** This is the core of the whole subject and it is fourteen
lines:

```
// On déplie les mangling d'indices de tourelles pour faciliter les recherches
// et refactos futures
DepictionOperator_Turret_1_Aim is TCosmeticTurretOperatorDesc
(
    OperatorId                = 'tourelle1'
    ZAxisNode                 = 'tourelle_01'
    ZAxisPhysicalPropertyName = 'tourelle1'
    YAxisNode                 = 'axe_canon_01'
    YAxisPhysicalPropertyName = 'tourelle1axe'
)
```

...repeated verbatim for turrets 2 through 5, with the indices substituted. The
comment ("we unroll the turret index mangling to make future searching and
refactoring easier") is a deliberate choice to write five near-identical objects
rather than one templated one, and it is the right call in a data file that gets
grepped.

Read it: **traverse is a rotation of `tourelle_01` about Z, elevation is a
rotation of `axe_canon_01` about Y, and the angles come from two named
properties the simulation publishes.** That is it. There is no interpolation
policy, no acceleration curve, no "turret controller" — the *simulation* owns
the turret's actual angle (§2.4), and the operator's only job is to get that
number onto a bone.

**Two separate bones for the two axes** is the piece worth noticing. It means
the mesh's skeleton encodes the mount: `axe_canon_01` is a child of
`tourelle_01`, so elevating after traversing is free and correct, and a turret
whose gun is not on the turret's centreline just works. No gimbal maths in code;
the rigger's hierarchy *is* the gimbal.

**Nested turrets.** **[BUILD-WARNO]** A commander's cupola MG on top of a tank's
main turret is expressed on the gameplay side as a second turret carrying
`MasterTurretYulBoneOrdinal = 1` — "my angles are relative to turret 1". The
depiction needs no special case at all, because the skeleton already nests.

### 2.3 The binding: `Tag` ↔ `OperatorId`, and a name against an ordinal

**[BUILD-WARNO]** The gameplay turret and the cosmetic operator are two
different objects in two different files, and they find each other by string:

| | gameplay side | depiction side |
|---|---|---|
| identity | `Tag = 'tourelle1'` | `OperatorId = 'tourelle1'` |
| bone | `YulBoneOrdinal = 1` | `ZAxisNode = 'tourelle_01'` |
| angle | *computed* | `ZAxisPhysicalPropertyName = 'tourelle1'` |

Three separate naming schemes for one turret, and the split is not sloppiness.

- **`OperatorId`** is how the *LOD system* addresses an operator (§5) — it is
  the operator's handle, not the turret's.
- **`ZAxisPhysicalPropertyName`** is the *channel*. The simulation writes a
  named float; the depiction reads it. Neither holds a pointer to the other, and
  a unit with no turret-1 operator simply has nobody reading the property.
- **`YulBoneOrdinal`** is the *simulation's* handle on the same bone — a number,
  not a name. **[inferred]** Because the simulation needs the muzzle's world
  position every time the weapon fires, and a per-shot string lookup into a
  skeleton is exactly the hash-in-a-hot-loop CLAUDE.md forbids. An ordinal is an
  array index. The depiction, which touches each bone once per frame in cold-ish
  code, gets to keep the readable name.

That split — **names where a human authors, ordinals where the simulation
indexes** — is worth stealing outright.

### 2.4 The gameplay turret: five subclasses, one property set, radians

**[BUILD-WARNO]** The simulation side lives in `WeaponDescriptor.ndf` and it is
where the turret's *behaviour* is. A real one, the M113 ACAV's commander cupola:

```
TTurretTwoAxisDescriptor
(
    AngleRotationBase             = 0.0
    AngleRotationBasePitch        = 0.17453292519943275   //  10°
    AngleRotationMax              = 6.283185307179586     // 360°
    AngleRotationMaxPitch         = 0.7853981633974483    //  45°
    AngleRotationMinPitch         = -0.17453292519943295  // -10°
    MountedWeaponDescriptorList   = [ ... ]
    OutOfRangeTrackingDuration    = 3.5
    Tag                           = 'tourelle1'
    TurretIdleBehaviourDescriptor = ~/TurretIdle_WatchForwardMG
    VitesseRotation               = 1.0471975511965976    //  60°/s
    YulBoneOrdinal                = 1
)
```

Everything angular is in **radians**, generated from a spreadsheet, which is why
they appear to seventeen significant figures. `AngleRotationBase` is the rest
heading, `AngleRotationMax` the traverse arc (`2π` = free), the three `Pitch`
values the elevation envelope, `VitesseRotation` the slew rate.

The subclasses in use across the database:

| class | instances | what it is |
|---|---|---|
| `TTurretTwoAxisDescriptor` | 959 | traverse + elevation — the normal case |
| `TTurretInfanterieDescriptor` | 750 | **a soldier's weapon** |
| `TTurretUnitDescriptor` | 455 | the whole unit turns to aim — no turret bone |
| `TTurretBombardierDescriptor` | 77 | an aircraft's bomb release |

**`TTurretInfanterieDescriptor` is the finding here.** It carries a
`MountedWeaponDescriptorList` and a `YulBoneOrdinal` and *nothing else* — no
angles, no slew rate. A rifleman's rifle is a turret with no axes. So the same
`TurretDescriptorList` on the same `TWeaponManagerModuleDescriptor` describes a
Leopard's 120 mm, a Humvee's roof gun and a Gurkha's Sterling, and every
consumer — targeting, line of fire, ammunition, the UI — is written once. **The
subclass decides only whether there are axes to move**, which is precisely the
part that differs.

`TTurretUnitDescriptor`'s 455 instances are the honest cheap answer for
everything with a fixed gun: no bone moves, the *chassis* turns, and it costs
nothing.

### 2.5 Turret idle behaviour — the operator that sells the whole thing

**[BUILD-WARNO]** `GameData/Gameplay/Gfx/Units/Gfx_UnitIdles.ndf`, whose header
comment is a note-to-self about the spreadsheet pipeline:

```
// pour que l'unit idle soit configurable depuis l'ods, prefixer avec TurretIdle_
```

The parameterisation:

```
template TurretIdleBehaviour
[
    MinTimeBetweenIdleSequences, MaxTimeBetweenIdleSequences,
    MinIdleTargetWaitDuration,   MaxIdleTargetWaitDuration,
    IdleSequenceProbability,     RotationSpeedMultiplier,
    MaxYawSpreadInDegree,        MaxPitchSpreadInDegree,
    PitchMaxInDegree = 90, PitchMinInDegree = -90,
    YawMaxInDegree  = 180, YawMinInDegree  = -180
] is TTurretIdleBehaviourDescriptor( ... )
```

Ten numbers, and they describe a complete behaviour: *wait a random 10–20 s;
with probability 0.5 pick a random heading within ±120° of rest; slew there at
20% of your real rotation speed; hold it 10–20 s; repeat.*

The shipped instances are tuned per role and the tuning is legible:

| | `TurretIdle_WatchForwardNormal` | `TurretIdle_WatchForwardMG` | `TurretIdle_DCAAutoMoteur` | `TurretIdle_ArtilleryAutoMoteur` |
|---|---|---|---|---|
| interval | 10–20 s | 20–30 s | 10–20 s | 20–30 s |
| dwell | 10–20 s | 30–40 s | 10–20 s | 40–60 s |
| probability | 0.5 | 0.6 | 0.5 | 0.5 |
| speed × | 0.2 | 0.1 | 0.25 | **0.05** |
| yaw spread | 120° | 50° | **360°** | **0°** |
| pitch spread | 0° | 45° | 75° (15–75°) | 25° (0–25°) |

A tank scans a 120° frontal arc slowly. An AA mount sweeps the entire sky.
**Self-propelled artillery has a yaw spread of exactly zero and a speed
multiplier of 0.05** — it never turns, it just lifts and lowers the barrel, very
slowly. That is one row of a table doing the work of an animation.

**[inferred]** The reason this matters more than it looks: a column of forty
parked vehicles with mathematically identical turrets reads as *a screenshot*.
Ten numbers and a per-unit random phase is the entire difference between a
diorama and an army. It is also the cheapest thing in the file — no clip, no
skinning, no state machine, and it runs on units the player is not looking at
because it costs a comparison against a timer.

Every commented-out old value is still in the file (`0.5 //0.6`,
`120 //60`), which is Eugen's habit throughout and makes the tuning history
readable.

### 2.6 Recoil: two models, one published impulse

**[BUILD-WARNO]** The simulation publishes `ShotImpulse_N` when turret *N*
fires. Three different operators read it, and which one a unit gets is a
statement about what kind of gun it has.

**Hydraulic recoil** — the barrel slides back into the mantlet and returns:

```
template DepictionOperator_TurretRecoil [ RecoilNode, ShotImpulsePropertyName ]
is TCosmeticCannonHydraulicRecoilOperatorDesc
(
    OperatorId  = 'cannon_hydraulic_recoil'
    RecoilNode  = <RecoilNode>          // 'canon_01'
    MaxRecoil   = 75    // 40 // 20 // 80.  Distance totale du recul
    RecoilSpeed = 4     // 0.8            Vitesse de recul
    RecoilPeak  = 0.5   // 0.9
    // entre 0 et 1, détermine à quel "pourcentage" de l'aller-retour le canon
    // est complètement rentré. Ex : 0.9 veut dire qu'il mettra 9 fois plus de
    // temps à revenir. 0.5 veut dire qu'il met autant de temps à rentrer qu'à
    // sortir.
)
```

`RecoilPeak` is the detail worth keeping: **a single scalar in [0,1] that splits
one duration into a fast-out and slow-back**, with the comment spelling out the
9× asymmetry. That is the difference between a gun and a piston, and it is one
number rather than a curve asset.

The autocannon variant is the same object with `MaxRecoil = 10` instead of 75.

**Rocking recoil** — for a machine gun, where the barrel does not slide but the
mount kicks:

```
template DepictionOperator_TurretRockingRecoil_LightMachineGun [ PitchNode, ... ]
is TCosmeticCannonRockingRecoilOperatorDesc
(
    PitchNode   = <PitchNode>   // 'axe_canon_01' — the elevation bone
    PitchMax    = 0.175         // rad ≈ 10°
    PitchPeriod = 0.25
    PitchDamp   = 0.025
)
```

A damped oscillator on the **elevation bone** — the same bone the aim operator
writes, one operator later in the list. That is the ordering dependency §2.1
mentions, and it is the only one in the file.

**Rotary cannon** — a third case, because a gatling's barrels spin *before* it
fires and coast down after:

```
is TCosmeticRotaryCannonOperatorDesc
(
    XAxisRotationNode                  = 'canon_01'
    XAxisHasTargetPhysicalPropertyName = 'tourelle1target'
    RotationSpeed                      = 100.0   // fast variant; slow is 25.0
    RotationAcceleration               = 2.0     //                      1.0
)
```

Note the property it reads: **`tourelle1target`, not `tourelle1shooting`.** The
barrels spin up when the turret *acquires* a target, not when it fires — which
is both correct and the only way the spin-up is ever visible.

### 2.7 Chassis, tracks, wheels

**[BUILD-WARNO]** The chassis operator ports a shared descriptor:

```
template DepictionOperator_Chassis [ ChassisDescriptor ] is TCosmeticChassisPortingDesc
(
    OperatorId              = 'chassis'
    ShotImpulsePropertyName = 'ShotImpulse_1'  // ShotImpulse de la tourelle principale.
    ChassisDescriptor       = <ChassisDescriptor>
)
```

and there are exactly six descriptors for the entire game
(`GfxDescriptorChassis.ndf`):

```
export GfxDescriptorChassis_MediumTank is TGfxDescriptorChassis
(
    SousMobileName         = 'chassis'
    SpringX = 30   SpringY = 50
    DamperX =  2   DamperY =  6
    SpringDamperMultiplier = 10
    NoiseGridSize   = 10
    NoiseGridHeight = 1.5
    SpringDamperMaxX = 0.2   SpringDamperMaxY = 0.1
    Force = 4
)
```

Two independent spring-dampers (pitch and roll), clamped to ±0.2 and ±0.1
radians, plus **a world-space noise grid** — 10 m cells, 1.5 amplitude — that
supplies the terrain roughness. **[inferred]** So the vehicle is not sampling
the ground under each wheel; it is sampling a 2D noise field at its own
position. Two vehicles side by side rock differently, a column driving the same
road rocks the same way twice, and the cost is one noise lookup per vehicle per
frame instead of four raycasts.

`Force` is the shot-impulse gain — and there is a variant,
`GfxDescriptorChassis_MediumTank_NoCanonRecoil`, which is byte-identical except
that `Force` is **absent**. That is the one the Humvee uses (§2). A wheeled jeep
borrowing a medium tank's suspension curve with the gun-kick term deleted: data
reuse by copy, and the name has stopped being true. Worth noting as the cost of
the approach — six shared descriptors is cheap, but the sixth one is called
`MediumTank` and is bolted to a Humvee.

**Tracks** are four fields:

```
//array-ization
DepictionOperator_Propulsion_ContinuousTrack is TCosmeticCaterpillarTrackOperatorDesc
(
    OperatorId         = 'Tracks'
    WorldFloorProxy    = $/M3D/Scene/WorldFloorForOnlyGround
    FirstWheelOnGround = false
    LastWheelOnGround  = false
)
```

The two booleans are the idler and drive sprocket — the wheels at each end that
are *raised*, so the road wheels drop to the terrain and those two do not. The
`WorldFloorProxy` is the shared ground query. The stray `//array-ization` is
somebody's TODO left in the shipped file.

**Wheels** are three booleans, and the three instances say exactly what varies:

```
DepictionOperator_Propulsion_Wheels_Generic    ( RotateDirection = true  MoveFromSuspension = true  )
// Version des canons (les roues ne tournent pas lors des virages et il n'y a pas de FX de propulsion)
DepictionOperator_Propulsion_Wheels_Canon      ( RotateDirection = false MoveFromSuspension = false )
DepictionOperator_Propulsion_Wheels_TowedCanon ( RotateDirection = false MoveFromSuspension = false ReverseRotation = true )
```

A towed gun's wheels roll *backwards* relative to the tractor's, because the
gun is hitched facing rearward and nobody was going to re-author the mesh.

**Rotors** carry two bones each:

```
THelix
(
    BladeCount     = 4
    BladesBoneName = "helice_ls_1"       // the modelled blades
    HelixBoneName  = "bloc_moteur_1"     // the hub they rotate about
    BlurActionId   = [ "FX_Helice_1" ]   // the disc that replaces them
    RotationAxis   = 2
    RotationSpeed  = 242                 // RPM
    Clockwise      = False
)
```

with the operator carrying `DecelerationAfterDeath = 2.0` and
`ForceRotorBladeBlur = $/Camera/ForceRotorBladeBlur` — a camera-driven switch
between real geometry and a blur card. **[inferred]** The blur is an *Action*
(an FX event), not an operator, so the crossover is a spawn/despawn rather than
a per-frame branch.

### 2.8 The escape hatch: a generic bone driver, and a side table of exceptions

Two mechanisms handle everything the vocabulary above does not.

**[BUILD-WARNO]** First, a generic procedural bone primitive:

```
TBoneProceduralAnimation
(
    BoneName      = 'roue_avant'
    Axis          = 1
    LimitValue    = 75
    Delay         = 4
    Duration      = 1
    IsTranslation = true      // otherwise it is a rotation
)
```

Six fields. Bone, axis, target value, when it starts, how long it takes, and
whether it is a rotation or a slide. A Mi-24's undercarriage retraction is
**six of these** — nose wheel up over 1 s at t=4, two bay doors swinging ±95°
at t=6, main gear translating 1.5 and rotating 25° over 2 s at t=4. That is a
landing-gear animation authored with no animator, no clip, no exporter, in a
text file.

Second, the **coating pantry** — a `MAP` from a string key to an operator, which
a depiction opts into by `CoatingName`:

```
liveEditCoatingPantry is TCoatingPantry
(
    Entries = MAP[
        ('F117_Nighthawk_trapdoor',   TCoatingIngredientOperator( Descriptor = TCosmeticTrapdoorOperatorDesc(...) )),
        ('Faun_Kraka_20mm_RFA_offset',TCoatingIngredientOperator( Descriptor = TCosmeticOffsetTurretWhileMovingOperatorDesc( AngleInDegree = 20 ) )),
        ('Mi_24V_SOV_landing_gears',  TCoatingIngredientOperator( Descriptor = TCosmeticLandingGearOperatorDesc( AnimationList = [ TBoneProceduralAnimation(...) x6 ] ) )),
        ...
    ]
)
```

**There are five entries in the whole game.** The F-117's weapons bay, one
German light vehicle whose gun sits 20° off-axis while driving, and three sets
of helicopter landing gear. **[inferred]** That number is the strongest evidence
that the vocabulary in §2.2–§2.7 is sufficient: five thousand-odd unit variants
and five exceptions, all of them declared in one side table rather than
special-cased in a depiction.

The name — a *pantry* of *ingredients* applied by *coating* — is doing real
work: the exception is a property of the mesh, so it travels with `CoatingName`
and the generated depiction file never has to know about it.

---

## 3. So what actually publishes the numbers?

**[inferred, but strongly constrained.]** The operators read *named properties*
and never call into the simulation. The vocabulary of published names is small
and completely regular:

| property | written when |
|---|---|
| `tourelleN`, `tourelleNaxe` | every frame — the turret's current two angles |
| `tourelleNtarget` | the turret has a target |
| `ShotImpulse_N` | turret *N* fired |
| `WeaponShootData_M_N` | weapon *N* of slot *M* fired (carries the shot) |
| `WeaponActiveAndCanShoot_N` | the weapon is live |
| `WeaponIgnored_N` | the weapon is masked out |
| `DamageRatio` | continuously |

So the simulation → depiction interface is **a flat property bag keyed by
string, written by the simulation and polled by the operators.** No events, no
callbacks, no depiction pointer in a unit.

The virtues are the ones you would expect and one you might not:

- An operator can be added, removed or disabled (§5) with no simulation change.
- The same properties feed **sound** (`DepictionOperator_MovementSound_*` is in
  the same `Operators` list) and **FX** (§3.1), so a new consumer is a new
  operator.
- **A remote unit in a multiplayer game needs no separate animation path.** It
  publishes the same properties from replicated state and the same operators
  run. This is the same conclusion `nuclear_option_control.md` §1 reaches from
  the opposite direction — one input struct, everything writes it — and
  `networked_animation_physics.md` §2 states as the general rule: animation
  replicates as *intent*, never as pose.

### 3.1 Actions are the other half, and they are not operators

**[BUILD-WARNO]** `Actions` is a `MAP` from event tag to a *happening* —
Eugen's word for a spawned FX/sound graph:

```
Actions = MAP[ ( [ "weapon_effet_tag1" ], Weapon_HMG_12_7_mm_M2HB ) ]
        + DepictionAction_Stress_And_Wrecked
        + DepictionAction_MovementFX_Wheeled
        + DepictionAction_CriticalFX_Tank
```

and the maps compose with `+`, so a unit's action set is assembled from shared
role fragments. Each happening names an **anchor bone**:

```
DepictionAction_MovementFX_Tracked is MAP[
    ( ['fx_deplacement'], TCompositeHappening( SubHappenings = [
        TIntroduceMobileHappening( Anchor = "fx_fumee_chenille_g1" ... ),
        TIntroduceMobileHappening( Anchor = "fx_fumee_chenille_d1" ... ),
    ]))
]
```

**The split is: operators move bones, actions attach things to bones.** Damage
smoke goes on `fx_stress_01`/`02`, engine damage on `fx_moteur`, ammunition
cook-off on `fx_munition`, a crashing helicopter picks one of five random crash
FX. The mesh must expose that anchor vocabulary, which is the price, and it is
why `DepictionAction_CriticalFX_Tank` can be a shared object bolted to every
tank in the game.

---

## 4. Soldiers on and around vehicles

This is the part the question above usually means, and Eugen's answer is
unusually clear-headed.

### 4.1 A sub-depiction is a whole depiction on an anchor bone

**[BUILD-WARNO]**

```
template SubDepiction_Driver [ MeshDescriptorHigh, MeshDescriptorLow ]
is TSubDepiction
(
    Anchors   = ['driver']
    Depiction = TemplateDepictionDriver( MeshDescriptorHigh = <...> MeshDescriptorLow = <...> )
)
```

A `TSubDepiction` is `(Anchors, Depiction)` — a *complete* `TDepictionTemplate`,
with its own mesh set, its own LOD selector and its own operator list, parented
to one or more named bones of its host. It nests arbitrarily: a towed AA gun's
sub-depiction has its own sub-depiction for the missiles on its rails, attached
to eight anchors at once (`aa_1_1` … `aa_1_8`).

**[inferred]** This is an attachment graph, not a merged skeleton — the opposite
of TF2's bonemerged `c_model` cosmetics
([`source_fps_viewmodel.md`](../../games/valve/source_fps_viewmodel.md) §8), and correct for the
same reason it is wrong there: TF2 needs a hat to deform with the head; a driver
does not need to deform with the Humvee. One transform is enough, and it means
every crew figure is independently cullable.

### 4.2 The anchor vocabulary is six names, for the whole game

**[BUILD-WARNO]** Across 2,873 NDF files, the human anchor names are:

```
'driver'    'tireur'    'tireur_g'    'tireur_d'    'servant_g'    'servant_d'
```

Driver, gunner, gunner-left, gunner-right, crewman-left, crewman-right. Plus
`remorque_1` for a trailer hitch. That is the entire set.

**[inferred]** So a modeller does not get to invent crew positions. The mesh
either exposes an anchor from the closed list or it has no crew there. That is
the deliberate trade — you lose the ability to put a fifth man on the engine
deck, and you gain the fact that `SubDepiction_Driver` is one shared object that
works on 129 different vehicles.

### 4.3 The crew of a moving vehicle are statues, and that is the whole trick

**[BUILD-WARNO]** Here is the driver's depiction:

```
template TemplateDepictionPose [ MeshDescriptorHigh, MeshDescriptorLow, Selector, Animation ]
is TDepictionTemplate
(
    ShadowLessInitialValue = true
    Selector = <Selector>
    DepictionAlternatives = [
        TDepictionDescriptor( SelectorId = [LOD_High] MeshDescriptor = <MeshDescriptorHigh> ),
        TDepictionDescriptor( SelectorId = [LOD_Low]  MeshDescriptor = <MeshDescriptorLow>  ),
    ]
    Operators = [
        TCosmeticFreezeSkeletalAnimationOperatorDesc( Animation = <Animation> )
    ]
)
```

**One operator, and it is `Freeze`.** The driver is a skinned mesh posed by a
single frame of `DriverStand` and then never touched again. The gunner gets
`GunnerSittingDown`. The helicopter pilot gets `PilotStand`. They do not
breathe, look around, or react.

`ShadowLessInitialValue = true` — and they do not cast shadows either.

**[inferred]** This is the correct answer and it is worth saying plainly. A
crew figure's job is to make the vehicle read as *crewed*; that job is done by
the silhouette. Animating him costs a skinning pass per figure per frame across
several hundred vehicles and buys motion nobody can see, because the figure is a
metre tall on a 2,000 m map and is *attached to a vehicle that is already
moving*. Freezing him keeps the entire benefit and pays only the skinning of a
static pose — which, if the engine is smart, it can bake once. The give-away
that this was a considered decision rather than an oversight is that the
machinery to animate him **is right there and used elsewhere** (§4.4).

The variants encode the only state that matters — whether he is visible:

```
HumanAlwaysVisibleSelector      DisplayCondition = nil
HumanVisibleWhenAliveSelector   DisplayCondition = TVisibleWhenAliveCondition()
HumanVisibleWhenDeployedSelector   TVisibleWhenAliveAndDeployedCondition()
HumanVisibleWhenUndeployedSelector TVisibleWhenDeadOrUndeployedCondition()
```

A gun crew is visible when the gun is deployed and gone when it is limbered.
That is four conditions doing the work of a transition.

**Counts.** `GeneratedDepictionHumans.ndf` holds **272 crew lists**, every one of
them referenced from `GeneratedDepictionVehicles.ndf` — so of the 572 ground-unit
depictions (which includes the towed guns), 272 carry visible crew at all, and
between them they hold **721 figures**:

| | count | animated? |
|---|---|---|
| `SubDepiction_ATGMServantRight/Left` | 334 | **yes** — 11 clips |
| `SubDepiction_ServantRight/Left` (+WalkOnly) | 203 | **yes** — 11 clips |
| `SubDepiction_Driver` (+Undeployed) | 130 | **no** — frozen |
| `SubDepiction_Gunner*` | 54 | **no** — frozen |

**The split is not by importance, it is by whether the thing they are attached
to is moving.** Servants crew towed guns and ATGM tripods — things that sit
still, deploy, reload and fire, where a static figure looks dead. Drivers and
vehicle gunners ride something that is already in motion, where a static figure
looks fine.

### 4.4 The servants animate, and it is a fixed slot list

**[BUILD-WARNO]** `DepictionAnimation.ndf`:

```
template DepictionOperator_AnimationServant
[ AimAnimation, DeadAnimation, DeployAnimation, FireAnimation, FoldAnimation,
  IdleAnimation, MoveBackAnimation, MoveFrontAnimation, MoveLeftAnimation,
  MoveRightAnimation, ReloadAnimation ]
is TCosmeticSkeletalAnimationServantOperatorDesc
(
    OperatorId         = 'AnimationServant'
    TransitionDuration = 0.2
    ...
)
```

**Eleven named slots and a 0.2 s crossfade.** No graph, no tags, no blend space
— eleven fields, filled twice (left crewman and right crewman) for the cannon
set and twice more for the ATGM set. Four objects cover 537 figures.

Compare [`source2_animation.md`](../../games/valve/source2_animation.md): Valve's Citizen player
graph is 2.2 MB of KV3 with **399 transitions** and 226 clips. A WARNO gun
crewman is eleven strings. Both are correct for their camera.

### 4.5 Transported infantry: ten slots, still frozen

**[BUILD-WARNO]** Riders are a *generator*, not an authored list:

```
template TransportedInfantrySubGenerator [ Mesh ] is TTransportedInfantrySubDepictionGenerator
(
    ReferenceMesh = <Mesh>
    Soldiers      = TransportedSquad     // ten entries, Index = 1..10
)
```

and each slot is:

```
template TransportedInfantryDepiction [ Index, Alternatives, Counts ] is TDepictionTemplate
(
    Selector = TTransportedInfantrySelector
    (
        Index = <Index>
        Counts = <Counts>
        HighNoneLimitInMeter = LodHighLowLimit_Infantry     // 200 m — see §5
        ...
    )
    Operators = [ TCosmeticFreezeSkeletalAnimationOperatorDesc( Animation = $/GFX/DepictionResources/FusilierTransport ) ]
    DepictionAlternatives = <Alternatives>
        + [ DepictionDescriptor_LOD_High( MeshDescriptor = $/GFX/DepictionResources/FuldaSoldier ) ] //pour avoir un 'high' pour le skeleton
)
```

Ten fixed slots. One frozen pose, `FusilierTransport`. `HighNoneLimit` rather
than `HighLow` — **there is no low-detail rider; past 200 m he is simply gone.**
128 of the 572 vehicles use the generator.

The trailing comment ("to have a 'high' for the skeleton") is a real
implementation wart worth recording: the alternatives list has to end with a
concrete mesh even when the selector may pick none of them, because the skeleton
is taken from the high entry. That is the kind of thing that only shows up in
shipped data.

**[inferred]** The generator exists because riders are *combinatorial* — any
squad in any transport — so authoring `HumanSubDepictions_<vehicle>` for them
the way §4.1 does for the driver would be a cross product. The driver is fixed
and authored; the riders are variable and generated. The line is drawn exactly
where the combinatorics start.

**This is new since 2014.** Red Dragon's class table has
`TMissileCarriageSubDepictionGenerator` and no transported-infantry equivalent,
so visible riders arrived with WARNO.

### 4.6 Infantry proper: tag sets, a 2D blend space, and a hand-listed torso mask

**[BUILD-WARNO]** The third and most expensive animation system, for
dismounted infantry. `TCosmeticSkeletalAnimation2OperatorDesc` holds a flat list
of `TAnimationWithTags`:

```
TAnimationWithTags( Animation = TSimpleBipedAnimation( Resource = FusilierAimingMMG ... ) TorsoOnly = true Tags = ['aiming','mmg'] ),
TAnimationWithTags( Animation = TSimpleBipedAnimation( Resource = FusilierFireMMG   ... ) TorsoOnly = true PlayTillEnd = true Tags = ['aiming','shoot','mmg'] ),
TAnimationWithTags( Animation = TAlternativeBipedAnimation( Resources = [FusilierCrouch, FusilierCrouch2, FusilierIdle, FusilierIdle2] ... ) Tags = ['stand'] ),
RunAnimation( Tags = ['run'] ),
RunAnimation( Tags = ['mmg','run'] ),
```

Roughly 25 entries. **[inferred]** Selection is by tag-set match — the runtime
holds a set like `{aiming, shoot, mmg}` and picks the best-matching entry —
which is why `['run']` and `['mmg','run']` can both exist and why the weapon
class is a tag rather than a parameter. Three composites do the rest of the
work:

- `TBlendSpace2DBipedAnimation( Front, Back, Left, Right, IsLegCycle = true )`
  — a four-corner locomotion blend space, with **three of them stacked** in
  `RunAnimation` so a squad's members do not run in lockstep.
- `TAlternativeBipedAnimation( Resources = [...] )` — pick one at random. Idles
  and deaths are five-deep.
- `TorsoOnly = true` + `PlayTillEnd = true` — the upper-body layer.

And the mask is not computed:

```
UpperBodyBoneList is ['bip01 pelvis', 'bip01 spine', 'bip01 spine1',
        // haut du corps
        'bip01 neck', 'bip01 head', 'bip01 l clavicle', ... 'arme_1', 'arme_2',
        ... 'fx_tir_01', 'fx_tir_02', ]
```

**Fifty bone names typed out in a data file**, including the weapon attachment
points and the muzzle-flash anchors — because the gun has to travel with the
torso. Compare Source 2, which ships a *graded* vocabulary
(`Blend_UpperBody_HalfArms`, `ReduceBonesBy50Percent`) precisely because a hard
cut at a joint is where layering looks wrong
([`source2_animation.md`](../../games/valve/source2_animation.md) §5). WARNO's is binary. At 200 m
it does not matter, and past 200 m the soldier is a low-poly mesh anyway.

There is one comment that reads like a scar:

```
// SwappingDuration donne la durée de la présente du tag de swap, sachant que
// les infos gameplay auront toujours la prio. On ne dispose que du temps de
// visée pour swapper l'arme, donc si le temps de visée est + court que le
// temps de swap, l'animation va glitch
SwappingDuration = 1.0
```

"Gameplay info always has priority… we only have the aim time to swap the
weapon, so if the aim time is shorter than the swap time the animation will
glitch." That is the honest statement of the whole architecture's cost: the
depiction is a *slave*, it cannot ask the simulation to wait, and where the
simulation is faster than the animation the animation loses.

### 4.7 Which soldier fires, and what he is holding

**[BUILD-WARNO]** Three fields on `TMountedWeaponDescriptor` connect a weapon to
a body:

```
TMountedWeaponDescriptor
(
    Ammunition            = $/GFX/Weapon/Ammo_HMG_12_7_mm_M2HB
    AnimateOnlyOneSoldier = False
    HandheldEquipmentKey  = 'MeshAlternative_3'
    NbWeapons             = 1
    EffectTag             = 'FireEffect_HMG_12_7_mm_M2HB'
    WeaponShootDataPropertyName = ["WeaponShootData_0_3"]
    ...
)
```

- **`AnimateOnlyOneSoldier`** appears **2,568 times** — on every mounted weapon
  in the game. A four-man squad firing Sterlings (`NbWeapons = 4`) has it
  `False`: all four play the fire animation. The same squad's single LAW has it
  `True`: one man plays it and the other three keep doing what they were doing.
  It is the answer to "the whole squad just fired one rocket in unison", which
  is the classic bug in this genre, and it is one boolean per weapon.
- **`HandheldEquipmentKey = 'MeshAlternative_N'`** selects the weapon mesh in
  the soldier's hand, through `SoldierDynamicWeaponSubDepictionSelector`
  (anchor `arme_1`) and its backpack twin (anchor `arme_2`). So a rifleman with
  a rifle slung and a LAW in hand is two sub-depictions and a mesh-alternative
  index.
- **`InitialSoldiersToTurretIndexMap`**, on
  `TInfantrySquadWeaponAssignmentModuleDescriptor`, says who mans what at
  spawn:

  ```
  InitialSoldiersToTurretIndexMap = MAP[
      (0,[0,]), (1,[0,]), (2,[0,2,]), (3,[0,1,]),
  ]
  ```

  Soldier → list of turret indices. Soldiers 0 and 1 carry only weapon 0;
  soldier 2 also carries weapon 2; soldier 3 also carries weapon 1. 291 squads
  have one. **[inferred]** It is *initial* because casualties reassign it —
  which is exactly why it is a map and not a rule.

### 4.8 The rule that falls out

**Three animation systems in one game, and the choice is made by count and by
whether the anchor is moving.**

| what | how | why |
|---|---|---|
| vehicle crew, riders | **one frozen frame** | hundreds of them, on something already in motion |
| towed-gun servants | **11 named clips, 0.2 s crossfade** | dozens, stationary, and the reload *is* the feedback |
| dismounted infantry | **~25 tagged clips, 2D blend space, torso mask** | the thing the player actually watches |

That is [`crowd_scale.md`](../scale/crowd_scale.md)'s expensive-near / cheap-far bet
applied not to distance but to **role**, and it is decided once at authoring
time rather than per frame.

---

## 5. LOD is an operator blacklist, and the distances are small

**[BUILD-WARNO]** This is the sharpest idea in the whole system.
`TemplateDepiction.ndf`:

```
private DisabledOperatorMid is [
    'weapon_effet_tag1_physic_fire_effect', ... 'weapon_effet_tag4_physic_fire_effect',
    'airplane_parts',
    'ikchain1',
    'WaterRotors',
]

private DisabledOperatorLow is DisabledOperatorMid + [
    'chassis',
    'cannon_hydraulic_recoil',
    'cannon_rocking_recoil',
    'GroundPuff',
    // 'rotary_cannon',
    'Tracks',
    'Wheels',
]
```

and the LOD descriptor carries them:

```
template DepictionDescriptor_LOD_Low [ MeshDescriptor, ... ]
is TDepictionDescriptor
(
    SelectorId        = [LOD_Low]
    MeshDescriptor    = <MeshDescriptor>
    DisabledOperators = DisabledOperatorLow + <DisabledOperators>
)
```

**The mesh LOD and the animation LOD are the same object, and the animation LOD
is a list of `OperatorId` strings to skip.** That is why `OperatorId` exists at
all (§2.3) and why it is a separate concept from the turret `Tag`.

Read the low list: past the mid/low boundary a vehicle stops rocking on its
suspension, stops recoiling, stops rolling its tracks and stops steering its
wheels. **The turret keeps aiming** — it is not on the list, because a turret
pointing the wrong way is legible at any distance and a still suspension is not.
`rotary_cannon` is commented out, meaning somebody disabled gatling spin at low
LOD, looked at it, and put it back.

The distances, from `DepictionSelectors.ndf`, with the pre-buff values still in
the comments:

| | high→mid | mid→low | low→none |
|---|---|---|---|
| vehicle | **200 m** *(was 100)* | **400 m** *(was 200)* | — |
| airplane / helicopter | 200 m *(was 100)* | 400 m *(was 200)* | — |
| infantry | 200 m (high→low) | — | **1,200 m** |
| missile on a rail | — | — | 400 m *(was 100)* |
| pylon | — | — | 200 m *(was 100)* |
| **a soldier's weapon** | — | — | **40 m** |

**`LodHighNoneLimit_SoldierWeapon is 40`** is the number to remember. The rifle
in a soldier's hands — the sub-depiction on `arme_1`, the one whose bone is in
the torso mask — is only drawn within forty metres of the camera. Everything
about §4.6 and §4.7, the mesh alternatives, the swap timing, the fifty-bone
mask, exists to be correct inside a forty-metre bubble.

**[inferred]** And note what the doubling in the comments says: these limits
were all raised by 2× at some point after they were first set, which is a GPU
budget being spent as hardware moved, in the one place where spending it is a
single number.

Selection is by **metres to camera** through a shared
`CameraMoverManagerModernWarfare`, not by projected screen size, and it is
additionally clamped by `$/GraphicOption/ModelQuality`. **[inferred]** Distance
rather than screen size is the right call for a game with a fixed vertical FOV
and no cinematics — it is one subtraction, it is stable under camera pitch, and
it means the LOD of a unit does not change when the player zooms the UI.

---

## 6. R.U.S.E. 2010 — the same decomposition, none of the same names

**[BUILD-RUSE]** R.U.S.E.'s class table has **no `TDepictionTemplate`, no
`TSubDepiction`, and not one `TCosmetic*` class.** The depiction architecture is
a post-R.U.S.E. rewrite.

What it has instead is a family of `TGfxDescriptor*` objects — and every one of
WARNO's cosmetic operators has an ancestor there doing the same job:

| R.U.S.E. 2010 | WARNO 2022 | job |
|---|---|---|
| `TGfxDescriptorTourelle` | `TCosmeticTurretOperatorDesc` | turret |
| `TGfxDescriptorChassis` (+`_Operator`) | `TCosmeticChassisPortingDesc` | suspension |
| `TGfxDescriptorChenille` / `ChenillePack` | `TCosmeticCaterpillarTrackOperatorDesc` | tracks |
| `TGfxDescriptorRoue` / `RouePack` | `TCosmeticWheelsOperatorDesc` | wheels |
| `TGfxDescriptorHelice` / `AvionPack` | `TCosmeticRotorOperatorDesc` / `PropellerOperatorDesc` | rotors |
| `TGfxDescriptorAtterrissageBoneOperatorDispatcher` | `TCosmeticLandingGearOperatorDesc` | undercarriage |
| `TGfxDescriptorBoneOperator` | `TBoneProceduralAnimation` | the generic one |
| `TGfxDescriptorVisibilityBoneOperator` | *(the `DisplayCondition` selectors)* | show/hide a part |
| `TGfxDescriptorModeleSousMobile` | `TSubDepiction` | a child model on a bone |
| `TGfxDescriptorHook` | *(anchors)* | attach point |

**The decomposition did not change. The vocabulary did.** This is
[`ruse.md`](../../games/strategy/ruse.md) §11.1's "652 classes survive verbatim" seen from the other
side: where a class *did* get renamed, it kept its responsibilities exactly.

The property lists are the interesting part, because they are richer than
WARNO's in two places and poorer in one.

**The generic bone driver, 2010:**

```
TGfxDescriptorBoneOperator
    SousMobileName  Operation  Axe
    Debattement_Max  Debattement_Min
    Elastic  Attraction  Amortissement
```

Bone, operation, axis, travel limits, and **a spring: elasticity, attraction,
damping**. WARNO's `TBoneProceduralAnimation` replaced the spring with
`Delay`/`Duration`/`LimitValue` — a *scripted* move rather than a *simulated*
one. **[inferred]** A trade, not an upgrade: the spring is better for something
reacting continuously, the timeline is better for a bomb-bay door, and by 2022
the reactive cases had all been given their own named operators.

**The chassis, 2010:**

```
TGfxDescriptorChassis
    NameBone
    Coef_Sur_Pente_Relation_Acceleration     Coef_Sur_Roulis_Relation_Rotation
    Coef_Sur_Pente_Relation_Terrain          Coef_Sur_Roulis_Relation_Terrain
    Coef_Bruit_Sur_Ressort  Coef_Bruit_Sur_Pente  Coef_Bruit_Sur_Roulis
    Operator_Translate_Vertical  Operator_Rotate_Pente  Operator_Rotate_Roulis
```

Pitch and roll each get **two separate causes** — acceleration/steering, and
terrain — with independent coefficients, plus three noise gains, plus three
sub-operators each of which is its own spring
(`TGfxDescriptorChassis_Operator { Debattement_Min, Debattement_Max, Attraction, Amortissement }`).

WARNO's `TGfxDescriptorChassis` (§2.7) **kept the class name and replaced the
entire schema** with `SpringX/Y`, `DamperX/Y`, `NoiseGridSize/Height`, `Force`.
Eleven cause-specific coefficients became four spring constants and a noise
grid. **[inferred]** Somebody decided the causes were not separable in practice
and that a spring driven by one aggregate input looked the same. Note which
direction that goes: **the 2010 model is the more elaborate one.**

**The tracks, 2010, are the surprise:**

```
TGfxDescriptorChenille
    RoueNb  RouePartieSolMin  RouePartieSolMax
    RoueNameRotate  RoueNameElev
    PartieHauteDeformName  PartieHauteDeformNb  Cote

TGfxDescriptorChenillePack
    ForceDeformationPartieHaut
    ForceBruitRoulementHaut  ForceBruitRoulementBas
    ForceCahotMin  ForceCahotMax  CahotsMax
    LongueurCahotMin  LongueurCahotMax
    IntervalleCahots  DelaiPropagation  FrequenceCahots
    ListeChenille
```

`RouePartieSolMin`/`Max` is the same idea as WARNO's
`FirstWheelOnGround`/`LastWheelOnGround` — which wheels are raised. Two separate
bone-name prefixes, one for the wheel that *rolls* and one for the wheel that
*travels vertically*. `PartieHauteDeform*` is the sag of the upper track run.

And then `*Cahot*` — a *cahot* is a jolt. Minimum and maximum jolt force,
maximum concurrent jolts, jolt length range, interval, frequency, and
**`DelaiPropagation`**: a propagation delay. **A bump enters the track at one
end and travels along it.** In 2010, on a game whose camera is usually five
hundred metres up. That level is gone from WARNO's four-field track operator.

**The wheel, 2010** — `NameBone`, `CoefRotate`, `CoefElev`,
`CoefRotateDirection`, `AngMaxRotateDirection`, `AngParSecondeRotateDirection`
— has a **steering rate and a steering limit per wheel**, where WARNO has one
boolean (`RotateDirection`) for the whole vehicle.

**The propeller, 2010**, has two speed regimes (`...MaxLS` / `...MaxHS`), each
with its own min and max prop RPM against a max airspeed, plus a spin-down
time. WARNO's `THelix` has one `RotationSpeed`.

**The skeleton mask, 2010:**

```
THierarchicalASEModelSkeletonMask
    MaskUnionCode  DefaultWeight  Weights  AnimationSlot
```

Per-bone **weights**, a default, a union code and an *animation slot* — that is
a graded, layered mask with slots, i.e. the thing
[`source2_animation.md`](../../games/valve/source2_animation.md) §5 argues you need. WARNO's
`UpperBodyBones` is a flat list of fifty names (§4.6). **The mask got simpler,
not richer**, which given a 40 m weapon-draw distance is a defensible call and
is still a capability that was removed.

**And the turret schema did not change at all.**

| property | R.U.S.E. 2010 | Red Dragon 2014 | WARNO 2022 |
|---|---|---|---|
| `Tag` | ✔ | ✔ | ✔ |
| `VitesseRotation` | ✔ | ✔ | ✔ |
| `AngleRotationMax` | ✔ | ✔ | ✔ |
| `AngleRotationBase` | ✔ (one-axis) | ✔ | ✔ |
| `AngleRotationBasePitch` | ✔ | ✔ | ✔ |
| `AngleRotationMaxPitch` | ✔ | ✔ | ✔ |
| `AngleRotationMinPitch` | ✔ | ✔ | ✔ |
| `MountedWeaponDescriptorList` | ✔ | ✔ | ✔ |
| `NbFX` | ✔ | ✔ | — |
| `TagIndex` | — | ✔ | → `YulBoneOrdinal` |
| idle behaviour | — | `UnitIdleManagerDescriptor` | `TurretIdleBehaviourDescriptor` |
| `OutOfRangeTrackingDuration` | — | — | ✔ |

Eight properties describing "what can this turret point at, and how fast", set
down in 2010 and still there in 2022 across two engine rewrites. **[inferred]**
That is not inertia — it is that the question genuinely has eight answers, and
the additions since are all about *behaviour when idle or losing a target*
rather than about the mechanism.

`TGfxDescriptorTourelle` itself is `SousMobileName`, `InfoAxe1`, `InfoAxe2`,
`InfoAxe3`, `SousElements` — **three axes in 2010**, against WARNO's two, plus a
child list for nesting. 619 of them in the database against 251 gameplay turret
descriptors, i.e. roughly 2.5 visual turrets per logical one, which is what you
would expect when a mesh has decorative mounts a weapon does not use.

---

## 7. Red Dragon 2014 — the middle point, and where the graph died

**[BUILD-WRD]** Red Dragon has the depiction architecture: `TDepictionTemplate`
(with `DepictionAlternatives`, `Selector`, `Actions`, `Operators`, `Constants`,
`SubDepictions`, `SubDepictionGenerators` — the same seven fields WARNO has),
`TSubDepiction`, `TDepictionDescriptor`, and **16 `TCosmetic*` operator classes
against WARNO's 48.**

Four differences are worth the space.

**7.1 The turret operator already had its final shape — and one field too many.**

```
TCosmeticTurretOperatorDesc            (2014)
    OperatorId  ZAxisNode  YAxisNode
    ZAxisPhysicalPropertyName  YAxisPhysicalPropertyName
    MaxYAngle  MinYAngle
```

Identical to WARNO's except for `MaxYAngle`/`MinYAngle` — elevation limits *on
the depiction side*, duplicating `AngleRotationMaxPitch`/`MinPitch` on the
gameplay descriptor. WARNO deleted them. **[inferred]** Two sources of truth for
one constraint, and the one that could disagree with the simulation is the one
that went.

**7.2 An animation *tree* keyed by AI states became a flat list of tag sets.**

```
TDepictionAnimationTreeNode  (2014)
    Childs  Animations  IAStates
```

A tree, where each node holds animations and the AI states that select it. That
class is gone from WARNO, replaced by `TAnimationWithTags` (§4.6) — a flat list
matched by tag set.

**[inferred]** This is the interesting one, because it is the *opposite* of the
direction Valve went. Source 2 answered "which pose now?" by building a bigger
graph — 399 transitions in the Citizen. Eugen had a graph in 2014 and threw it
away for a set-membership test. Both are right for their problem: a tree's value
is precise control over *transitions*, and transitions are exactly what nobody
can see at 200 m. What a flat tagged list buys instead is that adding a weapon
class is one tag rather than a new subtree everywhere, and
`source2_animation.md` §7's practitioner trap — *transition order is creation
order, so the most specific transition created last never fires* — cannot
happen, because there is no order.

**7.3 `TCosmeticTorsoTwisterOperatorDesc` — gone.**

```
TCosmeticTorsoTwisterOperatorDesc  (2014)
    OperatorId  OperatedBone
```

A dedicated operator to twist a soldier's torso toward his aim point. WARNO
does the same job with a `TorsoOnly` aiming clip over a `UpperBodyBones` mask.
**[inferred]** A procedural correction replaced by an authored pose — the
reverse of the usual direction, and consistent with the mask getting simpler:
if you only draw the weapon inside 40 m, an authored aim pose is both cheaper
and better than a solved twist.

**7.4 `Anchor` became `Anchors`.**

`TSubDepiction` had one anchor in 2014 and a list in 2022. That single
pluralisation is what lets one authored missile depiction instance across
`aa_1_1`…`aa_1_8` (§4.1) instead of eight near-identical sub-depictions.

Also present in 2014 and absent from WARNO's vocabulary:
`TCosmeticHingeOperatorDesc` (bone + axis — the ancestor of
`TBoneProceduralAnimation`), `TCosmeticMvtCarouselOperatorDesc` (which became
the two radar operators), `TCosmeticRotorPortingDesc`, `TCosmeticHaloDesc`.
`TTurretSkeletonModuleDescriptor { ControllerName, MeshDescriptor }` has no
WARNO counterpart at all.

---

## 8. Broken Arrow 2025 — the same shape, on Unity, assembled at spawn

Different studio, different engine, no shared code, and the architecture is
recognisably the same. This is the strongest single instance of
[`broken_arrow.md`](../../games/flight/broken_arrow/broken_arrow.md) §10's "the structures converged anyway"
reading.

**8.1 The `AnimationHub` is the operator list.**

**[TRACE-BA]** From the shipped logs — these ran:

```
BrokenArrow.Client.Ecs.Animations.AnimationHub:AnimationStart(IAnimationBehaviour[]&)
BrokenArrow.Client.Ecs.Animations.AnimationHub:SetState(AnimatorHubState)
BrokenArrow.Client.Ecs.AnimationBehaviors.AnimatorConnect:OnStart(IAnimationHubData)
BrokenArrow.Client.Ecs.AnimationBehaviors.AircraftTrails:OnStart(IAnimationHubData)
```

**[BUILD-BA]** and around them: `AnimationHubComponent`, `AnimationHubSystem`,
`AnimationHubData`, `animationBehaviours`, `MergeInternalAnimationHub` with a
local function `MergeAnimationBehaviours`.

An array of `IAnimationBehaviour`, each handed a shared `IAnimationHubData` at
start, driven by one `SetState(AnimatorHubState)`. **That is Eugen's `Operators`
list, a shared property bag, and a per-frame reset, arrived at independently.**
It is an ECS component and system, matching the audio finding in
[`broken_arrow_audio.md`](../../games/flight/broken_arrow/broken_arrow_audio.md) §5.2 — the client-side visual
layer is DOTS-shaped even though the note's §4.1 shows the simulation ECS is
`DefaultEcs`.

`MergeAnimationBehaviours` is the one thing Eugen have no equivalent for, and
§8.2 says why it is needed.

**8.2 Turrets are prefabs, and a unit is assembled at spawn.**

**[TRACE-BA]** The real spawn chain, read off a stack:

```
GameController:Update()
  EcsLoader:Update()
    <CreateUnitEcs>d__8:MoveNext()
      <CreateUnitModel>d__48:MoveNext()
        <InitView>d__51:MoveNext()
          UnitBuilder:LoadTurretsDynamically(UnitModelInfo, Turrets, Int32, String)
          UnitBuilder:UnpackRemainingTurretParts(GameObject, UnitModelInfo)
          UnitBuilder:InitUnitExtraModelByName(IReadOnlyCollection`1, String, UnitModelInfo, Transform)
          UnitBuilder:InitAbilities(Units, UnitModelInfo, Transform)
```

All `d__NN:MoveNext()` — UniTask coroutines, so **model construction is async
and spread across frames.**

And a real shipped warning, in 4 of the 49 logs:

```
Warning: Inside turret prefab DLC2_Leopard2A8_HMG(Clone) left some objects
Warning: Inside turret prefab DLC2_Leopard2A8_NoMG(Clone) left some objects
```

Two variants of one tank's turret — with and without a commander's machine gun
— as **separate Unity prefabs**, instantiated and unpacked into the unit's
hierarchy. **[inferred]** That is the structural difference from Eugen, and it
is driven by the product: Broken Arrow sells per-unit loadout customisation, so
a unit's *model* is not knowable at author time. Hence
`IsDynamicTurretOption`, `LoadTurretDynamically`,
`IntegrateDynamicTurretsFromOptionsEcs`, `IntegrateAdditiveTurret`,
`IntegrateNestedTurretAsBaseAddition`, `MergeWeaponsOnGroundTurrets`,
`CloneWithClonedWeaponsAndTurrets` — and hence `MergeAnimationBehaviours`,
because if the model is assembled then so is the behaviour list.

WARNO's `GeneratedDepictionVehicles.ndf` is a 1.1 MB file with one static entry
per unit, produced offline by a Python writer. Broken Arrow does the equivalent
at spawn, per unit, in a coroutine. **Same output; the difference is entirely
whether the loadout is fixed.**

**8.3 Twenty-one turret slots, unrolled.**

**[BUILD-BA]** `Turret0` … `Turret20` and `Turret0Id` … `Turret20Id` exist as
individual properties — 21 slots, flattened rather than a list, exactly as
Eugen unroll five turret operators (§2.2) and for the same reason. Alongside
them: `ParentTurretId` and `ChildTurrets` and `AddChildTurrets` (a hierarchy,
where WARNO has `MasterTurretYulBoneOrdinal`), `ParseTurretHierarchy` and
`FindTurretByHierarchy`, `TurretRotationSystem` and `TurretComponent` and
`UnitTurretLocalRotation` and `_turretBone`, `HorizontalTurretRotationSpeed`,
`SetTurretEnabled`, `TurretActiveWeaponIndex`.

**Twenty-one against Eugen's five** is the concrete measure of how much more
detailed a Broken Arrow unit is, and it is the same trade
[`battle_scale.md`](../scale/battle_scale.md) names: count × depth ≈ constant.

`ThrowTurret`, `RemoveTurretIfNotThrow` and `TurretFly` have no Eugen
counterpart — a destroyed tank's turret physically departs, which fits the
per-shell ballistics depth `broken_arrow.md` §7 identifies as the reason they
could not use lockstep.

`RestDatabaseTurretsData` / `RestDatabaseTurretUnitsData` /
`RestDatabaseTurretWeaponsData` and `TurretsJson` put the turret definitions in
the same REST-served balance database `broken_arrow_damage.md` §7 documents —
so a turret's traverse rate is a live JSON row, not a build.

**8.4 Seats, not passengers.**

**[BUILD-BA]** The vocabulary for people on vehicles is *seat*: `SeatData`,
`SeatIndex`, `SeatsCount`, `MaxSeats`, `CurrentSeats`, `FreeSeat`,
`TryGetFreeSeat`, `ContainerSeatInitializer`, `AddSeatsAndHeavyLift`,
`ignoreWeightSeats` — and, for the visual, **`SeatAnimation`, `AnimationSeats`
and `UnitSeatAnimation`.** There is no `Passenger`, `Crew`, `Gunner`,
`Occupant`, `Embark`/`Disembark` or `Dismount` identifier anywhere in the
203,203 names.

**[inferred]** So the model is: a container has N seats, a seat has an index, a
position and an animation. That is a richer model than Eugen's closed
six-anchor vocabulary (§4.2) — it is data rather than a fixed list — and it is
what you need if the vehicle roster is customisable.

Soldier animation is stock Unity `Animator`: `SoldierAnimationManager`,
`SoldierAnimationState(s)`, `SoldierAnimatorComponent`, `_soldierAnimators`,
`RefreshInfantryAnimatorState`, `SaveLastAnimatorState`, and — the animation-LOD
lever — **`EnableSoldiersAnimator` / `DisableSoldiersAnimator` /
`ChangeSoldiersAnimator`**. Switching the `Animator` component off is the Unity
spelling of WARNO's `DisabledOperators` (§5): the same idea, at coarser
granularity, because Unity's unit of animation cost is the component.

Also present: `Suspension`, `SuspensionElement`, `SuspensionSide`,
`SuspensionSpeed`; `BarrelElevation`, `BarrelClipping`, `Barrel1`…`Barrel4`;
`Recoil`, `RecoilWeaponTransform`, `RecoilDataDictionary`, `BodyRecoil`,
`BodyRecoilAmplitude`, `AnimateBurstRecoil`; `ArmorBone`, `BaseArmorBones` (the
four-facing armour model of `broken_arrow_damage.md` §2, hung off bones);
`DeadModel` (a wreck swap, matching WARNO's `UniteCadavreDescriptor` — **neither
game ragdolls a vehicle**).

**8.5 And no animation middleware at all.**

**[BUILD-BA]** No Animancer, no Rukhanka, no DMotion, no GPU-skinning package —
nothing. In a build that `broken_arrow.md` §2 shows buys ~90 packages including
*two* audio middlewares, animation is stock `Animator`, `SkinnedMeshRenderer`,
Playables, and in-house code.

**[inferred]** Which is the same conclusion `nuclear_option_audio.md` reaches
about audio and `nuclear_option.md` §16 states as a rule: **you buy the thing
that is generic and hard, and write the thing that is specific to your product.**
Streaming virtual texturing is generic. "How does a tank turret behave" is not,
and there is no package for it — because the answer is thirty lines of maths
plus a data schema, and the schema is the actual work.

---

## 9. Read side by side

| | R.U.S.E. 2010 | Red Dragon 2014 | WARNO 2022 | Broken Arrow 2025 |
|---|---|---|---|---|
| composition unit | `TGfxDescriptor*` tree | depiction + 16 operators | depiction + **48** operators | `AnimationHub` + `IAnimationBehaviour[]` |
| per-frame contract | *(unknown)* | `SetToPose` then apply | `SetToPose` then apply | `SetState(AnimatorHubState)` |
| sim → visual | `SousMobileName` | named physical properties | named physical properties | ECS component read |
| turret axes | 3 (`InfoAxe1..3`) | 2 | 2 | 2 (`Horizontal…`, `BarrelElevation`) |
| turret slots | — | — | 5 | **21** |
| nested turrets | `SousElements` | — | `MasterTurretYulBoneOrdinal` | `ParentTurretId` / `ChildTurrets` |
| turret angle limits | gameplay only | gameplay **and** depiction | gameplay only | REST DB |
| idle scan behaviour | — | `UnitIdleManagerDescriptor` | 10-field table, 5 presets | *(not found)* |
| chassis | 11 coefficients, 3 spring sub-ops | ported descriptor | 4 springs + noise grid | `Suspension*` |
| track detail | jolts propagating along the run | — | 4 fields | *(not found)* |
| bone mask | **weighted**, with slots | — | flat 50-name list | Unity avatar mask |
| pose selection | — | **animation tree** on AI states | **flat tag sets** | Unity `Animator` states |
| crew figures | `TGfxDescriptorBiped` | `TSubDepiction` (1 anchor) | `TSubDepiction` (N anchors) | seats |
| vehicle crew animate? | *(unknown)* | *(unknown)* | **no — one frozen frame** | `SeatAnimation` (exists) |
| riders on transports | — | — | generator, 10 slots, frozen | seats |
| animation LOD | `MaxMaskedAnimationCount` | — | **`DisabledOperators` by `OperatorId`** | `DisableSoldiersAnimator` |
| model assembled | offline | offline | offline (Python writer) | **at spawn, async** |
| exceptions | — | — | coating pantry, **5 entries** | prefab variants |

Three readings.

**One: the vocabulary converged on the same dozen operators, twice,
independently.** Turret, chassis, wheels, tracks, rotor, recoil, undercarriage,
attached figures, a generic bone driver, and a way to switch some of them off
with distance. Eugen reached it over three engine generations; Steel Balalaika
reached it in one, on Unity, with different words. That is the shape of the
problem, not a style.

**Two: the direction of travel is toward *fewer, coarser* knobs, not more.**
R.U.S.E.'s chassis had eleven cause-specific coefficients; WARNO's has four
springs. R.U.S.E.'s tracks propagated jolts; WARNO's do not. R.U.S.E.'s bone
mask was weighted; WARNO's is binary. Red Dragon's pose selection was a tree;
WARNO's is a flat tag list. **Every one of those simplifications is downstream
of a decision about what the camera can resolve**, and every one of them was
paid for with a capability. That is the honest version of "they got better at
it": they got better at knowing what not to build.

**Three: the two things that never simplified are the turret's angle schema and
the property-name interface.** Eight properties describing a turret's envelope,
unchanged across twelve years and two rewrites; and a flat bag of named floats
between simulation and visual, in all four builds. **[inferred]** Those are the
two interfaces that everything else hangs off, which is exactly the pattern
CLAUDE.md's derived-cache rule describes from the other side — get the boundary
right and the things on either side of it stay cheap to change.

---

## 10. What transfers here

Ranked by what this project could use, and against
[`nav_architecture.md`](../../plans/nav_architecture.md)'s test — does it stay a drop-in for
an RTS, an FPS and a third-person game?

1. **An ordered operator list over a reset pose, addressed by string id.** The
   whole architecture in one sentence, and it is genre-neutral: an FPS weapon,
   an RTS turret and a third-person character's procedural lean are all "apply
   these named functions to these named bones after resetting". The `OperatorId`
   is what makes §5 possible and is the field people leave out.
2. **Animation LOD as a list of operator ids to skip, attached to the mesh
   LOD.** One list, edited by an artist, with no code change and no per-operator
   distance check. This is the single most reusable idea in the note and it is
   maybe forty lines.
3. **Names for authoring, ordinals for the simulation** (§2.3). The depiction
   says `'tourelle_01'`; the simulation says `YulBoneOrdinal = 1`. Two handles on
   one bone because they have different hot-path requirements.
4. **The idle-behaviour table** (§2.5). Ten numbers per role turn a parked army
   from a diorama into an army, and cost a timer comparison. Directly applicable
   to this project's units the moment they have anything that points.
5. **Freeze the animation of anything attached to something already moving**
   (§4.3). Keep the mesh, drop the clip. It is a one-line operator and it is the
   reason 721 crew figures are affordable.
6. **`AnimateOnlyOneSoldier`** (§4.7). One boolean per weapon that prevents the
   entire squad playing the fire animation for a single rocket. The bug it
   prevents is subtle and universal, and it is a *data* fix.
7. **A generic bone driver with six fields** (§2.8) plus a **side table of named
   exceptions**. Five entries in the whole of WARNO. The lesson is that if the
   exception table is small, the vocabulary is right; if it is growing, a new
   operator is overdue.
8. **`RecoilPeak` — one scalar that splits a duration into fast-out and
   slow-back** (§2.6). Applies to any asymmetric one-shot motion; cheaper and
   more tweakable than a curve asset.
9. **A world-space noise grid instead of per-wheel ground sampling** (§2.7). Two
   vehicles differ, one vehicle is consistent over the same ground, cost is one
   lookup. Directly usable for this project's tile-grid vehicles if it ever has
   any.
10. **The `Tag`/`OperatorId`/property-name triangle as the sim↔visual
    interface.** Nothing holds a pointer to anything; a missing consumer is not
    an error. It is what makes the same code path serve a locally simulated and
    a replicated remote unit, which is
    [`networked_animation_physics.md`](networked_animation_physics.md)'s rule
    with a concrete implementation.

**Two anti-patterns, both visible in the diff:**

- **Duplicated constraints.** Red Dragon's `MaxYAngle`/`MinYAngle` on the
  depiction duplicated the gameplay descriptor's elevation limits; WARNO deleted
  them. If two objects can disagree about a limit, one of them is wrong on some
  unit and nobody will find out which.
- **A shared descriptor whose name has stopped being true.**
  `GfxDescriptorChassis_MediumTank_NoCanonRecoil` on a Humvee (§2.7). The
  six-descriptor economy is right; the naming is a trap for whoever tunes it
  next. Name shared tuning data after the *behaviour* it produces, not the first
  unit that needed it.

---

## 11. Reproducing this, and what is not established

No new tools. The three R.U.S.E./Wargame readers in
[`ruse.md`](../../games/strategy/ruse.md) §12 do the binary work; WARNO's mod workspace is plain
text; Broken Arrow needs the six-line IL2CPP identifier extract in
[`broken_arrow.md`](../../games/flight/broken_arrow/broken_arrow.md) §12.

Two small things were needed on top and are worth recording:

- **`ruse_ndf.py`'s object walker also throws `OverflowError`** on a bad array
  length, which `objects()` does not catch (it catches `ValueError` and
  `struct.error`). Fold `OverflowError`/`MemoryError`/`IndexError` into the
  resync path before iterating `everything.cpp.gladndfbin`.
- **`PROP` entries carry a trailing `uint32` that is the owning class index**, a
  fact the reader's own comment states and then discards. Reading it groups
  every property name under its class, which is how §6's and §7's schema tables
  were produced — with **no value decoding involved at all**, so they are as
  reliable as the string tables.

**Not established, and it matters:**

- **No R.U.S.E. or Red Dragon *values*.** §6 and §7 are schemas — class names
  and property names. The NDF value decoder is incomplete, so every number in
  those sections is absent rather than uncertain. Where a R.U.S.E. property
  *sounds* like it means something (`DelaiPropagation`), that is a reading of the
  name, and it is marked.
- **`TGfxDescriptorTourelle`'s `InfoAxe1/2/3` were not decoded.** The
  three-axis claim rests on three property names, not on three axes observed
  doing anything.
- **Runtime behaviour is inferred throughout for all four games.** Nothing here
  comes from instrumenting an executable or capturing a frame. That an operator
  list is evaluated in order, after a reset, once per frame, is a reading of the
  data and the French comment quoted in §2.1 — a strong reading, not an
  observation.
- **Broken Arrow's animation coverage is thin by comparison and the note should
  not pretend otherwise.** §8 is ~85% identifier names. Four log lines and one
  warning are the only **[TRACE]** evidence. `data.unity3d` (6.8 GB) still has
  not been opened, and it holds the prefabs, the `SeatData` and the actual
  `IAnimationBehaviour` implementations. AssetRipper is installed on this
  machine; that is the obvious next step and would move most of §8 from
  inference to evidence.
- **No costs.** Not one measurement in this note is a millisecond. The claims
  about *why* something is cheap (skinning dominates, a frozen pose avoids it, a
  noise lookup beats four raycasts) are reasoning from what the data does, not
  from a profile of these games.
- **The WARNO mod workspace is the gameplay half only.** Engine-side classes do
  not appear in it, so an absence from a WARNO column is not evidence of
  removal — that caveat from [`ruse.md`](../../games/strategy/ruse.md) §12 applies to every
  comparison here. Where §7 turns on WARNO's *absence* of a class it says so.
- **One identifier trap, recorded because it nearly landed.** `BladeCount` in
  Broken Arrow's table is Unity's *physical camera aperture* blade count, not a
  rotor. `AnimationTrack`, `ActivationTrack` and `CreateAnimationClipForTrack`
  are Timeline, not caterpillar tracks. The IL2CPP table is a flat namespace-less
  list of 203,203 names and it will happily confirm whatever you searched for.

---

## Sources

**Read directly**
- The retail WARNO install, `Mods\Test\{GameData,CommonData}` — 2,873 `.ndf`
  files, Eugen's own NDF source with comments. All **[BUILD-WARNO]** claims.
  Produced by the `GenerateMod.bat` in `Mods\Utils`; any WARNO install can make
  one.
  Principal files: `GameData/Gameplay/Gfx/Units/{DepictionOperators, DepictionActions, DepictionAnimation, DepictionCoatingPantry, SubDepiction, GfxDescriptorChassis, Gfx_UnitIdles, DepictionSelectors}.ndf`,
  `GameData/Gameplay/Gfx/Templates/TemplateDepiction.ndf`,
  `GameData/Generated/Gameplay/Gfx/{WeaponDescriptor, UniteDescriptor}.ndf`,
  `GameData/Generated/Gameplay/Gfx/Depictions/Generated*.ndf`.
- The retail Wargame: Red Dragon install,
  `Data\WarGame\PC\510064564\NDF_Win.dat` — `pc\ndf\patchable\gfx\everything.ndfbin`.
  All **[BUILD-WRD]** claims.
- The retail R.U.S.E. install, data version `190852` —
  `Data\PC\190852\ZZ_GladPatchableWin.dat`,
  `genglad\patchable\gfx\everything.cpp.gladndfbin`. All **[BUILD-RUSE]** claims.
- The retail Broken Arrow install, `BrokenArrow v.1.0.9.1` —
  `BrokenArrow_Data\il2cpp_data\Metadata\global-metadata.dat` and `GameLogs\`
  (49 logs). All **[BUILD-BA]** and **[TRACE-BA]** claims.

**Tools**
- [`../tools/ruse/ruse_edat.py`](../../../tools/ruse/ruse_edat.py),
  [`../tools/ruse/ruse_ndf.py`](../../../tools/ruse/ruse_ndf.py) — see §11 for the two
  adjustments needed.

**Related notes**
- [`ruse.md`](../../games/strategy/ruse.md) — the engine; §11 is the twelve-year diff this note
  extends into the animation layer
- [`broken_arrow.md`](../../games/flight/broken_arrow/broken_arrow.md) — §10's convergence argument, of which §8
  here is the strongest instance
- [`broken_arrow_damage.md`](../../games/flight/broken_arrow/broken_arrow_damage.md) — the REST balance database
  §8.3's turret data is served from; the four-facing armour §8.4's `ArmorBone`
  implements
- [`source2_animation.md`](../../games/valve/source2_animation.md) — the graph-based answer §7.2
  says Eugen abandoned
- [`rigging_ik.md`](rigging_ik.md) — the constraint and IK layer none of these
  four games has for vehicles
- [`lod_systems.md`](../world/lod_systems.md) — §5 is a fifth meaning of LOD: cost
  removed by disabling named systems rather than by simplifying geometry
- [`crowd_scale.md`](../scale/crowd_scale.md), [`battle_scale.md`](../scale/battle_scale.md) —
  count against depth, which §8.3's 21-versus-5 turret slots measures
- [`networked_animation_physics.md`](networked_animation_physics.md) — §3's
  property-bag interface is why a replicated unit needs no second path
