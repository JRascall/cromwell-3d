# Broken Arrow — the same genre, assembled instead of built

Deep dive on **Broken Arrow** (Steel Balalaika / Slitherine, 2025), read from the
retail install on this machine. It is the newest serious attempt at the genre
[`ruse.md`](ruse.md) covers — a modern-warfare RTS with a deep-zoom camera,
kilometre-scale maps and hundreds of detailed units — and it is the natural
control experiment, because **it solves the same problems with almost none of the
same code.**

Eugen spent fifteen years building IRISZOOM. Steel Balalaika bought most of an
engine, assembled roughly fifteen third-party packages on top, and wrote the
simulation. Reading the two side by side is the clearest available answer to
"what does a bespoke engine actually buy you in this genre, and what is now
cheaper to purchase than to write."

> **Sources, and their limits.** Everything tagged **[BUILD]** was read from
> `broken_arrow/` — the install layout, `boot.config`, `ScriptingAssemblies.json`,
> the shipped `GameLogs/`, the IL2CPP metadata's identifier table, and file
> magics in `StreamingAssets/`. Nothing here comes from running or profiling the
> game.
>
> **The one caveat that governs every claim below.** The IL2CPP identifier table
> is a flat list of all 203,203 identifiers compiled into the binary, with no
> namespaces. **A name's presence proves a package is compiled in — not that the
> game uses it.** Unity ships whole packages whether or not they are referenced.
> So this note separates two grades of evidence: identifiers, and *runtime stack
> traces* from the shipped logs, which prove a thing actually ran. Where the two
> disagree the traces win, and §4.1 is a case where they did.

Tags: **[BUILD]** read from the install. **[TRACE]** seen in a runtime stack
trace, i.e. it demonstrably ran. **[inferred]** our reading.

**Audio and the damage model have their own notes.**
[`broken_arrow_damage.md`](broken_arrow_damage.md) reads the armour, penetration,
critical-hit and suppression systems against Nuclear Option's.

**Vehicle animation has its own note.**
[`vehicle_animation.md`](vehicle_animation.md) reads the turret, suspension,
track and crew-figure systems against Eugen's across three of their builds — and
§8 there is the strongest single instance of §10's convergence argument: an
`AnimationHub` holding an `IAnimationBehaviour[]` is Eugen's operator list,
arrived at independently on a different engine.

**Audio has its own note.** [`broken_arrow_audio.md`](broken_arrow_audio.md) reads
the FMOD and Resonance usage in detail, against Nuclear Option's no-middleware
equivalent — same method, same install.

Related: [`ruse.md`](ruse.md) (the bespoke-engine counterpart, and §10 here reads
the two against each other), [`map_scale.md`](map_scale.md),
[`lod_systems.md`](lod_systems.md), [`terrain_rendering.md`](terrain_rendering.md),
[`valve_networking.md`](valve_networking.md),
[`nav_architecture.md`](nav_architecture.md).

---

## 1. What it is, technically

**[BUILD]** Not what I expected before opening it, and worth stating plainly
because the genre's reputation points the other way:

| | |
|---|---|
| Engine | **Unity**, IL2CPP, metadata version 31 (Unity 2022.3 / 6000 era) |
| Render pipeline | **HDRP** (`com.alteregogames.aeg-fsr.Runtime.HDRP`, `ProjectDawn.Impostor.HighDefinition`) |
| Simulation | a **C# ECS**, in-house systems over a third-party framework |
| Scripting | **Lua** (MoonSharp) *and* a bespoke node-graph engine |
| Networking | **LiteNetLib** (reliable UDP), EOS + Steam |
| Audio | FMOD Studio + Resonance |
| Install | **54 GB**, of which `BrokenArrow_Data` is 53 GB |
| Assemblies | 187, of which ~95 are third-party or in-house rather than Unity/BCL |

`boot.config` enables `gfx-enable-gfx-jobs=1` and `gfx-enable-native-gfx-jobs=1`
— multithreaded rendering on — and sets `gc-max-time-slice=3`, an explicit
garbage-collector budget which is the tell that GC pauses were a real problem.

---

## 2. The package manifest is the design document

**[BUILD]** `ScriptingAssemblies.json` lists every assembly. Stripping Unity and
the BCL leaves a list that describes the architecture better than any diagram:

| Package | What it answers |
|---|---|
| `DefaultEcs`, `Leopotam.EcsProto.*` (5 assemblies) | the entity model — **two ECS frameworks compiled in**; see §4.1 |
| `GPUInstancer` | indirect GPU instancing, Hi-Z occlusion culling, billboards |
| `ProjectDawn.Impostor` (+ HDRP, URP) | impostors |
| `VisualDesignCafe.Nature` / `.Rendering.Nature` / `.Rendering.Instancing` / `.ShaderX` | Nature Renderer — vegetation |
| `Whinarn.UnityMeshSimplifier` | LOD mesh generation |
| `com.alteregogames.aeg-fsr` (+ HDRP) | FSR upscaling |
| `LiteNetLib` | reliable UDP transport |
| `MemoryPack.Core`/`.Unity`, `protobuf-net` | binary serialisation, two of them |
| `MoonSharp.Interpreter` (+ `VsCodeDebugger`) | Lua, with a VS Code debug adapter |
| `com.alelievr.NodeGraphProcessor` | the node-graph editor framework |
| `VContainer` | dependency injection |
| `UniTask` (+ 4 integrations), `UniRx` | async and reactive plumbing |
| `Sirenix.OdinInspector` / `.Serialization` | editor tooling and serialisation |
| `Cinemachine` | camera |
| `ExcelDataReader` | **game data authored in spreadsheets** |
| `SingularityGroup.HotReload.Runtime` | **hot reload, shipped in the retail build** |
| `Tayx.Graphy` | an on-screen perf overlay, also shipped |
| `io.sentry.unity` (+ 12 support assemblies) | crash and error telemetry |
| `BrokenArrow`, `NetworkCommon`, `ServiceAssembly`, `SceneDataAssembly`, `ToolsAssembly`, `AnalyticsCommon` | the in-house code |

**[inferred] Six in-house assemblies against ~90 bought ones.** That is the whole
thesis in one ratio, and the split is not arbitrary — what they wrote is the
*simulation, the services and the tools*. Everything that renders, streams,
serialises, transports or upscales was purchased.

Two entries deserve a note on their own. **`ExcelDataReader` means the unit
database is spreadsheets** — the same job Eugen's NDF does (`ruse.md` §2.2),
solved by not building a data language at all. And shipping
**`SingularityGroup.HotReload` and `Tayx.Graphy` in the retail build** means the
developer loop was never fully separated from the shipping build; the profiler
overlay CLAUDE.md asks for is, in their case, a bought package that never got
stripped.

---

## 3. Terrain and world rendering

### 3.1 Half the install is streamed virtual texture

**[BUILD]** `StreamingAssets/` contains four `.gts` files and a large set of
`.gtp` files. The magic is `GRPG` — **Granite**, i.e. Unity's Streaming Virtual
Texturing (`.gts` tile-set header, `.gtp` tile pages). Measured:

| | |
|---|---|
| `.gts` + `.gtp` total | **27 GB** |
| Largest tile-set header | 26 MB (`f51bbaf7….gts`); the other three are 1 MB each |
| `data.unity3d` | 6.8 GB |
| Whole install | 54 GB |

**[inferred] Virtual texturing is not a feature here, it is the budget.** Half
the shipped bytes are VT pages, and the 26:1 ratio between the big tile-set
header and the small ones says one enormous set (the world/terrain) and three
small ones. This is the same problem `ruse.md` §3.4 shows Eugen solving with a
`div_map` and a texture-diversity field — keep a whole-map view from looking like
a repeating tile — answered instead by paging unique texels for the entire world.

It is a straight trade of **disk and streaming bandwidth against authoring
effort and shader complexity**, and it is only available because a middleware
vendor solved it. Eugen could not buy this in 2010, so they had to be clever
about tiling; Steel Balalaika can, so they were not.

### 3.2 Vegetation and props: instanced, Hi-Z culled, impostored

**[BUILD]** The GPU Instancer type set is complete and specific:
`GPUInstancerPrefabManager`, `GPUInstancerTerrainManager`, `GPUInstancerTreeManager`,
`GPUInstancerDetailManager`, `GPUInstancerPrototypeLOD`, `GPUInstancerBillboard`
+ `GPUInstancerBillboardAtlasBindings`, **`GPUInstancerHiZOcclusionGenerator`**,
`GPUInstancerCell` / `PrefabCell` / `DetailCell`, and
`GPUInstancerFloatingOriginHandler`.

**[TRACE]** `GPUInstancer.GPUInstancerPrefabRuntimeHandler` appears **3,318 times**
in the shipped logs, so this one is unambiguously live.

**[inferred]** So the answer to "how do you draw a forest across several
kilometres" is: indirect instancing from spatial cells, culled on the GPU against
a hierarchical depth buffer, dropping to a billboard atlas at range, with LOD
meshes generated offline by `UnityMeshSimplifier` and true impostors from
`ProjectDawn.Impostor` beyond that.

Set against `ruse.md` §4.4 and §4.5, the *architecture is the same*: cells,
instancing, LOD ladder, impostors with baked lighting. Eugen wrote every layer of
it, including an offline texture-grouping tool that atlases a whole scenery set.
Broken Arrow has the same ladder assembled from three packages. **The interesting
part is that neither could skip a rung** — the problem shape is fixed by the
camera, not by the engine.

`GPUInstancerFloatingOriginHandler` is present **[BUILD]** but never appears in a
trace, so whether they actually rebase the origin is unproven. **[inferred]** For
a few-kilometre map you would not need to.

---

## 4. The simulation

### 4.1 Two ECS frameworks compiled in, one of them actually running

This is the case where identifiers and traces disagree, and it is a useful
warning about reading a build from its manifest.

**[BUILD]** Both `DefaultEcs` and five `Leopotam.EcsProto.*` assemblies ship.

**[TRACE]** In the shipped logs, `DefaultEcs.System.SequentialSystem` appears
**6,408 times** and `DefaultEcs.System.AEntitySetSystem` 198 times.
**`Leopotam` appears zero times.** The live call chain is:

```
BrokenArrow.Client.Ecs.Controllers.GameController:Update()
  BrokenArrow.Client.Ecs.Controllers.EcsLoader:Update()
    DefaultEcs.System.SequentialSystem`1:Update(T)
```

**[inferred] The running entity model is DefaultEcs, driven sequentially from a
MonoBehaviour `Update`.** Leopotam is either vestigial, a migration in progress,
or used on a path that never threw. Had I read only the manifest I would have
written the opposite of the truth.

Note also what this is *not*: it is not Unity DOTS/Entities. `lib_burst_generated.dll`
ships, so Burst compiled something, but the entity loop is a managed C# ECS
ticking sequentially — with `BrokenArrow.Shared.Ecs.FastParallelRunner` and its
`ThreadExecutionLoop` **[TRACE]** available for the parts that parallelise.

### 4.2 The system list

**[BUILD]** Filtering the identifier table for `*System` gives the simulation's
shape directly. A representative slice:

| Area | Systems |
|---|---|
| Movement | `NavigationSystem`, `NavMeshPathfindingSystem`, `InfantryNavigationSystem`, `HelicopterNavigationSystem`, `InfantrySoldierMoveSystem`, `InfantrySprintSystem`, `NavigationIdleStateSystem`, `PathPostProcessingSystem`, `MoveComplexSystem`, `RotateUnitSystem` |
| Combat | `ShootingSystem`, `HitDetectionSystem`, `BallisticShellSystem`, `MissileGuidanceSystem`, `MissileStarterSystem`, `AircraftWeaponSystem`, `DamageOverTimeSystem`, `AutoFireAbilitySystem`, `HoldFireSystem` |
| Perception | `FogOfWarSystem`, `AiTargetDetectionSystem`, `RadarSystem`, `RadarCommandSystem` |
| Orders | `MainCommandSystem`, `BaseCommandSystem`, `CommandsStateSystem`, `FirePosCommandSystem`, `AirstrikeCommandSystem`, `PrecisionStrikeCommandSystem`, `LaserDesignateCommandSystem`, `AltitudeChangeCommandSystem`, `InstantUnloadCommandSystem` |
| AI | `AiArtillerySystem`, `AircraftAiControlSystem`, `AircraftWaypointProgressTrackerSystem` |
| Logistics | `CreateSupplySystem`, `ResupplyAbilitySystem`, `ObjectiveZoneSystem` |
| Network | `NetworkEntitySystem`, `NetworkTransformSyncSystem`, `NetworkUnitCompensationSystem` |

**[inferred] Two things stand out.** Movement is split **by locomotion type** —
infantry, helicopter and a general navigation system are separate systems, not
one system with a branch, which is the ECS-idiomatic answer and the opposite of
Eugen's single `PathfindTypes` enum (`ruse.md` §11.5's WARNO reading:
`Flying / AmphibiousVehicle / Infantry / Vehicle`, four values on one code path).

And **ballistics are simulated per shell**: `BallisticShellSystem` and
`MissileGuidanceSystem` alongside `HitDetectionSystem`. R.U.S.E. resolved a shot
against a tolerance (`ToleranceTirTouche = 6000`); WARNO rolls dice
(`ruse.md` §11.4). Broken Arrow flies the projectile. That is the genre's
long-running trade in [`battle_scale.md`](battle_scale.md) — count against depth —
being spent on depth.

### 4.3 Space: a tree, again

**[TRACE]** `BrokenArrow.Shared.Ecs.BattleSystem.UnitsPositionsTree`.

**[inferred]** Not a grid and not a hash — a tree over unit positions, which puts
Broken Arrow on the same side of `ruse.md` §5's argument as Eugen, arrived at
independently. Both games have sparse units over kilometres, and both reached for
a hierarchy. That is a genuine convergence and it is worth more than either data
point alone.

---

## 5. Navigation, and a shipped bug worth learning from

**[TRACE]** The pathfinder is in-house:

```
BrokenArrow.Core.Navigation.NavigationGraph
BrokenArrow.Core.Navigation.NavigationGraph+PathFinder.FindPath(PathSearchTask, PathQuerySettings)
BrokenArrow.Core.Navigation.NavigationGraph.SortLayers(Span<T> layers)
BrokenArrow.Core.Navigation.NavigationGraph.ProcessPathTask
BrokenArrow.Core.StackList<T>
```

alongside Unity's own — `NavMeshQuery`, `NavMeshPathfindingSystem`,
`NavMeshAreas`, `NavMeshMetaLayers`, `NavAgentsPoolComponent` **[BUILD]**, with
`NavMeshAreas` appearing 3,712 times in traces.

**[inferred] So they run both**, which is exactly the position
[`nav_architecture.md`](nav_architecture.md) argues for: *there is no navigation
system, only representations and algorithms that read them.* Unity's navmesh
handles local agent movement; a bespoke layered `NavigationGraph` handles the
long-range query. `PathQuerySettings` as a distinct parameter object, and
`SortLayers` taking a `Span<T>`, say the graph is layered and the query is
configured per call rather than globally.

### 5.1 The bug

**[TRACE]** In one session log — build `v1.0.8.rc2`, 30 MB, ~37 minutes — this
exception appears **16,056 times**:

```
Exception: IndexOutOfRangeException: Index was outside the bounds of the array.
  BrokenArrow.Core.StackList`1[T].get_Item (System.Int32 index)
  BrokenArrow.Core.Navigation.NavigationGraph.SortLayers (System.Span`1[T] layers)
  BrokenArrow.Core.Navigation.NavigationGraph+PathFinder.FindPath (PathSearchTask, PathQuerySettings)
```

**Stated fairly:** this is one release-candidate build, and it is the only log of
roughly forty-eight that shows it. It is not evidence that the shipped game
does this. It *is* evidence of a real defect that reached an RC.

**[inferred] Three lessons, and the third is the one that matters here.**

An out-of-bounds read in a **hand-rolled container** (`BrokenArrow.Core.StackList`,
their own, alongside `BrokenArrow.Core.FastList`) inside the pathfinder's sort.
Custom containers get written for hot loops precisely because the standard ones
allocate or bounds-check — and then the bounds check is the thing that was
protecting you.

It is **caught and logged, sixteen thousand times, without stopping anything**.
In C# an out-of-range read throws; the frame continues; a unit silently fails to
path. In C++ the same code reads whatever was adjacent and the failure surfaces
somewhere else entirely, much later. Managed languages convert this bug class
from *undefined behaviour* into *log spam* — which is better, but only if someone
reads the log.

And **nobody read the log**. Sixteen thousand exceptions in one session is not a
rare edge case; it is a condition the pathfinder hits constantly under some map
or unit configuration. The signal was there, in a file the game itself writes to
disk, and it shipped to an RC anyway. CLAUDE.md's argument for the profiler panel
— that an unzoned system *"shows up as nothing at all"* — has a sibling here: an
error that is logged but never surfaced is the same as an error that is not
detected, and the fix is a build that is loud rather than a log that is complete.

---

## 6. Perception

**[BUILD]** `FogOfWarSystem`, `FogOfWarComponent`, `FogOfWarConfig`,
`FogOfWarConstants`, `FogOfWarFilter`, `FogOfWarHelper`, `FogOfWarTargetPoint`,
`FogOfWarWorker`, `FOWVisible`, `FOWOpaciedDistance`, `FoWDistance`,
`FOWDistanceElementPositionType`, plus `DetectedUnits`, `DetectedUnitsGroup`,
`DetectionSource`, `DetectedFireSpot`, and a `FOWTool*` family
(`FOWToolService`, `FOWToolPoints`, `FOWToolConfig`) **[TRACE]** for
`FOWToolService`.

**[inferred]** `DetectionSource` and `DetectedFireSpot` are the same idea WARNO
reached in `ruse.md` §11.5 — detection is attributed to *how* you found the
target, and firing is one of the ways. `FogOfWarWorker` implies the FoW solve is
off the main thread. `FOWOpaciedDistance` (their spelling) and `FoWDistance`
suggest a distance-attenuated visibility field rather than a binary mask.

The `FOWTool*` set is an authoring tool for fog volumes, shipped in the runtime —
consistent with §2's pattern of the editor never being fully separated.

---

## 7. Networking — the one place they diverge completely from Eugen

**[BUILD]** The component set is unambiguous:

```
NetworkUnitComponent      NetworkUnitLocalComponent      NetworkUnitRemoteComponent
NetworkSoldierLocalComponent                             NetworkSoldierRemoteComponent
NetworkAircraftLocalComponent                            NetworkAircraftRemoteComponent
NetworkMissileComponent   NetworkMissileLocalComponent   NetworkMissileRemoteComponent
NetworkSupplyComponent    NetworkSupplyLocalComponent    NetworkSupplyRemoteComponent
NetworkWeaponComponent    NetworkWeaponLocalComponent    NetworkWeaponRemoteComponent
NetworkTransformSyncComponent / NetworkTransformSyncSystem
NetworkUnitCompensationSystem
NetworkPotentialDeathComponent
NetworkEntitySystem
```

over `LiteNetLib`, with `MemoryPack` and `protobuf-net` for the wire format.

**[inferred] This is authoritative-server replication, not lockstep**, and the
**local/remote component pair per entity kind** is the giveaway: an entity is
either simulated here or mirrored from elsewhere, and which one it is is a
component you can query. `NetworkUnitCompensationSystem` is lag compensation —
the mechanism [`valve_networking.md`](valve_networking.md) §7.3 documents in full
from TF2's server.

`NetworkPotentialDeathComponent` is the detail worth keeping. **[inferred]** A
unit the client believes may have died but has not had confirmed — an explicit,
named, queryable state for "I have predicted something irreversible and am
waiting to be told I was right". Most codebases handle that with a boolean and a
comment.

**Read against `ruse.md` §9, this is the sharpest contrast in either note.**
Eugen run deterministic lockstep: every machine simulates identically, visibility
is derived per observer from one shared truth, and the price is that changing the
simulation invalidates every replay and save — which R.U.S.E.'s 2026 re-release
paid in public. Steel Balalaika replicate state and reconcile, and the price is
per-entity network components, a compensation system, and a "potential death"
state that exists only because prediction can be wrong.

**[inferred] Neither is the better answer in the abstract; the deciding variable
is entity count against simulation depth.** R.U.S.E. caps at 200 units and can
afford to simulate all of them everywhere. Broken Arrow flies individual shells
and models individual soldiers — depth that lockstep would have to reproduce
bit-identically on every machine, which is the constraint
[`networked_animation_physics.md`](networked_animation_physics.md) shows is
tractable only for small, closed scenes like Rocket League's.

---

## 8. Scripting and tooling

**[BUILD]** Two scripting systems ship, and they are for different audiences.

**Lua**, via `MoonSharp.Interpreter` — with `MoonSharp.VsCodeDebugger`, so
someone attached a real debugger to game scripts.

**A bespoke node-graph engine**, `BrokenArrow.ScriptEngine.*` **[TRACE]**, built
on `com.alelievr.NodeGraphProcessor`:

```
ScriptEngine.Core.NodeLogic          (ActivationAsync, Complete, RefreshFunctionalValues)
ScriptEngine.Core.PortCore
ScriptEngine.Core.SignalTransfer     (SendSignal, SendAllData)
ScriptEngine.Loader.NodeController
ScriptEngine.Nodes.Commands.{BaseWaypoint, NodeMoveComplex, NodeAirDrop, NodeCancelUnitCommands}
ScriptEngine.Nodes.Buildings.NodeDestroyBuilding
ScriptEngine.Nodes.Events.NodeOnTriggerEnter
ScriptEngine.Nodes.Decks.NodeChangeUnitOwner
ScriptEngine.Editor.Values.{BaseNodeValue, FlagValue}
ScriptEngine.Editor.Properties.DataCollections.{DataLinkerItem, TriggerLink}
```

plus a shipped **mission editor** — `BrokenArrow.MissionEditor.{Data, Systems,
Storage, MissionResolver, ObjectCreator}` **[TRACE]** — and user scenario folders
on disk under `Scenarios/`.

**[inferred] Compare `ruse.md` §2.4 and §8.3.** Eugen's answer was Python: a text
language, marshalled, with the AI's objective layer written in it and a regression
suite built on top. Broken Arrow's is a visual graph with typed ports and signal
transfer, aimed at mission designers and players rather than at engineers, backed
by Lua for the parts that want text.

`NodeLogic.ActivationAsync` says nodes are async — a waypoint node awaits arrival
rather than polling — which is the right shape for mission logic and is what
`UniTask` is doing in the manifest.

**[inferred]** What is conspicuously absent is any equivalent of Eugen's shipped
AI regression suite (`ruse.md` §8.3). There is a mission editor and a script
engine, which are the *ingredients*, but nothing in the identifier table or the
logs resembles `testia` / `teststrategicia` / `testequivalence`. Given §5.1, that
is not a coincidence worth ignoring.

---

## 9. What the logs say about the developer loop

**[BUILD]** Three things ship that normally would not:

- `SingularityGroup.HotReload.Runtime` — hot reload.
- `Tayx.Graphy` — an on-screen FPS/memory overlay.
- `io.sentry.unity` plus 12 support assemblies, and `crashpad_handler.exe` — full
  crash and error telemetry.
- 43 MB of `GameLogs/` written to the install directory, with **full C# stack
  traces including namespaces**.

**[inferred]** The telemetry is the modern answer to §5.1's problem, and it is the
right one: Sentry would have aggregated those 16,056 exceptions into a single
issue with a count. The lesson is not "they had no tooling" — it is that the
tooling that catches this class of bug is *aggregation with a threshold*, not
logging. A log records; a counter that trips is what makes someone look.

Also **[BUILD]**: the log's first line is `BrokenArrow v.1.0.9.1` and the
`Warning:` entries include *"The referenced script (SceneCompressor.BuildingPrefabUnpacker)
on this Behaviour is missing!"* repeated at startup — a build-pipeline class
(`SceneCompressor`) stripped from the player build while prefabs still reference
it. Harmless, noisy, and shipped.

---

## 10. Read against Eugen

The same genre, fifteen years apart, with almost no shared code.

| | **R.U.S.E. / IRISZOOM** (2010–2022) | **Broken Arrow** (2025) |
|---|---|---|
| Engine | bespoke, ~15 years of it | Unity + HDRP |
| Data language | **NDF**, a bespoke typed object language with templates and unit literals | spreadsheets (`ExcelDataReader`) + ScriptableObjects |
| Terrain texturing | `div_map` diversity field, texture groups atlased offline | **27 GB of Granite streaming virtual texture** |
| Vegetation / props | in-house impostors, in-house texture grouping, cluster impostors | GPU Instancer + Nature Renderer + ProjectDawn.Impostor |
| LOD generation | in-house bake pipeline | `UnityMeshSimplifier` |
| Spatial index | `TStreamedMeshKdTree`, three per map by purpose | `UnitsPositionsTree` **(a tree, independently)** |
| Navigation | terrain-type layer + building paths | in-house layered `NavigationGraph` **+** Unity NavMesh |
| Entity model | descriptor objects with a module list (`ruse.md` §11.7) | C# ECS (`DefaultEcs`) with per-system components |
| Ballistics | tolerance test (R.U.S.E.) → dice roll (WARNO) | **per-shell simulation** |
| Scripting | Python 2.5, marshalled | Lua + a bespoke node graph |
| Networking | **deterministic lockstep** | **replicated authority + lag compensation** |
| AI testing | **a shipped regression suite** | none found |
| Unit budget | 200 (R.U.S.E.) | not determined from the build |

**[inferred] Four readings.**

**What got bought is what got commoditised, and it is all rendering.** Every row
where Broken Arrow uses a package is a row where Eugen wrote a system: impostors,
instancing, LOD generation, virtual texturing, upscaling. Fifteen years turned
"the hard part of an RTS renderer" into a shopping list. Nothing in the
*simulation* column was bought.

**The structures converged anyway.** Both ended up with a hierarchy over unit
positions, a cell-and-instance vegetation pipeline, an impostor tier and a
layered navigation representation. The problem shape forced it. That is a useful
prior: when a bespoke engine and an off-the-shelf one arrive at the same
structure independently, the structure is probably not a matter of taste.

**The one irreducible decision is networking, and they chose oppositely.** It is
the row where no package helps, because it is determined by what you chose to
simulate. Depth pushed Broken Arrow off lockstep.

**Bought breadth costs you a floor on quality you do not control, and a much
lower one on what you must understand.** Broken Arrow ships hot reload, a
profiler overlay, a stripped build-pipeline class and a pathfinder throwing
sixteen thousand exceptions. Eugen ship a 27 KB test map whose only purpose is to
assert that the AI still captures depots. The difference is not headcount; it is
that assembling a stack quickly leaves the *seams* — the in-house code between
the packages — as the least-tested part of the product, and §5.1 is precisely a
seam.

---

## 11. What transfers here

**Take: name the "predicted but unconfirmed" state.** `NetworkPotentialDeathComponent`
is a good idea independent of networking. Anywhere this codebase acts on a
prediction that can be revoked — a queued destruction, a committed move that
validation may reject — an explicit state beats a boolean, because it is
queryable and it appears in a debug view.

**Take: split movement systems by locomotion, not by branch.** Broken Arrow's
`InfantryNavigationSystem` / `HelicopterNavigationSystem` /
`NavMeshPathfindingSystem` split is the shape
[`nav_architecture.md`](nav_architecture.md) §11 argues for, shipped. The
alternative — one system with a `PathfindTypes` switch, which is what Eugen did —
is what makes a navigation layer hard to extend later.

**Take: aggregate errors, do not log them.** §5.1 and §9. A counter that trips at
N is what turns a written log into a noticed defect. Cheap to add to the dev
panel; the profiler zone infrastructure is already the right place for it.

**Note, do not take: hand-rolled containers in hot paths.** `StackList<T>` and
`FastList<T>` exist for good reasons and one of them was the site of the bug. Our
equivalent rule is already in CLAUDE.md — *do not micro-optimise, structural not
constant-factor* — and this is a shipped example of the cost of crossing it.

**Do not take: two ECS frameworks, two serialisers, and a package for everything.**
`DefaultEcs` plus `Leopotam` with only one running, `MemoryPack` plus
`protobuf-net`, Sentry's 13 assemblies. Each was individually reasonable. The
aggregate is 187 assemblies and a build nobody holds in their head, which is the
mechanism by which a stripped `SceneCompressor` reference and a screaming
pathfinder both survive to release.

---

## 12. Reproducing this, and what is not established

No new tools were needed. The IL2CPP identifier table extracts with the header at
offset 0 — magic `0xFAB11BAF`, version 31 — where the fourth `(offset, size)`
pair after the version field locates the identifier blob, a run of
null-terminated ASCII:

```python
d = open('global-metadata.dat','rb').read()
off, size = struct.unpack_from('<ii', d, 8 + 2*8)   # field 4: identifier strings
names = [s for s in d[off:off+size].split(b'\x00') if s]   # 203,203 of them
```

Everything else is `ls`, `cat` on `boot.config` / `ScriptingAssemblies.json`, and
`grep` over `GameLogs/`.

**Not established, and worth saying explicitly:**

- **No map sizes, unit counts or performance figures.** Nothing in the readable
  layer carries them, and I did not run the game. Every scale comparison with
  `ruse.md` §1 is therefore one-sided.
- **The identifier table proves compilation, not use.** §4.1 is the worked
  example of how badly that can mislead; treat every **[BUILD]**-only claim here
  as "this package is present", not "this is how the game works".
- **`data.unity3d` (6.8 GB) was not opened.** Scenes, prefabs, ScriptableObjects
  and the actual unit database are in there. AssetStudio or AssetRipper would
  read it and would move most of §4 and §6 from inference to evidence — that is
  the obvious next step if this note is ever worth extending.
- **The 16,056 exceptions are one release-candidate session.** They are real and
  they are quoted exactly; they are not a claim about the shipping build.
- **No render-pass detail.** The HDRP frame was not captured, so §3 describes
  what streams and what instances, not how the frame is composed. That is the
  half `ruse.md` §4 has for Eugen and this note does not have for Broken Arrow.

---

## Sources

**Read directly**
- The retail Broken Arrow install, `BrokenArrow v.1.0.9.1` —
  `BrokenArrow_Data/{boot.config, app.info, ScriptingAssemblies.json, StreamingAssets/, il2cpp_data/Metadata/global-metadata.dat, Plugins/x86_64/}`
  and `GameLogs/` (48 logs, 43 MB, builds 1.0.8.rc2 through 1.0.9.1).

**Format references**
- IL2CPP metadata layout — [Il2CppDumper](https://github.com/Perfare/Il2CppDumper), `Il2CppGlobalMetadataHeader`
- Granite `.gts`/`.gtp` — Unity Streaming Virtual Texturing, magic `GRPG`

**Packages named in the build** (for what each solves)
- [DefaultEcs](https://github.com/Doraku/DefaultEcs), [LeoEcsProto](https://gitverse.ru/leopotam/ecsproto),
  [GPU Instancer](https://assetstore.unity.com/packages/tools/utilities/gpu-instancer-117566),
  [Nature Renderer](https://assetstore.unity.com/packages/tools/terrain/nature-renderer-153552),
  [UnityMeshSimplifier](https://github.com/Whinarn/UnityMeshSimplifier),
  [LiteNetLib](https://github.com/RevenantX/LiteNetLib),
  [MemoryPack](https://github.com/Cysharp/MemoryPack), [UniTask](https://github.com/Cysharp/UniTask),
  [MoonSharp](https://www.moonsharp.org/), [NodeGraphProcessor](https://github.com/alelievr/NodeGraphProcessor),
  [VContainer](https://github.com/hadashiA/VContainer)

**Related notes**
- [`ruse.md`](ruse.md) — the bespoke-engine counterpart; §10 here is the comparison
- [`nav_architecture.md`](nav_architecture.md) — representations versus algorithms, which §5 is a shipped instance of
- [`valve_networking.md`](valve_networking.md) — the replication model §7 belongs to
- [`lod_systems.md`](lod_systems.md) — the four meanings of LOD; §3.2 uses three of them
- [`battle_scale.md`](battle_scale.md) — count against depth, which §4.2's per-shell ballistics spends
