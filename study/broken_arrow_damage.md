# Broken Arrow — armour, penetration and suppression

The counterpart to [`nuclear_option_damage.md`](nuclear_option_damage.md), the
same way [`broken_arrow_audio.md`](broken_arrow_audio.md) is the counterpart to
the Nuclear Option audio note. Two games in the same engine, both resolving "this
weapon hit that unit, what happens", and they divide the problem almost exactly
opposite ways.

The one-sentence version, which §9 spends the rest of the note earning:

> **Broken Arrow models the projectile-armour interaction in enormous detail and
> the target categorisation crudely. Nuclear Option does precisely the reverse.**
> Broken Arrow has four armour facings × two damage types, range-interpolated
> penetration, two separate damage formulas, a critical-hit table and a
> suppression model — and decides whether a weapon may engage a target at all with
> **five boolean flags**. Nuclear Option has one scalar armour value per part and
> no facings at all — and decides engageability with a **five-element vector dotted
> with a four-element one**. Neither is wrong. The camera position explains both.

> **Method and its limits.** Broken Arrow is **IL2CPP**, so there is nothing to
> decompile. Everything here is read from the retail install: the IL2CPP metadata
> identifier table, the shipped `Manual/`, and the shipped `GameLogs/`.
>
> The caveat from [`broken_arrow.md`](broken_arrow.md) §4.1 governs everything
> below. **A name in the metadata proves it was compiled in, not that it runs.**
> Unlike the audio note I have no *bodies* to read here and no runtime traces of
> the damage path — so this note is almost entirely **[BUILD]** plus **[inferred]**,
> and the inferences are reconstructions from naming. Where I am reading a lot into
> a little, I say so.
>
> I have not played the game, and **no authored value was recovered** — §8 explains
> why that is structurally harder here than for Nuclear Option.

Related: [`nuclear_option_damage.md`](nuclear_option_damage.md) (the A/B),
[`broken_arrow.md`](broken_arrow.md) (the parent note, same install, same method),
[`broken_arrow_audio.md`](broken_arrow_audio.md),
[`ruse.md`](ruse.md) §11.4 (Eugen's dice-roll resolution, the genre's other
tradition), [`battle_scale.md`](battle_scale.md).

---

## 1. Armour is directional and typed

**[BUILD]** The clearest finding in the note, and it is unambiguous from the field
names alone:

```
KinArmorFront    KinArmorSides    KinArmorRear    KinArmorTop        ← kinetic
HeatArmorFront   HeatArmorSides   HeatArmorRear   HeatArmorTop       ← shaped charge
ArmorType   ArmorValue   ArmorSides   ArmorId   UnitArmors
GetArmorBySideAndType
GetArmorSideFromShotVector
GetMinArmor
SameArmorKinetic   SameArmorHeat
```

**Four facings × two damage types = eight armour values per unit.** That is the
Wargame / Steel Beasts / Combat Mission lineage, and it is what a player of this
genre expects: flanking a tank is a *mechanic*, not a flavour.

**[inferred] `GetArmorSideFromShotVector` is the detail that matters.** The facing
is computed from the actual incoming shot vector against the target's orientation,
not from a coarse relative-bearing bucket assigned when the shot was fired. So a
shell that arrives while the target is turning hits whatever facing is presented at
impact, and the flanking manoeuvre is resolved in the geometry rather than in a
targeting decision.

The kinetic/HEAT split is the standard one and it is the reason ERA and composite
armour can be expressed at all: a unit can be `KinArmorFront = 20` and
`HeatArmorFront = 40` and now HEAT weapons are the wrong tool against its front but
right against its sides. `SameArmorKinetic` / `SameArmorHeat` are **[inferred]**
convenience flags for units where the two are equal, so the data does not have to
be entered twice.

There is a duplicate spelling in the identifier table — `KinectArmorFront`,
`KinectArmorRear`, `KinectArmorSides`, `KinectArmorTop` alongside the `KinArmor*`
set. **[inferred]** Either a rename that left both, or a serialisation alias for old
data. Worth noting only because it is the kind of thing a spreadsheet-driven
pipeline (§7) produces and never quite cleans up.

---

## 2. Penetration is a curve over range, and there are two formulas

**[BUILD]**

```
PenetrationAtMinRange    PenetrationAtMinimalRange
PenetrationAtEffectiveRange
PenetrationAtGroundRange
Penetration   GetPenetration   ComputePenetration   Query_ComputePenetration
GetPenetrationForAmmunition   GetPenetrationForShell
SetPenetrationAndDamage
ARMOR_PENETRATION_EFFECTIVENESS
HE_ARMOR_EFFECTIVENESS
DamageFormulaKinetic
DamageFormulaHEAT
```

**[inferred] Three or four sampled penetration values per ammunition, interpolated
over range.** `PenetrationAtMinRange` / `PenetrationAtEffectiveRange` /
`PenetrationAtGroundRange` is the same authoring shape Eugen use in WARNO — the
designer enters penetration at a couple of reference ranges and the runtime
interpolates. It models the real thing (kinetic penetrators lose velocity and
therefore penetration with distance; shaped charges do not) without anyone
authoring a curve.

`PenetrationAtGroundRange` as a *separate* entry from effective range is
**[inferred]** most likely the air-to-ground case — the same ammunition fired from
an aircraft at a ground target, where the engagement range and the impact angle are
different enough to need their own number.

**`DamageFormulaKinetic` and `DamageFormulaHEAT` are separate.** So the two damage
types do not merely index different armour values, they run different arithmetic —
which is correct, because a kinetic penetrator that defeats armour does damage
proportional to its residual energy, while a shaped charge that defeats armour does
a roughly fixed amount of damage behind it. `HE_ARMOR_EFFECTIVENESS` is a third
path: high explosive against armour, handled by a global constant rather than by
penetration at all.

**[inferred] That three-way split — kinetic, HEAT, HE — is the standard tactical
wargame answer**, and it is a much more specific model than the four generic
channels (`pierce`, `blast`, `fire`, `impact`) Nuclear Option uses. Nuclear
Option's channels are *categories of harm*; Broken Arrow's are *mechanisms of
armour defeat*.

### 2.1 Top attack

```
DoesAmmunitionUseTopArmorAttack   DoesProjectileUseTopArmorAttack
IsTopArmorArmorAttack   TopArmorAttack   forceTopArmorAttack
BUILDINGS_TOP_ARMOR_AIMING_TRESHOLD_ANGLE
```

**[inferred]** Top attack is a property of the *ammunition* (a Javelin-style
munition declares it) and can also be forced. `BUILDINGS_TOP_ARMOR_AIMING_TRESHOLD_ANGLE`
(their spelling) is a separate rule for buildings — above some dive angle, a shot
at a building counts as hitting its top. Given `KinArmorTop`/`HeatArmorTop` are the
weakest facings on any vehicle, this is the mechanic that makes top-attack ATGMs
and dive-bombing worth their cost.

---

## 3. Hit resolution: a chance *and* a flown shell

**[BUILD]**

```
CalculateWeaponHitChance   CalculateMissileHitChance   HitChance   LastHitChance
LastCalculatedHitChance    GetTargetAccuracy   CalculatePrecision   CalculateTimeToHit
Dispersion   DispersionHorizontalRadius   DispersionVerticalRadius   DispersionMinimal
DispersionModifier   DispersionMultiplier   MajorDispersionMultiplier   MinorDispersionMultiplier
DispersionInfMultiplier   DispersionPlaneMult   GetDispersionReferenceRange
ARTILLERY_DISPERSION_CDF   ARTILLERY_DISPERSION_CORRECTION
MAX_WEAPON_AIMING_IMPRECISION   MIN_POINT_DISPERSION   MAX_POINT_DISPERSION_MULTIPLIER
IsPrecisionAmmo   IsPrecisionWeapon
```

alongside the ECS systems from [`broken_arrow.md`](broken_arrow.md) §4.2:
`ShootingSystem`, `BallisticShellSystem`, `HitDetectionSystem`,
`MissileGuidanceSystem`.

**[inferred] They do both things, and that is the interesting part.** The genre's
two traditions are *roll to hit* (R.U.S.E.'s tolerance test, WARNO's dice — see
[`ruse.md`](ruse.md) §11.4) and *fly the projectile* (Men of War, and Nuclear
Option, where a bullet is a simulated object that either intersects a collider or
does not). Broken Arrow has `CalculateWeaponHitChance` **and** `BallisticShellSystem`.

The most plausible reading, and I want to flag it as a reading: **the hit chance
and dispersion determine where the shell is aimed, and then the shell is flown to
that point.** `DispersionHorizontalRadius` / `DispersionVerticalRadius` describe a
scatter ellipse, `GetDispersionReferenceRange` scales it with range, and
`ARTILLERY_DISPERSION_CDF` — a *cumulative distribution function* — is how you draw
a correctly-distributed sample from that ellipse rather than a naive uniform or
box-Muller pick. Then `BallisticShellSystem` flies the round and
`HitDetectionSystem` resolves whatever it actually hits, which may not be the
intended target.

**[inferred] That combination gets you both properties.** The statistical model
gives designers a knob they can balance (`MinorDispersionMultiplier`,
`DispersionInfMultiplier` for infantry targets, `DispersionPlaneMult` for aircraft)
and gives the player a legible "this unit is accurate". The flown shell gives you
overshoots that hit something behind, shells that clip terrain, and visible tracer
that goes where the round went. Rolling alone cannot produce the first; flying alone
makes accuracy hard to author.

The modifier list is a good summary of what the game thinks affects a shot:

| Modifier | **[inferred]** what it models |
|---|---|
| `CounterMeasuresAccuracyMultiplier`, `CMAccuracyMultiplier`, `DecoyAccuracyMultiplier` | flares/chaff/decoys degrading a guided shot |
| `ECMAccuracyMultiplier` | jamming |
| `MissileAccuracyMultiplier`, `MissileAccuracyPlaneMult` | per-missile and against-aircraft accuracy |
| `LastMissileStressAccuracyMultiplier` | **suppression degrading the shooter's accuracy** (§6) |
| `DispersionInfMultiplier` | dispersion is different against infantry |
| `MagazineReloadTimeStressModifier` | suppression slowing reloads |
| `missileFlareResistivity` | per-seeker flare rejection |

**[inferred]** `missileFlareResistivity` is the same concept as Nuclear Option's
`flareRejection` ([`nuclear_option_combat.md`](nuclear_option_combat.md) §3.3) —
but there it is one term in a signal-versus-dazzle comparison built from aspect,
range, sun angle and angular separation, and here it is (as far as the naming
shows) a multiplier on a hit chance. **Same phenomenon, two orders of magnitude
apart in modelling depth**, and again the camera explains it: nobody in an RTS is
judging their flare timing.

---

## 4. Area effect, with two radii

**[BUILD]**

```
AOEDamage   AOERadius   DealAOEDamage   AOE_SEARCH_EXTRA_RADIUS
HealthAOEDamageModifier   HealthAOEModifier   HealthAOERadius
StressAOEDamageModifier   StressAOEModifier   StressAOERadius
SearchBuildingsForAOE   BuildingAOEHitInfo
BombDropOffsetAmmoAoeMult   SUPPLY_EXTRA_AOE_RADIUS
```

**[inferred] The detail worth taking is that health and stress have *separate* AoE
radii and modifiers.** A shell kills within one radius and suppresses within a
larger one, and both are authored per ammunition. That is exactly right — the
lethal radius of an artillery shell is a fraction of the radius over which it makes
you keep your head down — and it is the mechanic that makes suppressive fire a
distinct tactic rather than just weak damage.

Compare Nuclear Option's blast model
([`nuclear_option_damage.md`](nuclear_option_damage.md) §3): a single physically
scaled wave with overpressure decaying as `1/Z³`, gathered once and applied
progressively as the front expands, with a `60·mass` impulse cap. **[inferred]
Nuclear Option models the physics of one blast very carefully and has no concept of
suppression; Broken Arrow abstracts the blast into two radii and two modifiers and
spends the saved complexity on what the blast *does to the crew*.** That is the
trade in one line.

`AOE_SEARCH_EXTRA_RADIUS` and `SearchBuildingsForAOE` are **[inferred]** the query
margin — search wider than the damage radius so partially-overlapping and
large-bounds objects are not missed, which is the same instinct as Nuclear Option
gathering at `blastRadius * 2f`.

---

## 5. Critical hits: subsystem damage as a table

**[BUILD]**

```
CriticalEffectSystem   CriticalEffectComponent   CriticalEffectData
CriticalEffectTypes    CriticalEffectStatus     CriticalEffects   AllCriticalEffects
AddCriticalEffect   UpdateCriticalEffects   SetCriticalEffectsZero   IsSameCriticalEffects
CriticalBaseProbability   CriticalValuesTable   _criticalMultipliers   SetCriticalModifiers
UNIT_CRIT_SLOTS_COUNT   IsCriticalHit   CriticalDamage   CriticalDamages   get_CriticMultiplier
CriticalMobility   CriticalOptics   CriticalTargeting   CriticalFuel   CriticalLoading
GetCriticalMobility
NetworkCriticalEffectTypes   SendUnitGetCriticalEffect   RemoteUnitGetCriticalEffect
_criticalMobility  _criticalOptics  _criticalTargeting  _criticalLoading
CriticalMobilityImage  CriticalOpticsImage  CriticalTargetingImage  CriticalColor
```

**Five critical effects, and the names say exactly what each one is:**

| Critical | **[inferred]** what it disables |
|---|---|
| `CriticalMobility` | tracks / drivetrain — the unit cannot move, or moves slowly |
| `CriticalOptics` | sights — detection range and accuracy |
| `CriticalTargeting` | turret drive / fire control — cannot engage properly |
| `CriticalFuel` | fuel tank — leaking, or immobilised over time |
| `CriticalLoading` | autoloader / crew — rate of fire |

Structured as an ECS system with a component, a **fixed number of slots per unit**
(`UNIT_CRIT_SLOTS_COUNT`), a **base probability with multipliers**
(`CriticalBaseProbability`, `_criticalMultipliers`, `SetCriticalModifiers`,
`CriticalValuesTable`), replicated over the network as its own message
(`SendUnitGetCriticalEffect` / `RemoteUnitGetCriticalEffect`,
`NetworkCriticalEffectTypes`), and surfaced in the UI with per-effect icons. The
mission editor exposes it too — [`broken_arrow.md`](broken_arrow.md)'s manual
documents a *Repair / set health* node that can "add/remove critical hits".

**[inferred] This is the single sharpest contrast in the whole comparison, and it
is not about fidelity — it is about *where* the fidelity lives.**

Both games model "the thing is damaged but not dead, and it now behaves worse".
Broken Arrow does it with **a probability table over five named effects**. Nuclear
Option does it **physically**: there is no critical-hit table anywhere, because a
damaged wing's `wingEffectiveness` falls and its drag area rises
([`nuclear_option.md`](nuclear_option.md) §3.3), a damaged joint's break force is
rescaled so the wing separates at a lower g load, a shot-up rotor blade actually
gets shorter, and a flooded ship compartment's longitudinal drag coefficient lerps
toward its lateral one so the hull genuinely stops being streamlined.

**Nuclear Option cannot have a crit table because its parts are real; Broken Arrow
cannot simulate its parts because it has a thousand units.** `CriticalMobility` is
what you write when you cannot afford to simulate a track coming off — and it is
the correct thing to write at that scale. The crit table is *cheaper, more
legible to the player, easier to balance and trivially networkable*
(`NetworkCriticalEffectTypes` is an enum; replicating Nuclear Option's part state
requires per-part damage RPCs). The physical model is *more surprising and
self-consistent* and costs a rigidbody per part.

---

## 6. Suppression is a second damage channel

**[BUILD]**

```
StressComponent   StressLevel   StressLevelOneShocked   MaxStress   CurrentStressValue
CurrentStressLevel   Stress   IsStressed   IsSuppressed   IsFlowSuppressed
CalculateStressDamage   CalculateStressDamageFromFixedValue   ApplyStressDamage
StressDamage   IncreaseStress   SetStress   SetStressLevel   SetStressModifiers
STRESS_TICK   RunStress   RemoteUpdateStress   SendUnitGetStressDamage
Panic   Panicked   PanicFunction   RunPanic   InvokePanicFunction
PanickedStressValue   ShockedStressValue   PanickedLevelMultiplier   PanickedLevelMultiplierInfantry
ForcedStressProportion   OnTransportDeathCargoForcedStressPercentage
OnTransportUnloadCargoForcedStressPercentage   StressDamageModThreshold
MagazineReloadTimeStressModifier   LastMissileStressAccuracyMultiplier
StressAOEDamageModifier   StressAOERadius
StressIndicator   StressGlow   StressBlinkState   DebugStressSystem
```

**[inferred] A full morale model, and it is a parallel damage system with its own
verbs.** Stress accumulates (`IncreaseStress`, `ApplyStressDamage`, computed by
`CalculateStressDamage` — note *damage*, the same vocabulary), ticks on its own
schedule (`STRESS_TICK`), crosses **named thresholds** (`ShockedStressValue` →
`PanickedStressValue`, with `StressLevelOneShocked` suggesting an enumerated ladder),
and feeds back into combat through `MagazineReloadTimeStressModifier` and
`LastMissileStressAccuracyMultiplier` — **a suppressed unit reloads slower and
shoots worse.** `PanickedLevelMultiplierInfantry` says infantry panic on a different
curve from vehicles.

The two transport entries are the best detail in the set:
`OnTransportDeathCargoForcedStressPercentage` and
`OnTransportUnloadCargoForcedStressPercentage`. **[inferred] Infantry whose IFV is
destroyed under them come out already suppressed, and infantry unloading normally
take a smaller stress hit.** That is a specific, observed-from-life behaviour
encoded as two authored percentages, and it is the kind of thing that only gets
written by someone who noticed that dismounts from a brewed-up vehicle should not
be combat-effective.

**Nuclear Option has no equivalent whatsoever** — its only "the operator is
degraded" model is `GLOC`, a blood-pressure integrator that takes the *player's*
inputs away ([`nuclear_option_control.md`](nuclear_option_control.md) §1.3). Which
is the right call for a game with one crewman per unit and no infantry.

---

## 7. Where the numbers live: a REST-served database

**[BUILD]** This is the finding I did not expect, and it changes how you read
everything above:

```
RestDatabaseArmorsData        RestDatabaseUnitArmorsData
RestDatabaseAmmunitionsData   RestDatabaseWeaponAmmunitionsData
RestDatabaseAbilityData       RestDatabaseMobilityData
RestDatabaseCountriesData     RestDatabaseModificationsData
RestDatabaseOptionsData       RestDatabasePlaneFlyPresetData
RestDatabaseConfig   RestDatabaseConfigs
ArmorsJson   UnitArmorsJson   AmmunitionsJson   PublicArmorData   PublicData
GetAllArmors   GetAllUnitArmors   GetAllAmmunitions   GetAllWeaponAmmunitions
InitArmors   LoadArmor   LoadAmmunitions   LoadDatabase
CallOnArmorsUpdate   OnArmorsUpdate   add_OnArmorsUpdate
DisplayInArmory   SubscribeToArmory
DataBaseService   DataBaseCompiled   DataBaseManagerJson   IDataBaseModel   IDataBaseResolver
```

**[inferred] The balance database — armour values, ammunition, mobility,
modifications, per-country rosters, even aircraft flight presets — is fetched over
REST as JSON, with an update event (`OnArmorsUpdate`) and a subscription
(`SubscribeToArmory`).**

Put that next to [`broken_arrow.md`](broken_arrow.md) §2's `ExcelDataReader` and
the pipeline reads end to end: **spreadsheets → a REST service → the client, live,
with change notification.** A balance patch does not need a client build. The
in-game "Armory" browses the same data (`DisplayInArmory`, `PublicArmorData`), so
the stat card the player reads and the numbers the simulation uses come from one
source.

**[inferred] This is the deepest structural difference between the two games'
data models**, and it is bigger than any individual formula. Nuclear Option's
equivalent numbers are `ScriptableObject` fields baked into the build
([`nuclear_option_damage.md`](nuclear_option_damage.md) §5) — which is why its type
trees being stripped makes them unrecoverable, and equally why changing a wing area
requires a patch. Broken Arrow's are rows in a table behind an HTTP endpoint.

The costs are real and worth naming: the client now has a *runtime dependency on a
service* for its core simulation constants; the data can change under a running
match unless carefully versioned; and offline or modded play needs a fallback
(`DataBaseCompiled`, `COPY_DATABASE_FILE` and `DataBaseManager_old` are
**[inferred]** exactly that — a baked copy). But for a live multiplayer game with a
DLC roster and a competitive scene, being able to nerf a tank on a Tuesday without
shipping a build is worth a great deal.

---

## 8. What is not established

- **This note has almost no [TRACE].** The audio note had `IMusicService` in 49
  session logs and a listener warning in 38. The damage path produces no log
  output, so §§1–7 are **[BUILD]** naming plus my reconstruction. The parent note's
  §4.1 exists precisely because that can be wrong.
- **There are no method bodies.** IL2CPP means `DamageFormulaKinetic` is a *name*.
  I do not know what the formula is. Recovering it needs a reverse-engineering pass
  on 130 MB of `GameAssembly.dll` with a metadata-driven disassembler, which I did
  not attempt.
- **No authored value was recovered, and here it is harder than for Nuclear
  Option.** Nuclear Option's numbers are in stripped-type-tree assets on disk —
  recoverable in principle with a type-tree generator. Broken Arrow's are **served
  over the network** (§7), so they are not in the install at all. A `DataBaseCompiled`
  fallback may exist on disk; I did not find or parse it.
- **§3's reading — dispersion picks the aimpoint, then the shell is flown — is an
  inference from names**, not something I can show. It is the reading that makes
  `CalculateWeaponHitChance` and `BallisticShellSystem` coexist sensibly, but a
  hit-chance gate that skips the shell entirely for some weapon classes would also
  fit the evidence.
- **The critical-effect probability model is not established** beyond that it has a
  base probability, multipliers and a values table. Whether penetration margin,
  damage dealt or hit location drives it is unknown.
- **`HitDetectionSystem` and `ShootingSystem` were not analysed**, only named from
  the parent note's system list.
- **I have not played either game.**

---

## 9. Read against Nuclear Option

| | Broken Arrow | Nuclear Option |
|---|---|---|
| Armour facings | **4** (front / sides / rear / top) | **none** — one value per part |
| Armour types | **2** (kinetic, HEAT) + an HE constant | **3 channels** (pierce, blast, fire) + impact |
| Armour maths | penetration vs armour, per facing and type | `max(dmg − armour, 0) / tolerance`, summed |
| Penetration | **range-interpolated, 3–4 sample points** | none — no penetration concept |
| Damage formulas | **`DamageFormulaKinetic` and `DamageFormulaHEAT`, separate** | one `CalcNetDamage` for all channels |
| Kinetic damage | via penetration model | `pierce · v²/v₀²` — kinetic energy |
| Target categorisation | **5 boolean flags** (`TargetTypeVehicles`, `…Infantry`, `…Aircrafts`, `…Helicopters`, `…Projectiles`) | **a 5-vector · 4-vector dot product**, continuous blends |
| Hit resolution | **hit chance + dispersion CDF, then a flown shell** | flown projectile only, no roll |
| Blast | two radii (health, stress) + modifiers | one wave, `yield^(1/3)`, overpressure `1/Z³`, propagating at 340 m/s |
| Subsystem damage | **5-effect critical table**, probabilistic, slotted | **physical** — parts detach, areas change, joints weaken |
| Suppression | **full stress/panic model**, second damage channel | none (GLOC affects the player only) |
| Where the numbers live | **REST-served JSON, live-updatable** | ScriptableObjects baked into the build |
| Scale it must serve | ~1,000 units, camera above | ~dozens of aircraft, camera *inside* one |

**[inferred] Four readings.**

**The camera position determines the whole design.** Broken Arrow's player watches
a tank duel from above and wants to know *"did my front armour hold"* — so armour
gets four facings and penetration gets a range curve, and the answer has to be
legible enough to plan around. Nuclear Option's player is inside one aircraft and
wants to know *"will this missile engage that target, and what breaks when I get
hit"* — so categorisation gets a continuous blend and damage gets physical
consequences you feel through the stick. **Each game spent its complexity budget
where its player is looking.**

**The two categorisation models are not the same idea at different fidelities —
they answer different questions.** Broken Arrow's five flags answer *may this
weapon engage that unit at all* (`CanAmmoTargetUnitType`), and once the answer is
yes, the armour and penetration model decides everything. Nuclear Option's dot
product answers *how much do I want to* — it is a continuous preference feeding a
scoring cascade, because its AI has to choose between targets rather than merely
being permitted to shoot. **[inferred] Broken Arrow can afford flags because a
human commander picks the targets; Nuclear Option needs a gradient because nobody
does.**

**Abstraction and simulation of subsystem damage are a genuine either/or, and
scale forces the choice.** §5 is the clearest instance of a rule that shows up
repeatedly across these notes: you can have a thousand units with a crit table, or
forty parts per unit with real physics, and there is no configuration where you
have both.

**And the data pipeline is the difference that would matter most on a real
project.** Everything else here is a modelling choice you could change in a
refactor. Spreadsheets → REST → live client update, versus ScriptableObjects in the
build, is an architectural commitment made early that shapes how the team works for
years — who can change a number, how fast, and whether it needs QA on a build.

---

## 10. What is worth taking

1. **Derive the armour facing from the shot vector at impact, not from a bearing
   assigned at fire time.** (§1.) `GetArmorSideFromShotVector` makes flanking a
   geometric fact rather than a targeting decision, and it costs one dot product.

2. **Separate armour by defeat mechanism, not just by amount.** (§1.) Kinetic and
   HEAT values per facing is what makes composite armour, ERA and "wrong tool for
   the job" expressible at all.

3. **Author penetration at two or three reference ranges and interpolate.** (§2.)
   Designers can reason about "penetration at 1,000 m"; nobody can reason about a
   curve.

4. **Give different defeat mechanisms different damage formulas.** (§2.) A kinetic
   penetrator's damage scales with residual energy; a shaped charge's does not.
   One formula for both is a compromise that satisfies neither.

5. **Combine a statistical aimpoint with a flown projectile.** (§3.) The roll gives
   designers a balancing knob and the player a legible accuracy stat; the flown
   shell gives overshoots, terrain clips and honest tracer. Sample the scatter from
   a CDF rather than a naive uniform.

6. **Give area effects separate radii for lethality and suppression.** (§4.) It is
   two extra authored floats and it is what makes suppressive fire a real tactic
   instead of weak damage.

7. **Model "damaged but alive" as a small table of *named* effects.** (§5.)
   Mobility / optics / targeting / fuel / loading, probabilistic, slotted,
   networkable as an enum, and surfaced with an icon each. If you cannot afford
   physical part damage — and at a thousand units you cannot — this is the right
   abstraction, and naming the five effects is most of the design.

8. **Treat suppression as a parallel damage channel with the same verbs.** (§6.)
   `CalculateStressDamage` / `ApplyStressDamage`, its own tick, named thresholds,
   and feedback into reload time and accuracy. And model the specific cases —
   infantry dismounting from a destroyed transport arriving pre-suppressed is one
   authored percentage.

9. **Serve balance data rather than baking it.** (§7.) Spreadsheets → REST → client
   with a change event means a balance patch is not a build. Keep a compiled
   fallback for offline. This is the decision on this list with the longest tail.

10. **And the meta-lesson: decide where your player is looking before you decide
    what to model.** (§9.) Both of these games are internally coherent, and every
    difference between them traces back to camera position and unit count. A damage
    model copied from the wrong genre will be simultaneously too detailed and not
    detailed enough.

---

## 11. Reproducing this

```
# the identifier table — the only real source for this note
broken_arrow/BrokenArrow_Data/il2cpp_data/Metadata/global-metadata.dat   # 27.9 MB
  regex [A-Za-z_][A-Za-z0-9_]{3,60}
  filter out UnityEngine/System/Epic/Mono/Microsoft/FMOD/Sirenix/EOS_/AntiCheat prefixes
  then grep: armor, penetr, critical, stress, dispersion, aoe, ammunition, restdatabase

# first-party documentation, shipped
broken_arrow/Manual/Data/MissionEditor/2. Node groups/Gameplay.md   # "add/remove critical hits"
broken_arrow/Manual/Data/MissionEditor/3. Lua/07. LuaUnit.md        # "Returns curent HP percentage"

# runtime evidence — none found for the damage path
broken_arrow/GameLogs/*.log
```

**[inferred] What would move this note from reconstruction to fact**: a
metadata-driven IL2CPP disassembly of `DamageFormulaKinetic`, `DamageFormulaHEAT`,
`ComputePenetration` and `CalculateStressDamage`. Everything above is the shape of
the model; those four bodies are the model.

---

## Sources

- **The retail install**, `C:\Program Files (x86)\Steam\steamapps\common\broken_arrow`
  — `global-metadata.dat`, the shipped `Manual/`, `GameLogs/`.
- [`broken_arrow.md`](broken_arrow.md) — the parent note, and the **[BUILD]** /
  **[TRACE]** discipline this one inherits (and mostly cannot satisfy).
- [`nuclear_option_damage.md`](nuclear_option_damage.md) — the other half of §9.
- [`ruse.md`](ruse.md) §11.4 — Eugen's resolution model, the genre's third answer.
- **No engineering talk, blog or paper was found for either game.**
