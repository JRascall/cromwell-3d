# World in Conflict — the gameplay layer, decompiled

WiC ships its entire gameplay and mission layer as **Python 2.3 bytecode**, and
it decompiles cleanly: **375 modules, 7.7 MB of source, zero failures**. This is
what that layer looks like — the group/command model, the two-slot behaviour
system that gives units a standing order and a combat personality at the same
time, the tasking vocabulary the C++ AI actually exposes, and the 127-event
reaction system the campaign is written against.

> **On sources.** Everything tagged **[BUILD]** was decompiled from the retail
> install on this machine (`E:\World in Conflict`, v1.0.1.0) with
> [`tools/wic/wic_pyo.py`](../../../../tools/wic/wic_pyo.py) over uncompyle6.
> Class names, method names, default arguments, docstrings and comments are
> Massive's own, transcribed. Where a Python method is a one-line facade over a
> C++ entry point the note says so and names the entry point, because **the
> shape of that boundary is the most informative thing here** — it says exactly
> which decisions Massive kept in the engine and which they handed to designers.

Tags: **[BUILD]** decompiled from the install. **[inferred]** our reading.

Companions: [`world_in_conflict.md`](world_in_conflict.md) (the renderer),
[`world_in_conflict_particles.md`](world_in_conflict_particles.md) (the smoke).

Related: [`spatial_queries.md`](../../../topics/agents/spatial_queries.md),
[`navigation.md`](../../../topics/agents/navigation.md),
[`ruse.md`](../ruse.md) — Eugen also ship marshalled Python, and §6 is the
comparison.

---

## 1. The shape of the layer

**[BUILD]** The tree divides three ways:

| | Modules | What it is |
|---|---:|---|
| `python/` | ~60 | the framework: `unit`, `group/`, `reaction/`, `trigger`, `player`, `team`, `camera/`, `wicgame/` |
| `maps/<name>/python/` | ~250 | per-mission scripts — `allunits.py`, `server.py`, `client.py`, `convoy.py`, `mapvars.py` |
| `python/_debugger/` | ~30 | **a full remote Python debugger shipped in the retail build** — `pydb`, `bdb`, `threaddbg`, `gdb`, `inspect`, `pydoc` |

The per-map scripts are large: `maps/berlin1/python/allunits.py` alone
decompiles to **240 KB of source**. A mission is not data with a few hooks; it
is a program.

**[inferred]** The shipped debugger is worth a sentence. Massive left a
`pydb`-based remote debugger, `pydoc` and `inspect` in the retail archives —
about 300 KB of tooling a player never runs. That is the signature of a team
whose designers were writing real code against a live game and needed to break
into it, and it is the same instinct as the `ViewPenumbraPass` and
`DebugOverdrawPass` shaders left in the shipped renderer
([`world_in_conflict.md`](world_in_conflict.md) §2).

---

## 2. Groups, and a queue of 62 commands

**[BUILD]** The unit of scripting is not the unit. It is the **group**, with
`Platoon` above it and individual units addressed by index below it:

```python
mapvars.grpPlayer1 = CreateGroup('grpPlayer1', (), None, PLAYER_SCRIPT, TEAM_USSR)
mapvars.grpPlayer1.CreateSquad([spetznasMarine] * 4, 'areaSpetznas2')
mapvars.grpPlayer1.SetExperienceLevel(3)
mapvars.grpPlayer1.SetFormationEx(FORMATION_BOX, DEFAULT_FORMATION_DISTANCE, 4, 5)
```

Orders are **queued objects, not calls** — `group.PostCommand(CMD_MoveGroup(...))`
appends to a per-group queue. `group/command.py` defines **62 command classes**:

| Group | Commands |
|---|---|
| Movement | `MoveUnit`, `MoveGroup`, `MoveGroupBackwards`, `MoveForward`, `MoveSubgroup`, `MoveMultipleTargets`, `TeleportGroup`, `FollowPath`, `Follow`, `StopGroup` |
| Combat | `AttackUnit`, `AttackTarget`, `AttackArea`, `SurroundTarget`, `TakeCover`, `HoldFire` |
| Transport | `EnterContainer`, `UnitEnterContainer`, `UnitsEnterContainer`, `EnterBuilding`, `EnterGroup`, `UnloadAll`, `UnloadFrom`, `SetTransporterGroup` |
| Formation | `SetFormation`, `SetFormationEx`, `SetFormationOffset`, `SetFormationWidthOffset`, `Regroup`, `RegroupAt`, `RegroupAtOnNeed` |
| Facing / speed | `LookAt`, `FaceDirection`, `SetSpeed`, `SetUnifiedSpeed`, `RestoreSpeed` |
| Lifecycle | `CreateGroup`, `CreateUnits`, `CreateUnitsAtPosition`, `AddUnits`, `AddUnitsFromGroup`, `DestroyGroup`, `DestroyUnits`, `SetHealth`, `SetRandomHealth`, `SetState` |
| Ownership | `SetOwner`, `SetTeam`, `SetGroupApOwner` |
| Supply | `ActivateRefillMode`, `UpdateRefillMode`, `DeactivateRefillMode`, `Refill` |
| Flow | `Wait`, `WaitUntil`, `WaitUntilNextDrop`, `CustomCommand` |
| Behaviour | `SetBaseBehavior`, `SetAttackBehavior`, `AddCombatExceptionGroup`, `RemoveCombatExceptionGroup` |

**[inferred]** Two design points. `Wait` and `WaitUntil` are *commands in the
same queue*, which is what lets a whole mission beat be written as one linear
sequence of `PostCommand` calls rather than as a state machine — the queue is
the state machine. And the last row means **behaviour is itself a queued
order**: a group's personality can change mid-sequence, at a scripted moment,
without the script having to own the consequences.

Only three formations ship — `FORMATION_BOX`, `FORMATION_LINE`,
`FORMATION_COVER`. `SetFormationEx(shape, distance, cols, rows)` carries the
rest.

---

## 3. Two behaviour slots: a standing order and a temperament

**[BUILD]** This is the piece worth taking. `group/behavior.py` gives every
group **two behaviours running at once**, in independent slots:

```python
class BehaviorManager(object):
    def ActivateBaseBehavior(self, aNewBehavior):   ...
    def ActivateAttackBehavior(self, aNewBehavior): ...
```

**13 base behaviours** — what the group does when nothing is happening:

`BHB_Idle`, `BHB_IdleUnits`, `BHB_IdleInfantry`, `BHB_IdleInfantryCover`,
`BHB_Patrol`, `BHB_MoveRandomly`, `BHB_HoldPosition`, `BHB_RegroupAt`,
`BHB_CaptureCommandPoint`, `BHB_Artillery`, `BHB_AreaArtillery`,
`BHB_GroupHealer`, `BHB_RepairInAreas`

**9 attack behaviours** — how it reacts when shot at, named as temperaments
rather than as tactics:

`BHA_Fearless`, `BHA_Aggressive`, `BHA_Blitzer`, `BHA_Defensive`,
`BHA_StandFast`, `BHA_StandFastEx`, `BHA_Chicken`, `BHA_Flanker`,
`BHA_CautiousFlanker`

### 3.1 The two slots are driven completely differently

**[BUILD]** This is the detail that makes it work, and it is easy to miss:

```python
# base: polled on a timer
self.__BaseReaction = Repeat(self.__BaseBehavior.GetUpdateTime(),
                             Action(self.UpdateBaseBehavior))

# attack: subscribed to an event
self.__AttackReaction = RE_OnGroupInCombat(self.__Group,
                                           Action(self.UpdateAttackBehavior))
```

The **standing order is polled**; the **combat reaction is event-driven**. And
the rate limiting on the combat side is done by *unsubscribing*, not by checking
a clock inside the handler:

```python
def UpdateAttackBehavior(self, anUnitId=-1, anAttackerId=-1):
    ...
    self.__AttackBehavior.Execute(anUnitId, anAttackerId)
    self.__AttackReaction = None                       # stop listening
    self.__StartAttackReaction = Delay(self.__AttackBehavior.GetUpdateTime(),
                                       Action(self.StartAttackReaction))
```

**[inferred]** A group under sustained fire generates combat events continuously.
Handling every one and early-returning on a timer would still pay the dispatch,
the two unit lookups and the handler entry on every bullet. Dropping the
subscription and re-arming it after the behaviour's own update interval makes
the cost of being shot at *zero* between re-evaluations, and the re-arm interval
is a property of the behaviour — a `BHA_Chicken` can reconsider more often than a
`BHA_StandFast` without either of them knowing about the other.

**The transferable rule: poll the standing order, subscribe the reaction, and
rate-limit a reaction by unsubscribing rather than by testing a timer inside the
handler.** It applies directly to this repo's entity tick — see the hot-loop
rules in CLAUDE.md, which say the same thing about work that should not be
reached rather than work that should return early.

Note also `AddCombatExceptionGroup` / `RemoveCombatExceptionGroup`: a per-group
list of enemies that do *not* trigger the attack behaviour. Scripted convoys can
be shot at by a specific group without waking up.

---

## 4. What the C++ AI actually exposes: zones, not paths

**[BUILD]** `wicgame/ai.py` is a thin facade; every method is one call into
`wicg.*`, the C++ side. That makes the module a precise inventory of the
engine/designer boundary. The whole tasking vocabulary is three verbs:

```python
wicg.HoldAICommandPointArea(id, cpCenterPos, stance)
wicg.HoldAIZone(id, moveZoneCenterPos, moveZoneRadius, someKillZones, stance)
wicg.HoldAIZoneLine(id, someMoveStripes, aWidth, someKillZones,
                    aUseFlanking, aUseSpreadOut, aUseHeight, stance)
```

A task is **a region the AI may occupy plus a set of regions it is responsible
for covering** — a *move zone* and *kill zones* — with a stance
(`AI_STANCE_DEFENSIVE` / `AI_STANCE_OFFENSIVE`). Nothing about paths, positions,
targets or ordering. `HoldZoneLine` extends it to a front: a set of **move
stripes** and a width, with `aUseFlanking`, `aUseSpreadOut` and `aUseHeight`
(defaulting **on** — the AI prefers high ground unless told not to).

The rest of the surface is toggles over what the AI is *permitted* to do:
`EnableBuy`, `EnableTA` with `SetTAZones`, `SetArtilleryZones`,
`EnableSpecialAbilities` with `SetSpecialAbilityOffensiveFactor`,
`EnableInfantryEnterBuildings`, and `HideCommandPoint` / `ShowCommandPoint` —
which hides an objective from the AI's consideration entirely rather than
telling it to ignore one.

Difficulty is not a stat multiplier here. It is
`AI_SPECIAL_ABILITY_FACTOR_EASY / NORMAL / HARD` — how aggressively the AI
spends its abilities.

**[inferred]** This is the cleanest expression in any of these studies of "the
script says *where and whether*, the engine says *how*". A designer paints
areas and sets permissions; positioning, target selection, flanking and cover
stay in C++ where they can be fast and where a mission script cannot get them
wrong. Compare R.U.S.E. ([`ruse.md`](../ruse.md)), which also ships Python but
puts far more of the decision-making in it.

---

## 5. Reactions and triggers: 127 events

**[BUILD]** Missions are written against `RE_On*` reactions — **127 distinct
event types**. The distribution is the interesting part:

| Domain | Examples |
|---|---|
| Units and groups | `OnUnitDestroyed`, `OnGroupInCombat`, `OnGroupHealth`, `OnGroupSize`, `OnEmptyGroup`, `OnGroupRefill`, `OnUnitCreatedInSquad` |
| Space | `OnUnitInArea`, `OnGroupNotInArea`, `OnPlayerInForest`, `OnTeamInArea`, `OnRadarScanInArea` |
| Objectives | `OnCommandPointTaken`, `OnPerimeterPointTaken`, `OnFortificationCreated`, `OnBridgeDestroyed`, `OnBuildingDestroyed` |
| Tactical aid | `OnTAChoosen`, `OnTAClicked`, `OnTAExecuted`, `OnTASpawned`, `OnTAProjectile`, `OnTAProjectileDestroyed`, `OnTAOutsideRestrictedArea` |
| **Camera** | `OnCameraZoomIn/Out`, `OnCameraPanLeft/Right/Up/Down`, `OnCameraMoveForward/Backward`, `OnCameraInArea`, `OnEnterMegaMap` |
| **UI** | `OnMiniMapClicked`, `OnUnitpaneSelected`, `OnReinforcementsMenuClicked`, `OnObjectiveBrowserActivated`, `OnSkipMessageBoxButtonClicked` |

**[inferred]** Roughly a fifth of the vocabulary is camera and UI input. Those
events exist for one reason — **the tutorial** — and they are the cost of being
able to write "wait until the player actually zooms in" as a script line rather
than as engine code. It is a real design decision with a real price: 25-odd
event types, plumbed from the UI into the scripting layer, used by a handful of
missions.

`trigger.py` adds a composable layer above: `TRG_UnitInArea`,
`TRG_UnitsInAreaAllIn`, `TRG_UnitsInAreasAllIn`, `TRG_UnitsKilled`, `TRG_Timer`,
`TRG_Event`, `TRG_UnitUnderAttack`, `TRG_MessageBoxClosed`, `TRG_CustomTrigger`,
combined by `TRG_ANDList` and `TRG_ORList`.

`wicgame/` holds the mission-design helpers built on all of it: `objective`,
`tacticalaid`, `los`, `feedback`, `civilians`, `cinematic`, `timer`,
`actionqueue`, and `disobey`.

**`disobey.py`** is the "you are leaving the mission area" system, and it is
worth naming because it is a *designed* mechanic rather than a failure state:
a `Disobey` object binds groups to areas and holds **three escalating message
sets** (`AddFirstMess` / `AddSecondMess` / `AddThirdMess`), each firing a custom
event (`DisobeyWarning_1..3`) that the mission script binds to whatever it likes
— a warning, a disabled command point, or `GameOver`. It has a 30-second
`ReactivateTime`, so wandering out and back does not burn all three warnings.

**[BUILD]** Line of sight for scripting is explicit volumes, not queries:
`LOSCircle(target, radius)` and `LOSRectangle(target, size, orientation)`, added
and removed by hand. A mission grants vision by placing a shape.

---

## 6. The voice feedback matrix — 9,662 lines, and it carries information

**[BUILD]** `sound/feedback/` is **9,662 distinct files, 132 MB** — the single
largest content system in the game after the textures, and larger than every
mission script, shader and effect definition combined. The filenames decode
completely, and what they decode to is a five-dimensional matrix:

```
sound/feedback/eu/tanks_br/move/eu_c_br_tan_mov_med_001a.mp3
               │   │        │     │  │  │   │   │   │    └ take a/b/c
               │   │        │     │  │  │   │   │   └──── variant 001..
               │   │        │     │  │  │   │   └──────── qualifier (med/sht)
               │   │        │     │  │  │   └──────────── situation
               │   │        │     │  │  └──────────────── unit class
               │   │        │     │  └─────────────────── nationality
               │   │        │     └────────────────────── crew
               │   │        └──────────────────────────── situation folder
               │   └───────────────────────────────────── unit class
               └───────────────────────────────────────── faction
```

**Faction** is US (2,968), EU (3,586), USSR (3,104) — and the EU set is
subdivided by **nationality**: `tanks_br` (409), `tanks_ge` (351),
`infantry_no` (368) alongside the generic sets. A British Chieftain crew and a
German Leopard crew are different voices, in a faction that already has its own.

**Situation** is `attack` (2,102), `move` (1,394), `killshot` (1,360), `select`
(1,142), `under fire` (776), `special` (504), `error` (120), `perimeter` (120).

### The dimension that makes it a system rather than a soundbank

**[BUILD]** `attack`, `killshot` and `under fire` are keyed by **target class** —
and the split is exactly even, which is the signature of a spec rather than of
opportunistic recording:

| Token | `inf` | `tan` | `hel` | `veh` | `aau` | `sup` | `bld` | `for` |
|---|---|---|---|---|---|---|---|---|
| `killshot` | 120 | 120 | 120 | 120 | 120 | 120 | 120 | 120 |
| `attack` | 120 | 120 | 120 | 120 | 120 | 120 | 120 | 120 |
| `under fire` | 48 | 48 | 46 | ~48 | 48 | 42 | 48 | 48 |

Infantry, tank, helicopter, vehicle, anti-air, support, building, fortification.
So a unit says something different for *engaging* a helicopter than for engaging
a building, something different again for *killing* it, and — the interesting one
— **something different depending on what is shooting at it**.

**[inferred]** That last row is why this is worth the space. In an RTS the camera
is somewhere else most of the time, and "we're taking fire from a helicopter" is
not flavour, it is **an alert that names the threat class without requiring you
to be looking**. Massive spent 776 recordings, across three factions and eleven
unit classes, on making incoming fire self-describing. The a/b/c takes (1,650 /
1,650 / 518) exist so the same line does not repeat verbatim when it fires twice
in a fight.

The scripting side of this is `wicgame/feedback.py`, which wires objective events
(`CommandPointFeedbackCPLost`, `CommandPointFeedbackRuFortif`) to the campaign's
named officers — `aUseBannon`, `aUseWebb`, `aUseSawyer` are constructor flags on
a feedback object, so a mission chooses *which character* comments on an event.

---

## 7. The rest of the install, briefly

**[BUILD]** Two other formats carry design information worth naming.

**`.ice` — rigid bodies for destruction.** 2,031 files, of which **1,316 are
named `*_rb.ice`**. Magic `ice0010`. Each holds named sub-pieces with transforms,
quaternions and a **surface material token**: `METAL` (3,241 occurrences),
`WOOD` (206), `CONCRETE` (193), `FLESH` (172), `BRICK` (32), `GLASS` (4), with
`NONE` as the default (5,591). Every destructible prop, every vehicle, and every
wreck variant ships a physics breakdown — `chieftain_mkv_wreck_ver01_rb.ice`,
`_ver02_rb.ice` — so a tank has *authored alternative wrecks*, not one.

**`.gety` — wreck geometry straight out of LightWave.** 223 files, and they are
literal `FORM…LWO2` objects with a `TAGS` chunk reading `Wreck`. The destruction
meshes were not converted into an engine format at all; the DCC file is the
shipped asset.

**[inferred]** Both point the same way. WiC's destruction is **authored, not
simulated** — a fixed set of pieces with hand-assigned materials and hand-made
wreck variants, driven by Havok but not decided by it. That is consistent with
what the terrain shader does with its wreck textures and direction field
([`world_in_conflict.md`](world_in_conflict.md) §5): the *appearance* of
open-ended destruction, assembled from a bounded, art-directed vocabulary.

---

## 8. What to take

**Two behaviour slots, driven differently.** A standing order polled on a timer
and a combat temperament subscribed to an event, rate-limited by unsubscribing.
Thirteen of the first and nine of the second cover an entire campaign, and the
combinatorial product is why 22 small classes read as a varied army. §3.

**Task an agent with regions and permissions, not with positions.** A move zone,
a set of kill zones, a stance, and a handful of "may it buy / may it use
artillery / may it enter buildings" toggles. Everything expensive or easy to get
wrong stays in the engine. §4.

**Make waiting a command.** `CMD_Wait` and `CMD_WaitUntil` in the same queue as
movement and combat is what lets a mission beat read as a linear script instead
of a state machine. §2.

**An exception list beats a special case.** `AddCombatExceptionGroup` is one
list per group that suppresses combat reaction against named enemies — a scripted
convoy can be attacked without waking, with no branch in the behaviour. §3.1.

**And the warning:** roughly a fifth of a 127-event vocabulary exists to let the
tutorial observe the camera and the UI. Worth it or not, it is the kind of cost
that arrives late, attaches to one feature, and never leaves. §5.
