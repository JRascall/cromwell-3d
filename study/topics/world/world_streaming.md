# "Infinite" worlds — how they are actually divided and paged

Unreal's World Partition read from the engine's own C++, Source 2's spawn groups
read from its network protocol, and the one coupling everybody discovers late:
**streaming and LOD are the same problem.**

Companion to [`terrain_rendering.md`](terrain_rendering.md), which is the other
half of this question — once a tile is resident, how is it shaded and vegetated.

## Sourcing

Unusually good, because Epic ship the engine source with the binary installs:

| Tag | Meaning |
|---|---|
| **[UE-SRC]** | Read from `C:/Program Files/Epic Games/UE_5.7/Engine/Source/`. Class names, fields and constants are Epic's |
| **[VALVE-PROTO]** | Source 2 network schemas dumped from shipping binaries — see [`valve_networking.md`](../../games/valve/valve_networking.md) for why this is trustworthy |
| **[CIG]** | Cloud Imperium's technical statements |
| **[inferred]** | My reasoning |

UE 5.4, 5.6 and 5.7 are installed locally; **5.7** is the one read here.

---

## 1. "Infinite" is always a lie, and the interesting part is which lie

No shipped game streams an infinite world. What they do is one of three things,
and knowing which you are building matters more than any implementation detail:

| Approach | What is infinite | Example |
|---|---|---|
| **Bounded world, paged** | Nothing. The world is large and finite; only *residency* is dynamic. | Unreal World Partition, almost every open-world game |
| **Procedurally generated on demand** | The *content*, generated from a seed as you approach. | Elite Dangerous, No Man's Sky — see [`elite_dangerous.md`](../../games/space/elite_dangerous.md) |
| **Editable volume, paged** | Neither, but the *representation* is unbounded in principle. | Space Engineers — see [`voxel_terrain.md`](voxel_terrain.md) |

**Unreal's answer is firmly the first**, and the code says so: World Partition's
grid is built from `WorldBounds`, a finite box (§2.1). If you want the second,
Unreal gives you the paging machinery and you supply the generator.

---

## 2. Unreal's World Partition

### 2.1 It is a quadtree pyramid, and the code is short enough to quote

The entire spatial structure is `FSquare2DGridHelper` — *"Square 2D grid
helper"* in Epic's own comment — holding `TArray<FGridLevel> Levels`
**[UE-SRC]**. Construction:

```cpp
const double WorldBoundsMaxExtent = FMath::Max(DistMin.GetMax(), DistMax.GetMax());
GridSize = 2 * FMath::CeilToDouble(WorldBoundsMaxExtent / CellSize);
if (!FMath::IsPowerOfTwo(GridSize))
{
    GridSize = FMath::Pow(2, FMath::CeilToDouble(FMath::Log2((double)GridSize)));
}
GridLevelCount = FMath::FloorLog2_64(GridSize) + 1;
```

`Engine/Source/Runtime/Engine/Private/WorldPartition/RuntimeSpatialHash/RuntimeSpatialHashGridHelper.cpp:42-67`
**[UE-SRC]**

So: a **power-of-two square grid**, with `log2(GridSize) + 1` levels — a full
quadtree from the authored cell size up to a single root cell. Cell lookup is
plain arithmetic, no tree walk **[UE-SRC]**:

```cpp
FMath::FloorToInt(((InPos.X - Origin.X) / CellSize) + GridSize * 0.5)
```

Two accessors give the shape away:

```cpp
inline FGridLevel& GetLowestLevel()      { return Levels[0]; }
inline FGridCell&  GetAlwaysLoadedCell() { return Levels.Last().GetCell(FGridCellCoord2(0,0)); }
```

**[UE-SRC]** — `Levels[0]` is the finest grid; the top level's single cell is
the **always-loaded cell**. Anything that must never unload lives there, and it
is not a special case in the code — it is just the cell that covers everything.

**Why a pyramid rather than one grid.** An actor is assigned to the level whose
cells are big enough to contain it. A rock goes in a fine cell; a mountain range
or a mission-critical volume goes in a coarse one. Without the pyramid, a large
actor either spans many cells (and must be loaded by all of them) or forces the
whole grid to use huge cells. [inferred — this is the standard motivation and
the `GetAlwaysLoadedCell` / level-assignment structure matches nothing else, but
Epic do not state it in these files.]

### 2.2 Loading range, and that it is a runtime knob

Each streaming grid carries a `LoadingRange` **[UE-SRC]**, and it is
overridable live:

```
wp.Runtime.OverrideRuntimeSpatialHashLoadingRange   grid=N range=X
```

`WorldPartitionRuntimeSpatialHash.cpp:132-197` **[UE-SRC]**. Note it is
explicitly forwarded to the server too (`ServerExecConsoleCommand`), because
streaming radius is not purely a client concern.

**A game can have several grids with different cell sizes and ranges** —
`GridName`, `GridIndex`, per-grid `CellSize` and `LoadingRange`. That is the
important design affordance: buildings, foliage and gameplay volumes do not
want the same granularity, and the answer is not one tuned compromise but
several grids.

### 2.3 Data layers cut across cells

A cell is not the unit of streaming. `FGridCellDataChunk` subdivides a cell by
**`DataLayersID`** and **`ContentBundleID`** **[UE-SRC]**:

```cpp
FGridCellDataChunk(const TArray<const UDataLayerInstance*>& InDataLayers, const FGuid& InContentBundleID)
{
    Algo::TransformIf(InDataLayers, DataLayers,
        [](const UDataLayerInstance* DataLayer) { return DataLayer->IsRuntime(); }, ...);
    DataLayersID = FDataLayersID(DataLayers);
}
```

So one spatial cell produces **one chunk per distinct combination of runtime
data layers** present in it. That is what lets a game load the same square
kilometre in a "day" configuration or a "post-quest destruction" configuration
without duplicating the grid — and it is why the count of *streaming cells* can
far exceed the count of *grid cells*.

[inferred] The cost is combinatorial: N independent runtime data layers in one
cell can produce up to 2^N chunks. Data layers are cheap to add and expensive
to overlap, which is exactly the kind of thing that is invisible until content
is built.

### 2.4 The rest of the system, briefly

- **HLOD** — `WorldPartition/HLOD/`, plus `LandscapeHLODBuilder.cpp`
  **[UE-SRC]**. Proxy meshes standing in for unloaded cells, so the horizon
  exists without its actors. Without this, a paged world visibly pops into
  being at the loading range.
- **One File Per Actor** — `ActorDescContainer`, `WorldPartitionActorDesc*`
  **[UE-SRC]**. Each actor is its own file with a lightweight *descriptor*
  (bounds, class, data layers) that can be read **without loading the actor**.
  That is what makes the streaming grid buildable at all: you need every
  actor's bounds to assign it to a level, and you cannot load a whole world to
  find out.
- **Level Instances / Packed Level Actors** — `LevelInstance/`,
  `PackedLevelActor/` **[UE-SRC]**. Reusable authored chunks, the modern
  replacement for sub-levels.
- **Landscape integration** — `WorldPartition/Landscape/` **[UE-SRC]**, because
  landscape is one enormous actor and had to be taught to split.

### 2.5 What it replaced, and why that matters

World Partition superseded **World Composition** and hand-authored **sub-level
streaming**, where a designer decided which levels loaded when. The change is
not that the new system streams better; it is that **the partition is derived
from actor bounds rather than authored by hand**.

[inferred] That is the same move as everything else in this study directory:
the win is deleting a manual step that humans get wrong at scale, not making
the runtime faster. `ActorDesc` exists so the derivation is cheap; `bUseAlignedGridLevels`
and `GRuntimeSpatialHashPlaceSmallActorsUsingLocation` **[UE-SRC]** exist
because the derivation needs tuning knobs once you stop hand-placing things.

---

## 3. Source 2 — spawn groups, and they are a *network* concept

Source 2 has no World Partition equivalent that is publicly visible. What it has
is **spawn groups**, and the evidence is unusual: they show up in the *network
protocol* **[VALVE-PROTO]**, from `networkbasetypes.proto`:

```
CNETMsg_SpawnGroup_Load               (max 131 KB)
CNETMsg_SpawnGroup_ManifestUpdate     (max 2 KB)
CNETMsg_SpawnGroup_SetCreationTick
CNETMsg_SpawnGroup_Unload
CNETMsg_SpawnGroup_LoadCompleted
SpawnGroupFlags_t
```

And in `CSVCMsg_PacketEntities` **[VALVE-PROTO]**:

```
optional uint32 active_spawngroup_handle = 9;
optional uint32 max_spawngroup_creationsequence = 10;
```

Read those together and the architecture is legible [inferred, but the message
set constrains it tightly]:

- **A spawn group is the unit of world loading** — a section of world with
  positioning, loaded and unloaded as a unit.
- **The server drives it.** `CNETMsg_SpawnGroup_Load` is a *net* message, not a
  client-local operation, and the client acknowledges with `LoadCompleted`.
- **It is tick-stamped.** `SetCreationTick` and `max_spawngroup_creationsequence`
  mean entity replication is aware of which spawn group generation an entity
  belongs to — so entity IDs and world residency are reconciled explicitly
  rather than hoped about.
- **There is a manifest**, updatable separately from the content.

**This is a genuinely different position from Unreal's.** Unreal's streaming is
fundamentally a client-side residency question driven by streaming sources, with
the server kept in sync (§2.2's `ServerExecConsoleCommand` is a hint of the
friction). Source 2's is a **replicated, server-sequenced** operation from the
start.

[inferred] For a multiplayer engine that is the better default, because the
failure mode Unreal fights — a client that has not finished streaming the area
an entity just spawned in — is designed out rather than handled.

**What Source 2 does *not* have is a heightfield terrain system.** s&box, built
on Source 2, ships its own `.terrain` asset type **[observed on disk]** — which
is the tell that Facepunch had to add one. Source 2's worlds are meshes, and
that decision propagates into §2 of [`terrain_rendering.md`](terrain_rendering.md).

---

## 4. Star Citizen, in one paragraph

Covered in [`mmo_architecture.md`](../scale/mmo_architecture.md) §5, but the streaming
half belongs here: **Persistent Entity Streaming** means entity state lives in
the Replication Layer's EntityGraph rather than in a server's memory, so
"streaming" and "persistence" are the same system **[CIG]**. An object left on a
moon is not saved out and reloaded — it was never owned by the thing that
unloaded.

That is the most radical position of the three, and the one with the clearest
statement of what streaming *is*: not "which files are in memory" but "which
entities is this node authoritative for".

---

## 5. The coupling everyone finds late

**You cannot render detail you have not streamed**, and the honest systems make
that explicit rather than hoping the two stay in step.

Unreal's landscape does, in four lines **[UE-SRC]**:

```cpp
float FLandscapeComponentSceneProxy::ComputeLODBias() const
{
    float ComputedLODBias = 0;
    if (HeightmapTexture)
    {
        if (const FTexture2DResource* TextureResource = (const FTexture2DResource*)HeightmapTexture->GetResource())
        {
            ComputedLODBias = static_cast<float>(TextureResource->GetCurrentFirstMip());
        }
    }
    return ComputedLODBias;
}
```

`Landscape/Private/LandscapeRender.cpp:4503-4516`

**The mesh LOD is clamped by which heightmap mip is currently resident.** If
texture streaming has only delivered mip 3, the terrain renders at the
corresponding coarse LOD — not at the LOD the camera distance would ask for.
The geometry cannot be more detailed than the data it is built from, and rather
than let that produce a glitch, the renderer reads the streaming state directly
and lowers its ambition.

[inferred] Generalised: **anything with both a streamed data source and a
distance-based LOD needs the LOD to be a function of *both*.** The bug you get
otherwise is not a crash — it is intermittent visual popping under memory
pressure that does not reproduce on a dev machine with everything resident. That
is the worst class of bug to find late, and the fix is four lines if you design
for it.

---

## 6. What `cromwell` should take

This project's world is a **tile grid with storeys**, finite and modest. Most of
World Partition is not needed and would be actively wrong to build. But four
ideas are cheap and general:

1. **Derive the partition from content bounds; do not author it.** §2.5. The
   prerequisite is a lightweight descriptor readable without loading the thing
   it describes (`ActorDesc`) — that is the piece to design in early, because
   retrofitting "know an object's bounds without loading it" means touching
   every asset type.

2. **Arithmetic cell lookup over a tree walk.** §2.1 — Unreal's grid is a
   power-of-two pyramid indexed by `floor((pos - origin)/cellSize + gridSize/2)`.
   This codebase already does exactly this in `OccupancyGrid` and `SpatialHash`,
   so the lesson is really a confirmation: the same structure scales from a
   tile map to a 100 km² world, and CLAUDE.md's "keep a running index" applies
   unchanged.

3. **Several grids, not one tuned compromise.** §2.2. Different content classes
   want different cell sizes and ranges. One grid with an averaged cell size is
   the decision that looks simplest and ages worst.

4. **Make LOD a function of residency, explicitly.** §5. Four lines, and it
   converts a class of intermittent bug into a deterministic one. Worth doing
   the first time anything in this engine is both streamed and LOD'd — which,
   given the sun bake and the storey system, is plausible sooner than a
   streaming world is.

**And the thing not to take:** a data-layer system (§2.3) before there is
content that needs it. Its cost is combinatorial and paid by whoever authors
the world, not by the code.

---

## Sources

**[UE-SRC]** — Unreal Engine 5.7, `C:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/`:

| Area | Path |
|---|---|
| Grid pyramid | `Engine/Private/WorldPartition/RuntimeSpatialHash/RuntimeSpatialHashGridHelper.cpp`, `Engine/Public/.../RuntimeSpatialHashGridHelper.h` |
| Streaming grids, loading range | `Engine/Private/WorldPartition/WorldPartitionRuntimeSpatialHash.cpp` |
| Newer hash | `Engine/Private/WorldPartition/RuntimeHashSet/` |
| Actor descriptors (OFPA) | `Engine/Private/WorldPartition/WorldPartitionActorDesc*.cpp` |
| Data layers, HLOD, level instances | `Engine/Private/WorldPartition/{DataLayer,HLOD,LevelInstance,PackedLevelActor}/` |
| LOD/streaming coupling | `Landscape/Private/LandscapeRender.cpp` |

**[VALVE-PROTO]** — `SteamDatabase/GameTracking-CS2/Protobufs/`:
`networkbasetypes.proto` (spawn group messages), `netmessages.proto`
(`CSVCMsg_PacketEntities` spawn group fields). See
[`valve_networking.md`](../../games/valve/valve_networking.md) for provenance.

**[CIG]** — [Server Meshing and Persistent Streaming Q&A](https://api.star-citizen.wiki/comm-links/18397).

**Related notes:** [`terrain_rendering.md`](terrain_rendering.md) — the shading
half; [`mmo_architecture.md`](../scale/mmo_architecture.md) §5 — Star Citizen;
[`elite_dangerous.md`](../../games/space/elite_dangerous.md) — generated rather than paged;
[`voxel_terrain.md`](voxel_terrain.md) and [`space_engineers.md`](../../games/space/space_engineers.md)
— editable volume; [`valve_networking.md`](../../games/valve/valve_networking.md) — where the
Source 2 protobuf evidence comes from.
