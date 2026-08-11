# Level of detail — geometry, aggregation, skeletons, and AI

How **Source 2**, **Unreal**, **Total War** and **Hitman** decide what to draw
and what to think about, how the lower detail levels are *generated*, which
libraries do the generating and under what licence, where vertex animation
textures actually fit, and — §7, the longest section — why *"smarter AI closer to
the camera"* is the wrong axis and what the right one is.

Companion to [`crowd_scale.md`](../scale/crowd_scale.md), which is the same question
answered from the *agent* side (AC Unity's 40 real AI behind 10,000 bodies, its
300-vs-11 bone split, L4D's Active Area Set), and to
[`battle_scale.md`](../scale/battle_scale.md), which is Total War's simulation. This note
is the *rendering and representation* side, and it deliberately does not repeat
those.

| Tag | Meaning |
|---|---|
| **[S2-ASSET]** | Read directly out of shipped Source 2 files or binaries on this machine — class names, fields and values are as found |
| **[TW-ASSET]** | Parsed out of a Total War `.pack` on this machine (see §4.0 for the caveat) |
| **[EPIC]** | Epic's documentation, release notes or published talks |
| **[PAPER]** | A published paper or book chapter, read in full |
| **[NVIDIA]** / **[MESHOPT]** | Vendor / library documentation and release notes |
| **[COMMUNITY]** | Modding wikis, developer blogs, forum posts |
| **[inferred]** | My reasoning, including arithmetic I did myself |

---

## 0. The one framing that makes the rest simple

**"LOD" is four unrelated problems that happen to share a name.** Almost every
confused LOD discussion is two people solving different ones.

| # | Problem | The cost it attacks | What it swaps |
|---|---|---|---|
| 1 | **Geometry LOD** | vertices and pixels shaded | a mesh for a cheaper mesh |
| 2 | **Aggregation / hierarchical LOD** | *draw calls and state changes* | many objects for one object |
| 3 | **Deformation LOD** | bone transforms, skinning ALU, animation evaluation on the CPU | a rig for a cheaper rig, or for no rig |
| 4 | **Simulation / AI LOD** | game-thread time | thinking every frame for thinking rarely |

They fail differently, they are tuned by different people, and — the part that
matters — **they hit their walls in a different order depending on genre**.

- A **first-person** game with a handful of characters and a dense environment
  hits problem 1 first. That is the problem Nanite was built for.
- An **RTS or a battle game** hits problem 2 first and problem 3 second. Its
  triangles are individually cheap and there are simply thousands of *things*.
  Total War's answer (§4) is almost entirely problem 2, and its geometry LOD
  scheme is deliberately unremarkable.
- A **tactics game** — this project — hits problem 4 first if it hits anything,
  because the unit count is tens and the interesting cost is per-cell queries,
  not per-vertex work. See `navigation.md` §10–11 for the same conclusion
  arrived at from the simulation side.

Everything below is sorted by which of the four it addresses, and §10 is what
this codebase should actually do about it.

There is also a cross-cutting fifth question that turns out to be the most
interesting one — **what number do you compare against the threshold?** §8.

---

## 1. Source 2, read from the binaries

Source 2's model LOD system is not documented by Valve. What follows is read out
of **s&box** — Facepunch's game built on Source 2 — at
`E:/SteamLibrary/steamapps/common/sbox/`, which ships the Source 2 tools
(`modeldoc_editor.dll`, `hammer.dll`), the engine DLLs, and **uncompiled
plain-text KV3 model files including Valve's production Citizen model**. Same
material that [`source2_animation.md`](../../games/valve/source2_animation.md) is built from.

**The caveat, stated once.** These binaries are built from Facepunch's fork —
the embedded source paths read `C:\actions-runner\_work\sbox\sbox\src\tools\...`
**[S2-ASSET]**. The *schema* is unambiguously Valve's: `LODGroup_t`,
`CModelDocLODGroup`, `m_lodGroupSwitchDistances` and the bone-LOD flags appear
in `engine2.dll` and `meshsystem.dll` as well as the tools. Whether the
*auto-simplification* front end (§1.2) is Valve's or Facepunch's addition I
cannot tell from this machine, and I say so again where it matters.

### 1.1 The LOD group — a mesh *list* per level, not a mesh per level

The unit is `LODGroup`, held in a `LODGroupList` node in the `.vmdl`. Here is
Valve's production player model's actual LOD chain, in full, from
`addons/citizen/Assets/models/citizen/prefabs/citizen_lodgrouplist.vmdl_prefab`
**[S2-ASSET]**:

| LOD | `switch_threshold` | meshes |
|---|---|---|
| 0 | `0.0` | `Head_LOD0`, `Torso_LOD0`, `Hands_LOD0`, `Legs_LOD0`, `Feet_LOD0` |
| 1 | `5.0` | **`Head_LOD0`**, `Torso_LOD1`, `Hands_LOD1`, `Legs_LOD1`, `Feet_LOD1` |
| 2 | `20.0` | `Head_LOD2`, `Torso_LOD2`, `Hands_LOD2`, `Legs_LOD2`, `Feet_LOD2` |
| 3 | `40.0` | `Head_LOD3`, `Torso_LOD3`, `Hands_LOD3`, `Legs_LOD3`, `Feet_LOD3` |
| 4 | `70.0` | `Head_LOD4`, **`Torso_LOD3`, `Hands_LOD3`, `Legs_LOD3`, `Feet_LOD3`** |

Three things worth taking, all of which fall straight out of "a level is a *set
of meshes*, chosen independently per body part":

- **Levels are not uniform across the model.** LOD1 keeps the full-detail head
  and drops everything else a step. LOD4 drops the head *again* and keeps LOD3
  for everything else. The chain is not a single decimation ladder; it is a
  per-part schedule.
- Valve's own comments say why. LOD1: *"Note that LOD1 still uses the LOD0 head
  (for now!)"*. LOD2: *"LOD2 is when the head switches to a lower-poly version
  **without morphs**"* **[S2-ASSET]**. **The head is held at full detail for two
  levels because the face carries the morph targets** — dropping the head is not
  a triangle decision, it is a decision to stop supporting facial animation. It
  is filed as geometry LOD and is really deformation LOD (§0, problem 3).
- The list is authored as a **shareable prefab** (`.vmdl_prefab`), referenced by
  `citizen.vmdl` and by the human variants. One LOD schedule, many models.

A sibling class `LODGroupAll` exists, described as *"A group of meshes that are
present in all lod levels"* **[S2-ASSET]** — the escape hatch for something that
must never be swapped.

### 1.2 AutoLOD — Source 2's generator is meshoptimizer

`modeldoc_editor.dll` contains the string **`meshoptimizer`**, twice, in tooltip
text **[S2-ASSET]**:

> *"`auto_reduction` — meshoptimizer target ratio: fraction of triangles to keep
> (1.0 = no simplification, 0.5 = half)."*
>
> *"`auto_max_error` — meshoptimizer `target_error`: maximum allowed relative
> geometric error, normalized by mesh extent…"*

Every exposed knob maps one-to-one onto a documented meshoptimizer feature. The
left column is verbatim from the DLL **[S2-ASSET]**; the right is from
meshoptimizer's own v1.0 notes **[MESHOPT]**:

| ModelDoc attribute | UI label | meshoptimizer counterpart |
|---|---|---|
| `m_flReduction` | *"Fraction of triangles to keep (1.0 = no simplification, 0.5 = half)"* | `target_index_count` ratio |
| `m_flMaxError` | relative geometric error, normalised by mesh extent | `target_error` |
| `m_bLockBorderVertices` | *"Restricts the simplifier from collapsing edges that are on the border of the mesh… so that the LODs can be combined without introducing cracks"* | `meshopt_SimplifyLockBorder` |
| `m_bProtectUVSeams` | *"UV seam vertices are protected… Disabling allows the simplifier to collapse across UV seams for more aggressive reduction"* | attribute-aware simplify / `vertex_lock` |
| `m_bPermissiveSimplification` | *"Allow the simplifier to collapse edges across normal/hard-edge discontinuities… may lose hard edge detail"* | permissive mode (new in v1.0) |
| `m_bPruneIsolatedComponents` | *"remove isolated components regardless of topological restrictions inside the component"* | `meshopt_SimplifyPrune` |
| `m_nRegularize` | *"Produces more regular triangle sizes and shapes… **This can improve geometric quality under deformation such as skinning**. Light uses a smaller regularization factor"* | `meshopt_SimplifyRegularize` |
| `m_bStripVertexColor` | strip vertex colour before simplification, for meshes where *"decimation would otherwise stretch"* a small blend region across the mesh | (pipeline step, not a library flag) |
| `m_bMaterialCullingEnabled` / `LODGroupReplacements` | *"Drop specific materials at this LOD level: replace their faces with another material, or remove them… to bring down draw calls"* | — |

Two of these deserve calling out because they are the difference between a
simplifier that works on characters and one that does not:

- **`m_nRegularize` is there for skinning.** A quadric simplifier left alone
  produces long thin slivers; slivers under skinning deformation shear
  visibly. Trading geometric accuracy for triangle regularity is *the right
  trade for a skinned mesh and the wrong one for a static prop* — which is why
  it is a per-group toggle with a "Light" setting rather than a global.
- **`m_bLockBorderVertices` exists so that mesh subsets can be simplified
  independently and still fit together.** That is exactly the Citizen model's
  problem: `Torso` and `Legs` are separate meshes, decimated separately, and
  must not develop a gap at the waist. It is also, at a different scale,
  precisely Nanite's group-boundary trick (§4.2). **The same idea shows up at
  the mesh-part level and at the 128-triangle-cluster level.**

Beyond the knobs, the mode enum **[S2-ASSET]**:

| Mode | Meaning |
|---|---|
| *Manual (No Simplification)* | *"You specify meshes for this LOD level."* |
| *Auto Simplify (Mesh List)* | *"Simplifies this group's own mesh list (starts a new chain)."* |
| *Auto Simplify (Inherited from Chain)* | *"Always simplifies from the last manual LOD's original meshes to avoid compounding quality loss."* |

That last one is the non-obvious engineering. **Chained simplification —
LOD2 generated from LOD1 which was generated from LOD0 — compounds error.** The
inherited mode re-simplifies from the nearest hand-authored source every time,
so error is measured once against real geometry rather than accumulating. Any
LOD baker should do this; it costs nothing and it is easy to get wrong.

There is also a one-button *"Automatically create LOD groups 1-4 with recommended
settings"* **[S2-ASSET]** — the default ladder is four generated levels.

### 1.3 Skeleton LOD is in the model format, not bolted on

`meshsystem.dll` and `engine2.dll` both carry **[S2-ASSET]**:

```
ModelSkeletonData_t::FLAG_BONE_USED_BY_VERTEX_LOD0
... through ...
ModelSkeletonData_t::FLAG_BONE_USED_BY_VERTEX_LOD6
```

Seven bits, one per LOD level, per bone. The compiled skeleton records **which
LODs each bone is actually skinned to**, which lets the runtime skip evaluating
and uploading bones that no visible mesh reads. This is Source 1's
`$lod`/`bonetreecollapse` idea promoted into a per-bone bitfield in the
resource.

**The lesson is the placement, not the mechanism.** It is a field in the model
format, computed at compile time from the LOD mesh lists. A renderer that did
not reserve room for it would be retrofitting a per-bone mask into a shipped
skeleton format later — expensive, and exactly the class of decision
`nav_architecture.md` §10 calls "free now, expensive later".

### 1.4 Runtime controls, and one detail worth stealing

From `engine2.dll` **[S2-ASSET]**:

| Symbol | What it is |
|---|---|
| `m_lodGroupSwitchDistances` | the compiled thresholds, on the mesh resource |
| `m_refLODGroupMasks` | which mesh belongs to which LOD levels, as a bitmask |
| `sc_force_lod_level` | pin every scene object to a level |
| `sc_lod_distance_scale_override` | global multiplier — this is the scalability knob |
| `m_nLODOverride` | per-scene-object pin |
| `LOD Level: %d Draw Calls: %d Triangles: %d` | the debug readout, per level |
| *"Child models inherit parent LOD number"* / `sbox_feature_inheritlods` | see below |

**"Child models inherit parent LOD number"** is the detail. Source 2 characters
are bone-merged assemblies — a body, a head, clothing, a weapon, each a separate
model. If each picked its own LOD independently from its own bounds, a
character's shirt would swap detail at a different distance from the torso
underneath it, and the seams would open. Forcing children to take the parent's
level makes the assembly switch as one object. Anything with attachable parts
needs this rule, and it is the sort of thing you only discover by shipping.

Note also the shape of `sc_lod_distance_scale_override`: **one global scalar over
authored thresholds**. Quality settings do not re-tune content; they scale one
number. Same for texture: `r_texture_lod_scale`, `r_fallback_texture_lod_scale`.

### 1.5 The world side: aggregates, not hierarchical LOD

For the environment, Source 2's lever is **aggregation** (§0, problem 2) rather
than a generated HLOD pyramid. `engine2.dll` carries `CAggregateSceneObject`,
`CAggregateSceneObjectDesc`, `AggregateSceneObject_t`, `AggregateMeshInfo_t`,
`m_aggregateMeshes`, `m_aggregateSceneObjects` and the debug toggle
`sc_draw_aggregate_meshes` — *"Toggles drawing of aggregate meshes"*
**[S2-ASSET]**. Many static props are compiled into one scene object with a list
of mesh instances, so the renderer sets up state once for a whole neighbourhood
of props.

There is a separate compiler for the baked case — `bakedlodbuilder.dll`,
interface `BakedLODBuilderMgr001`, referenced from the model tools and the
resource compiler, containing `RenderBakedLODAtlas` **[S2-ASSET]**. A baked LOD
step that renders into an *atlas* is impostor-shaped: capture the thing once,
draw the capture afterwards. I could not get further than the symbol names from
this machine, so I am not going to claim more than that.

On the authoring side, `prop_static` exposes *Auto* LOD behaviour or an explicit
level, plus *Start Fade Dist* / *End Fade Dist*, and the Valve wiki warns that
props with LODs *"take up more room in the lightmap and do not merge"*
**[COMMUNITY]** — i.e. **giving a prop a LOD chain disqualifies it from
aggregation**, and for a small prop the aggregation is worth more than the
triangle saving. That trade — *problem 1 and problem 2 are in direct conflict* —
is the single most useful thing on this page for an RTS.

---

## 2. The generators: what actually does the simplification

### 2.1 meshoptimizer is the answer, and it is not close

**[MESHOPT]** MIT-licensed, C++11 with a C API, no dependencies, header + a
handful of `.cpp` files. It is what Source 2's ModelDoc calls (§1.2), what Godot
uses for automatic LOD, what glTF tooling standardised on, and what the two
open Nanite-alikes below are built out of.

The simplification surface, as of v1.0:

| Function / flag | What it does |
|---|---|
| `meshopt_simplify` | quadric edge-collapse to a target index count **or** a target error, whichever binds first |
| `meshopt_simplifyWithAttributes` | the same, weighting UVs / normals / colours so shading survives — **this is the one to use on anything textured** |
| `meshopt_SimplifyLockBorder` | never collapse mesh-border edges — lets subsets be simplified independently and reassembled without cracks |
| `vertex_lock` array | per-vertex locking, finer than the border flag; the cluster-LOD builders live on this |
| `meshopt_SimplifySparse` | for many small subsets: avoids O(total vertices) setup per call. Turns a cluster pipeline from quadratic-feeling to linear |
| `meshopt_SimplifyErrorAbsolute` | error in world units rather than normalised to mesh extent — required if you want a *screen-space* error metric later |
| `meshopt_SimplifyPrune` | drop isolated components that have become sub-pixel junk |
| `meshopt_SimplifyRegularize` | prefer regular triangles over minimal error — for skinned meshes (§1.2) |
| permissive mode | allow collapses across attribute discontinuities when the target is otherwise unreachable |
| `meshopt_buildMeshlets`, `meshopt_buildMeshletsSpatial`, `meshopt_partitionClusters` | clusterisation and cluster grouping — the inputs to a DAG builder |

The interesting historical note: **`meshopt_SimplifySparse`, per-vertex locking
and prune/regularize were added specifically to support Nanite-style
pipelines** **[MESHOPT]**. The library grew the features that cluster-LOD needed,
which is why the two open implementations below are thin.

### 2.2 Open, continuous, Nanite-shaped

| Project | What it is | Licence |
|---|---|---|
| **`clusterlod.h`** (meshoptimizer demo) | single header, *"generates a hierarchy of clusters that are progressively grouped and simplified, similarly to Nanite"* **[MESHOPT]**; written to be read and modified, not just linked | MIT |
| **`nvpro-samples/nv_cluster_lod_builder`** | NVIDIA's continuous-LOD mesh library — precomputes clusters that *"can be seamlessly combined across LOD levels"* **[NVIDIA]** | Apache-2.0-ish nvpro terms |
| **`nvpro-samples/vk_lod_clusters`** | the full renderer sample: streaming RAM→VRAM on demand, mesh-shader rasterisation, and ray tracing via `VK_NV_cluster_acceleration_structure`. Explicitly *"inspired by the Nanite rendering system"* **[NVIDIA]** | as above |
| **RTX Mega Geometry (RTXMG)** | NVIDIA's productised version of the same, aimed at ray tracing cluster BLASes | NVIDIA SDK terms |

So: **a continuous cluster-LOD system is now an off-the-shelf component**, in a
way it was not when Nanite shipped in 2021. If you want Nanite's *idea* you can
have it without Unreal.

### 2.3 What generation costs — the number nobody quotes

From meshoptimizer's author, building the cluster DAG for NVIDIA's Zorah scene
**[MESHOPT]**:

| | |
|---|---|
| Input | **1.64 billion triangles** |
| Baseline | ~7–9 minutes, 16 threads, **54–57 GB RAM** |
| After optimisation | **~2 m 35 s**, same 16 threads |
| End to end in `vk_lod_clusters`, including serialising | ~3 m 20 s → a **62 GB** cache file |

Parallelism is **per mesh**, not inside a mesh — different meshes on different
threads, sorted largest-first for load balance, reaching ~1574% CPU on 16
threads **[MESHOPT]**.

Two things follow. First, **the build is memory-bound before it is CPU-bound** —
54 GB for 1.6 B triangles is ~33 bytes per triangle of peak working set, and
that is the constraint that decides whether this fits in your asset pipeline.
Second, **the output is bigger than the input** — a virtual-geometry
representation is a cache, and the 62 GB file is the honest price of not
authoring LODs by hand.

### 2.4 The rest of the field

| Option | Note |
|---|---|
| **CGAL Surface Mesh Simplification** | edge collapse with pluggable cost/placement policies. GPL/commercial dual licence — the licence is usually the deciding factor |
| **vcglib / MeshLab** | the research-grade quadric implementation, GPL |
| **`sp4cerat/Fast-Quadric-Mesh-Simplification`** | one file, MIT, ~700 lines. Fine for a tool, no attribute awareness |
| **Simplygon** (Microsoft) | the commercial default; ships a UE plugin and implements UE's reduction interfaces, including HLOD generation **[COMMUNITY]** |
| **InstaLOD**, **Pixyz** | commercial competitors, both with UE integrations |
| **`meshopt_simplifySloppy`** | worth knowing it exists: ignores topology entirely, for the far end of the chain where only silhouette matters |

For a project in this codebase's position the answer is meshoptimizer, and the
reason is not quality — it is that it is a **build-time dependency with no
runtime component and no licence tail.**

### 2.5 Can this ship in a commercial game? — the licence table

The question that actually decides the choice. Licences checked against each
project's own `LICENSE` file, August 2026.

| Option | Licence | Ship a paid game? | Obligation |
|---|---|---|---|
| **meshoptimizer** | **MIT** — *"use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies"* | **Yes** | keep the copyright + licence text with the source / in third-party notices |
| **`clusterlod.h`** | **MIT** (same author, same repo) — header says *"This code is distributed under the MIT License"* | **Yes** | as above |
| **`nv_cluster_lod_builder`**, **`vk_lod_clusters`** | **Apache 2.0** — *"perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable"* | **Yes** | include the licence, state modifications; adds an explicit patent grant MIT lacks |
| **`sp4cerat/Fast-Quadric-Mesh-Simplification`** | MIT | Yes | as above — but no attribute awareness, so worse output than meshoptimizer |
| **CGAL** surface simplification | **GPL** / paid commercial | **No**, not without buying the commercial licence | — |
| **vcglib / MeshLab** | **GPL** | **No** | — |
| **Simplygon** (Microsoft) | proprietary EULA; a **free** tier exists, commercial is a **per-game-title** licence with an indie discount below ~$5M revenue/opex **[COMMUNITY]** | Under their terms only | licence agreement, per title |
| **InstaLOD**, **Pixyz** | proprietary, paid | Under their terms only | licence agreement |

**Take meshoptimizer.** MIT is the least restrictive licence in the table, the
library is what a shipped AAA engine calls (§1.2), and — the part that matters
for risk — it is **build-time only**. It runs in an asset-bake tool, its output
is plain mesh data, and nothing from it ends up in the shipped executable. There
is no runtime licence surface, no service dependency, and no per-title
negotiation.

**What no library gives you, at any price**, is the LOD *system*: the selection
metric (§8), the switch and its hysteresis, per-LOD bone masks (§1.3), the
inherit-parent-LOD rule for attached parts (§1.4), and aggregation (§1.5). That
part is engine-specific and would have been yours to write whichever simplifier
you licensed. **You roll your own LOD policy; you never roll your own
simplifier.**

---

## 3. Unreal's discrete stack

Unreal is worth reading because it is the only engine here that ships *all four*
LOD problems as named, separable systems, so it doubles as a checklist.

### 3.1 Static meshes: screen size, not distance

**[EPIC]** UE selects LOD by **screen size** — the fraction of the screen the
mesh's bounds cover — *"it is not calculated by distance, but by screen size"*.
With *Auto Compute LOD Distances*, screen coverage is divided evenly across the
available levels (2 LODs → swap at 50%; 3 LODs → ~66.7% and ~33.3%). Per-LOD
material overrides are supported, which is the same draw-call-reduction lever as
Source 2's `LODGroupReplacements` (§1.2).

Why this matters is §8.

Generation is via a pluggable **mesh reduction interface**; the built-in
reducer has historically been weak enough that Simplygon and InstaLOD both ship
plugins that replace it wholesale **[COMMUNITY]**, and Epic's own docs have
long pointed at Simplygon for auto-reduction **[EPIC]**.

### 3.2 Nanite: the same three ideas, applied per 128 triangles

The mechanism, from the SIGGRAPH 2021 Advances talk and its careful secondary
write-ups **[EPIC]** **[COMMUNITY]**:

1. **Cluster.** Split the mesh into clusters of ≤128 triangles.
2. **Group.** Partition clusters into groups of **8–32** neighbouring clusters.
3. **Simplify with the group boundary locked.** Halve the triangles inside the
   group; the boundary is untouched, so a group at level *L* still meets its
   neighbours at level *L*.
4. **Re-split** the simplified group into **4–16** new clusters.
5. **Repeat, with different group boundaries each level.** This is the trick
   that makes it work: an edge locked at one level is interior at the next, so
   nothing is permanently frozen.

That produces a **DAG**, not a tree — a cluster at level *L+1* has several
parents. Selecting a LOD means choosing a **cut** through the DAG.

The part that makes the cut computable in parallel on the GPU is the **error
metric**, and it is a nice piece of engineering:

- Each group stores the geometric error its simplification introduced.
- Errors are **forced monotonic** up the DAG — a parent's recorded error is at
  least the max of its children's.
- At runtime, error is projected to screen space, and a cluster is drawn iff
  **its own error is under ~1 pixel and its parent's error is over ~1 pixel**.
- Monotonicity means that test is **purely local**. No cluster needs to know
  what any other cluster decided, so every cluster can be tested concurrently
  and the resulting cut is guaranteed crack-free.

**That is the whole idea worth taking away, and it generalises far beyond
geometry.** Store, per level, a *conservative and monotonic* bound on the error
of using that level; then a global consistent decision decomposes into
independent local ones. The same shape appears in HZB occlusion (max-reduce, so
a parent is conservative), and in this codebase's own derived-cache rule in
CLAUDE.md — *the fast path may only skip work that provably does nothing.*

Supporting numbers **[COMMUNITY]** **[EPIC]**:

| | |
|---|---|
| Streaming unit | **128 KB** geometry pages; the first page of each mesh is always resident |
| Traversal | GPU job queue seeded with each instance's root cluster; children enqueued only if the parent survives cull + LOD tests |
| Hierarchy | a fixed-branching tree (8 children) extracted from the DAG for traversal |
| Rasterisation | small triangles go to a compute rasteriser, ~3× faster than fixed function for that case; large ones stay on hardware |
| Occlusion | HZB built with **max**, not average, so it is conservative |
| Nanite's own impostors | **12×12 pixel** captures from **144** directions, for the far end |
| Known weak spot | **many small instances**, where every instance still pays traversal and the triangles are sub-pixel |

Recent scope creep, worth knowing so you do not plan around the 2021 limits
**[EPIC]**: displacement/tessellation (5.5), spline meshes, mesh painting on
Nanite instances (5.5), Nanite Assemblies for instanced parts, and **skinned
Nanite** — arriving through foliage rather than characters, with 5.7 adding
skinned animation on Nanite meshes and Epic quoting **~0.1 ms on the GPU for
100,000 bones** **[EPIC]**, plus automatic animation cutoff below a screen-size
threshold.

**Do not build this.** For a fixed-camera tactics game or a top-down RTS it
solves a problem you do not have, at the cost of an asset pipeline (§2.3) and a
GPU baseline. Take the error-monotonicity idea, leave the machinery. If you do
want it, take `vk_lod_clusters` (§2.2) rather than writing it.

### 3.3 HLOD: four strategies, and they are not interchangeable

World Partition HLOD layer types **[EPIC]**:

| Type | What it produces | When |
|---|---|---|
| **Instancing** | the same meshes, batched into instanced draws | first rung — costs no quality at all |
| **Merged Mesh** | one mesh, materials preserved | when the draw calls are the problem, not the triangles |
| **Simplified Mesh** | one decimated mesh with a baked atlas | when both are |
| **Approximated Mesh** | a coarse proxy — the one aimed at Nanite trees | when nothing about the original matters at that range |

Layers chain, each with its own switch distance.

The ordering is the lesson, and it is the same one as Source 2's aggregates
(§1.5): **the first HLOD rung reduces draw calls without reducing detail at
all.** Instancing before decimation. A project that reaches for decimation first
is usually solving the wrong problem — see §0.

### 3.4 Skeletal meshes: three separate levers

**[EPIC]**

| Lever | What it does |
|---|---|
| **Skeletal Mesh LODs** with *Bones to Remove* and *Bones to Prioritize* | the same per-LOD bone reduction Source 2 encodes as `FLAG_BONE_USED_BY_VERTEX_LODn` (§1.3), exposed as authoring |
| **Update Rate Optimisation (URO)** | skip full pose evaluation on some frames and **interpolate between cached poses**; distant meshes evaluate less often |
| **Animation Budget Allocator (ABA)** | a global CPU budget that distributes URO automatically |

ABA is the one to copy, because it inverts the control:

| Setting | Default | Meaning |
|---|---|---|
| `a.Budget.BudgetMs` | **1.0 ms** | the whole animation budget for the game thread; scalability tiers ship 1.0–2.5 |
| `a.Budget.MaxTickRate` | 10 | worst allowed tick interval, in frames |
| `a.Budget.InterpolationMaxRate` | 6 | max rate while interpolating |
| `a.Budget.MaxInterpolatedComponents` | 16 | beyond this, stop interpolating and just throttle |
| `a.Budget.InterpolationFalloffAggression` | 0.4 | how fast interpolation is withdrawn under pressure |
| `a.Budget.InitialEstimatedWorkUnitTime` | 0.08 ms | seed estimate per component before it has been measured |

**You do not tune per-character LOD distances; you state a millisecond budget
and a significance function, and the system spends the budget.** Closest and
most significant meshes run at full rate, everything else degrades until the
budget is met. That is a fundamentally better shape than authored distances —
it degrades gracefully under a load the content author never anticipated, which
is precisely when authored thresholds fail.

The `InitialEstimatedWorkUnitTime` field is the giveaway that this is a
**measured** feedback loop: costs are learned per component, not assumed.

### 3.5 Significance Manager — the generic version

**[EPIC]** A framework and nothing more: register objects under a tag, supply a
significance function, get a value back, and *"make decisions about what level of
detail objects should be at, tick frequency, whether to spawn effects"*. The
documentation is explicit about why it exists — per-actor distance LOD is not
enough *"in the case of multiplayer games with high numbers of players or
AI-controlled characters that can converge in a single area"* **[EPIC]**.

**That sentence is the whole argument for significance over distance.** Distance
is a per-object property; significance is a *ranking*, and only a ranking can
answer "twenty enemies are all at 15 m, and I can afford six of them at full
rate."

### 3.6 Mass — where all four problems are solved at once

Epic's ECS crowd stack. `EMassLOD` has four states — **High / Medium / Low /
Off** — plus a float *significance* from 0.0 to 3.0 **[EPIC]**. What makes it
worth reading is that a single LOD level drives **both** the representation and
the tick rate, and each level carries a **maximum entity count** as well as a
distance.

Shipped defaults **[COMMUNITY]**, from the visualisation traits:

| | High | Medium | Low | Off |
|---|---|---|---|---|
| Distance (default trait) | 0 | 1,000 | 2,500 | 10,000 |
| Distance (**crowd** trait) | 0 | **500** | **1,000** | **5,000** |
| Visible-in-frustum distance | 0 | 2,000 | 4,000 | 10,000 |
| Max entities (default / crowd) | 50 / **10** | 100 | 500 | — |
| Representation | `HighResSpawnedActor` | `LowResSpawnedActor` | `StaticMeshInstance` | `None` |

Three details worth stealing:

- **Separate thresholds for in-frustum and out-of-frustum.** An entity behind you
  degrades sooner. Obvious, rarely implemented.
- **A max count per level, not just a distance.** This is the significance
  argument again, made concrete: at most 10 full-fidelity crowd actors exist, no
  matter how many are close. It is a hard ceiling on the worst case, which a
  distance threshold can never provide.
- **The bottom visual rung is `StaticMeshInstance`** — an ISM, one draw call for
  thousands. Which is where vertex animation textures come in, because a static
  mesh instance cannot be skinned.

---

## 4. Total War, read from shipped-format assets

### 4.0 What I actually parsed, and the caveat

Rome II is installed on this machine, but only mod packs are present in `data/`.
I scanned `ancient_sea_empires_unit_pack.pack` (160 MB, `PFH4` archive) for the
`RMV2` magic, decoded the header and per-LOD headers, and validated the layout
by checking that the block sizes chain correctly and that
`vertex_bytes / vertex_count` is an exact integer stride for every level
**[TW-ASSET]**.

Confirmed layout (version 6 = Rome II era):

```
0x00  'RMV2'
0x04  u32  version                     (6)
0x08  u32  lod_count
0x0C  char[128] base_skeleton          ("rome_man_game")
0x8C  per LOD: u32 groups, u32 vertex_bytes, u32 index_bytes,
               u32 start_offset, f32 lod_zoom_factor
```

**Caveat:** these are modder-packaged assets in CA's format, not verified CA
originals. What that undermines is any claim about CA's artistic choices; what it
does *not* undermine is the **format** and the **uniformity** — the engine reads
these fields, and a value that is identical across 237 independently authored
models is a pipeline default, not a per-asset decision.

### 4.1 Four levels, and the thresholds are a constant

251 models found. **[TW-ASSET]**

| LOD count | Models | Zoom factors |
|---|---|---|
| **4** | **237** | **100 / 200 / 400 / 500** — *identical in all 237* |
| 3 | 13 | 100 / 200 / 400 |
| 2 | 1 | 100 / 500 |

Triangle counts, over the 237 four-level models **[TW-ASSET]**:

| | LOD0 | LOD1 | LOD2 | LOD3 |
|---|---|---|---|---|
| Mean fraction of LOD0 | 1.000 | **0.676** | **0.326** | **0.122** |
| Median LOD0 triangles | 1,088 (min 72, max 2,145, mean 1,072) | | | |

A worked example — one cuirass mesh **[TW-ASSET]**:

| LOD | vertices | triangles | stride |
|---|---|---|---|
| 0 | 1,455 | 2,145 | 28 B |
| 1 | 1,205 | 1,724 | 28 B |
| 2 | 519 | 642 | 28 B |
| 3 | 252 | 268 | 28 B |

Read that carefully and Total War's geometry LOD scheme turns out to be
**completely ordinary**: four hand-made levels, roughly halving each step,
switching at four hard-coded thresholds shared by every asset in the game. There
is no continuous LOD, no cluster hierarchy, no screen-space error metric.

Also note the **vertex stride is 28 bytes at every level** — for 210 of the
single-group models the stride is constant across the whole chain (28 B or
32 B) **[TW-ASSET]**. **The distant soldier carries the same weights and the same
bone indices as the near one.** Whatever Total War does about bone cost, it does
not do it by shrinking the vertex format.

### 4.2 The actual trick: `imposter_model` is a *shared mesh*, not a billboard

The pack's variant-mesh definitions contain 1,785 uses of an `imposter_model`
attribute **[TW-ASSET]**:

```xml
<VARIANT_MESH model="…/man/armour/jaka_jata_3.rigid_model_v2"
              imposter_model="…/man/armour/carthaginian_linothorax_a.rigid_model_v2" />
<VARIANT_MESH model="…/man/armour/taka_tata_3.rigid_model_v2"
              imposter_model="…/man/armour/carthaginian_linothorax_a.rigid_model_v2" />
<VARIANT_MESH model="…/man/armour/gaka_gata_3.rigid_model_v2"
              imposter_model="…/man/armour/carthaginian_linothorax_a.rigid_model_v2" />
```

**The impostor is another `rigid_model_v2` — a real, skinned, animating mesh —
and many different variants point at the same one.** Four distinct armour
variants collapse onto one generic linothorax.

This is not a geometry LOD. It is an **aggregation LOD** (§0, problem 2). Its
purpose is to make a heterogeneous mass of soldiers *homogeneous*, so that at
distance the renderer is drawing N instances of one mesh with one material
instead of N/4 instances of four meshes with four materials. The triangle saving
is incidental; **the batch is the point**.

That is the answer to "how is Total War good at scale". Not a clever LOD ladder —
a boring four-step ladder plus a **variant-collapse rule that converts artistic
variety back into instancing** at exactly the distance where nobody can tell.
And it is a rule the *content* expresses, per variant, rather than something the
engine infers.

### 4.3 What the rest of Total War's scale comes from

Not from LOD, and this is the honest reading. Creative Assembly's own account
**[COMMUNITY]**, cross-referenced with [`battle_scale.md`](../scale/battle_scale.md):

- **The logic and render threads are decoupled** — *"the logic generating the
  'future' whilst the display renders the 'now'"*. This buys more than any LOD
  scheme, because it removes animation and battle logic from the frame's
  critical path entirely.
- Dynamic environment and unit LODs are described as reducing GPU load but
  *"only partially"* resolving it — with free camera zoom and rotation,
  additional techniques were *"mandatory"*.
- DX12, async compute and explicit multi-GPU came after, on the GPU side.

**In a game where the player can zoom from a strategic overview to a single
soldier's face, distance-based LOD cannot be the main lever, because the player
controls the distance.** That is why the thresholds are uniform and the real work
is elsewhere. It is also why [`battle_scale.md`](../scale/battle_scale.md) says there is no LOD tier
a unit can hide in — that note was about simulation, and the rendering side has
the same shape.

---

## 5. Vertex animation textures — what they actually buy

This is where the question "are VATs used for LOD scale, same as bone count"
needs unpicking, because **there are two different techniques that both get
called "animation in a texture"** and they have opposite trade-offs.

### 5.1 The two techniques

| | **VAT** (vertex animation texture) | **BAT** (bone animation texture) — a.k.a. animation instancing / GPU skinning from a texture |
|---|---|---|
| Texture contains | **final vertex positions** (and normals) per frame | **bone matrices** per frame |
| Texture size | vertices × frames | **bones × frames** |
| Vertex shader work | one or two texture fetches, a lerp | fetch 2–4 bone matrices, weight and blend — real skinning ALU |
| Mesh type | **static mesh** — no skin weights needed | skinned mesh, weights still in the vertex stream |
| Survives a mesh LOD swap? | **No — a new texture per LOD**, since vertex count and ordering change | **Yes — one texture serves every LOD**, because the skeleton is unchanged |
| Blending between clips | position lerp only; visibly wrong on rotation-heavy motion | correct, if you blend matrices/quaternions |
| Attachments, IK, ragdoll, hit boxes | none — there are no bones at runtime | bones exist; you can still read them |

**[COMMUNITY]** puts the LOD consequence exactly: *"VAT requires baking unique
textures for each LOD level as each has different vertex counts, whereas BAT only
needs one set because the bone hierarchy remains consistent across multiple LOD
levels."*

### 5.2 The arithmetic, because it decides the choice

**[inferred]** For one character, one clip, 60 frames:

| | VAT | BAT |
|---|---|---|
| Entities stored | 5,000 vertices | 100 bones |
| Per entity per frame | position as RGBA16F = 8 B (+ another 8 B if you store normals) | 3 × RGBA32F rows = 48 B |
| **Per clip** | 5,000 × 60 × 8 = **2.4 MB** (4.8 MB with normals) | 100 × 60 × 48 = **288 KB** |
| Across a 4-level LOD chain | ×4 sets of textures, one per level: **~9.6 MB** | **unchanged: 288 KB** |
| Across 20 clips | ~192 MB | ~5.8 MB |

**VAT memory scales with the product of vertex count, frame count and LOD
count. BAT memory scales with bone count and frame count only.** For a crowd
with a library of animations, that is a 30× difference and it decides the
question on its own.

So the honest summary: **VAT is not "the LOD technique for crowds". It is the
technique for the *bottom* rung**, where you need a static mesh because you are
rendering through an instanced-static-mesh path that cannot skin at all. Above
that rung, a bone texture is strictly better.

### 5.3 What it is worth, in numbers

The founding measurement is GPU Gems 3 chapter 2, *Animated Crowd Rendering*
**[NVIDIA]** — a BAT, not a VAT: *"encode all frames of all animations into a
texture and look up the bone matrices from that texture in the vertex shader."*

| | |
|---|---|
| Characters | **9,547**, independently animating |
| Frame rate | ~34 fps, 8800 GTX, 1280×1024 (2007 hardware) |
| Draw calls | **160 instanced** vs **59,726** traditional — a **373×** reduction |
| Instancing | per-instance data in a constant buffer indexed by `SV_InstanceID`; 5 × float4 per instance ⇒ 819 instances per call |
| LOD | three levels, at **20 / 60 / 100 character radii** |

The "5 float4s per instance" line is the design constraint that matters and it
has not changed: **whatever you want to vary per instance — position, tint,
current animation, current frame, phase offset — has to fit in a small
per-instance record**, because that record is what makes one draw call possible.
Everything else must be shared. This is the same constraint that makes Total
War's `imposter_model` collapse (§4.2) necessary.

Note also the LOD distances are quoted in **character radii**, not metres —
which is §8's point, arrived at in 2007.

### 5.4 In practice, in Unreal

**[EPIC]** **[COMMUNITY]** The **AnimToTexture** plugin, released with the UE5
City Sample and folded into 5.1, bakes skeletal animation into textures so
animations can be played on **instanced static meshes**. The shipping crowd
pattern is a three-tier one:

| Range | Representation | Cost |
|---|---|---|
| near | fully rigged skeletal actors (MetaHumans in City Sample) | full animation graph, per-actor |
| mid | vertex-animated static meshes generated from the same characters | vertex-shader only |
| far | ISM, one draw call for thousands | ~nothing |

And this maps exactly onto §3.6's `HighResSpawnedActor` /
`LowResSpawnedActor` / `StaticMeshInstance` ladder — Mass drives the swap, the
VAT makes the bottom rung possible.

### 5.5 The failure mode nobody mentions until it bites

**The swap is the hard part, not the representations.** A skeletal actor and a
VAT instance must agree, at the frame of the swap, on:

- **animation phase** — otherwise the character's legs teleport mid-stride;
- **which clip** — the VAT tier can only play clips that were baked;
- **transform and root motion** — the actor's root motion has been accumulating,
  the VAT's has not;
- **everything attached** — weapons, hats and shields have no bones to hang off
  once the skeleton is gone. Either they are baked into the mesh (which is
  Total War's `imposter_model` answer, §4.2) or they disappear.

That list, not the shader, is why crowd systems ship as *frameworks* rather than
as a material function. It is also why the near tier should be as small as you
can bear (Mass's crowd trait caps High at **10 entities**, §3.6): every swap is a
risk, and fewer entities in the expensive tier means fewer swaps in view.

---

## 6. Deformation LOD without textures

Before reaching for VATs there are three cheaper levers, in increasing order of
disruption.

1. **Bone-count LOD.** Skin the distant mesh to fewer bones, and skip evaluating
   the bones no visible LOD reads. This is Source 2's per-bone LOD bitfield
   (§1.3) and Unreal's *Bones to Remove* (§3.4). AC Unity's shipped numbers —
   **~300 bones near, 11 far** — are in [`crowd_scale.md`](../scale/crowd_scale.md); an
   order-of-magnitude cut, with no change to the animation system.
2. **Update-rate LOD.** Evaluate the pose every N frames and interpolate between
   cached poses — URO (§3.4). This attacks the *CPU* cost (graph evaluation,
   blending, bone hierarchy walk), which is usually the real cost, whereas bone
   count attacks skinning ALU, which usually is not.
3. **Budgeted update rate.** ABA (§3.4): state a millisecond budget, rank by
   significance, let the system pick the rates. Measured per-component costs,
   not assumed.

The ordering is deliberate: (1) is a content/format change you want to have made
early, (2) is a runtime change you can add any time, (3) is a policy layer over
(2). **Only when (1)–(3) are exhausted and you still need a single draw call does
VAT earn its memory.**

---

## 7. AI and simulation LOD

The obvious formulation — *AI gets cleverer the closer it is to the camera* — is
the one everybody starts with, it is what every shipped system approximates, and
**there is a well-argued case that it is measurably the wrong axis.** This
section is the long version, because it is the LOD problem this project is most
likely to actually need.

### 7.1 What there is to degrade

The levers, roughly in order of how much they save per unit of risk:

| Lever | What is degraded | Note |
|---|---|---|
| **Tick rate** | think every N frames | **stagger the phases** so N agents at rate 1/N cost one agent per frame, not a spike every N. The single highest-value line of code in any AI LOD system |
| **Interpolation between thinks** | the gap is filled rather than frozen | the animation-side equivalent is URO (§3.4); the AI-side version is "keep moving along the last plan" |
| **Perception rate and fidelity** | fewer raycasts, coarser vision tests, shared/pooled queries | usually **the** cost. `spatial_queries.md` §3.6 has CryEngine stating raycasts are the dominant cost of the whole AI system, and their rule — sort candidates, then run the expensive test from the best down and stop at the first pass |
| **Path quality and replan rate** | full A\* → cached path → flow field → straight line | the far tier can often share one flow field for a whole group (`navigation.md`) |
| **Steering and avoidance quality** | full RVO → simple separation → none | Sunshine-Hill's own example of a per-character LOD feature |
| **Behaviour depth** | full behaviour tree → small FSM → scripted idle | Hitman's crowd is a **three-state FSM** (§7.2) |
| **Substitution** | a real agent replaced by a crowd body | AC Unity's 40 real AI behind 10,000 bodies (`crowd_scale.md`) |
| **Existence** | the agent is not simulated at all | L4D's Active Area Set (`crowd_scale.md`); the classic "simulation bubble" |

Two of these — substitution and existence — are qualitatively different from the
rest, because they are the only ones that can change what the agent *does* rather
than *when* it does it. §7.5 is about that line.

### 7.2 What shipped games actually do

**Neverwinter Nights (BioWare, 2002)** is the origin of the term in AI. Mark
Brockington's *Level-of-Detail AI for a Large Role-Playing Game* **[COMMUNITY]**
describes subdividing game objects into categories and reducing the
time-intensive actions — **pathfinding and combat** specifically — for objects
outside the player's view, to achieve *"the perception of thousands of actors
thinking simultaneously"*. Note what is already present in 2002: it is
*categories*, not a smooth function, and the two things worth throttling are the
two expensive ones, not the whole agent.

**Hitman (IO Interactive)** is the best-documented modern version, and the
numbers are the useful part **[COMMUNITY]**:

| | |
|---|---|
| Crowd agents | 1,000+ simultaneously (Absolution: 1,200 per zone, 500 max on screen; HITMAN 2: 1,700 default per level, 700 on screen, **2,000+** crowd capacity) |
| Full behaviour-tree AI | **300+** per level, each with its own knowledge base and goal-driven decisions |
| Crowd AI | a **three-state FSM** — standing idle, walking, "pending walk" — driven by behaviour zones and panic flows, *not* behaviour trees |
| Update rate | *"the ones farthest away from the player update their behaviour less frequently as the CPU resource used for updating AI prioritises those closest to you"* |
| Animation | for distant characters *"either dialled down in quality or turned off entirely"* |
| Promotion | the game can **"possess" a crowd member and upgrade it** to a full coordinated character when it needs to |
| Perception | a **2D reasoning grid over the navmesh**, so sight tests and pathfinding do not hit the navmesh directly |

Two things there are worth more than the rest. **Promotion on demand** — the
cheap tier is not a dead end, any crowd body can be pulled up into a real agent
— and the **reasoning grid**, which is the same move this codebase already makes
with `OcclusionGrid` and `ReachField`: put a cheap summary in front of the
expensive structure so the per-agent query never touches the expensive one.

**Unreal** ships the generic frameworks (§3.5, §3.6): the Significance Manager,
and Mass's four tiers with per-tier maximum counts. Its documentation contains
the concise statement of why distance alone fails — per-actor distance methods
are not sufficient *"in the case of multiplayer games with high numbers of
players or AI-controlled characters that can converge in a single area"*
**[EPIC]**.

**Assassin's Creed Unity** and **Left 4 Dead** are in
[`crowd_scale.md`](../scale/crowd_scale.md) and not repeated here; they are the
substitution and existence rows of §7.1 respectively.

### 7.3 The LOD Trader — the argument that distance is the wrong axis

Ben Sunshine-Hill, *Phenomenal AI Level-of-Detail Control with the LOD Trader*,
Game AI Pro ch. 14 **[PAPER]**. Read in full for this note. It is the deepest
treatment of AI LOD in print and it is worth going through properly, because its
model is reusable even if you never build the algorithm.

#### 7.3.1 Why graphics LOD works and AI LOD does not

His framing, and it is exactly §0's point arrived at from the other side:

> *"In graphics, LOD acts as a natural limit on scene complexity. The player can
> only be next to so many objects at once, and everything that's not near the
> player is cheaper to render, so framerate tends to even out… For AI, however,
> the techniques we'd really like to use often aren't feasible to run on more
> than a small handful of NPCs at once, and a cluster of them can easily blow our
> CPU time budget. **There's no 'LOD threshold distance' we could pick which
> would respect our budget and give most visible characters the detail we
> want.**"*

The measured consequence, from their implementation:

> *"In order to guarantee a reasonable framerate, it was necessary to set the
> threshold distances so close that things like low-quality locomotion could be
> clearly seen, particularly in **sparsely populated areas** and where there were
> **long, unobstructed lines of sight**."*

**That is the whole indictment of distance-based AI LOD in one sentence.** A
distance threshold must be tuned for the worst case — a crowd converging on the
player — so it is *wastefully aggressive* in the common case, an empty room. The
system spends its quality budget when it is not needed and runs out when it is.

#### 7.3.2 The unit of measurement: "will the player notice?"

Rather than trying to measure realism, he measures **the probability that the
player notices** a reduction — the event is called a **BIR** (the chapter defines
it as *the player actually noticing an issue*; *"a BIR only occurs when the
player notices the issue; just reducing the detail of an entity isn't a BIR if
she doesn't notice it as unrealistic"*).

Then a modelling trick worth stealing on its own: work not in probability *p*
but in **P-space**, `P = −ln(1 − p)`. Probabilities of independent events
multiply; their P-values **add**. That turns "minimise the chance the player
notices anything anywhere" from a product over hundreds of agents into a **sum**,
which is what makes the whole thing a linear optimisation rather than a mess.

#### 7.3.3 Three kinds of noticing, and they have different triggers

This is the part that generalises furthest, and it is why "closer to the camera"
is insufficient — **two of the three do not require the player to be looking at
all**:

| Category | What it is | Examples from the chapter |
|---|---|---|
| **US** — unrealistic state | the immediately observable simulation is wrong. Needs only *momentary* attention, and *"the attention need not be voluntary — the eye tends to be drawn to such things"* | eating from an empty plate; running in place against a wall; wearing a bucket on his head |
| **FD** — fundamental discontinuity | current state is incompatible with the player's **memory** of past state. Can be caused entirely off-screen | a character disappearing while briefly around a corner; frozen in place for hours while you were away; regaining the use of a limb that had been broken |
| **ULTB** — unrealistic long-term behaviour | only revealed by **extended observation** | wandering randomly instead of having goal-driven behaviour; *"a car that never runs out of gas"*. *"At any given time, only a small handful of characters are likely to be prone to ULTB BIRs"* |

Each category gets its own **criticality model**, a product of factors:

| Category | Factors |
|---|---|
| US | **observability × attention** |
| FD | **memory × return time** |
| ULTB | **attention × memory × duration** |

- **Observability** — *"comes closest to graphical LOD"*: screen area the
  character occupies, divided by a saturation size, clamped to 1. Their
  saturation constant was **the screen area of a fully visible character at 4
  metres**; they note a smaller value suits high-definition displays. Off-screen
  is 0.
- **Attention** — *"the most difficult factor to estimate."* Attempted attention
  is an **exponential moving average of observability** (α = 2, giving *"a 95%
  falloff in 1.5 seconds"*), mixed with a **focusing** term — the EMA of
  `dot(camera forward, direction to character) × observability`, converging much
  more slowly, because a player keeping something centred in view is a strong
  attention signal. Their weights: **0.7 observability, 0.3 focusing**. Then
  **interference**: attention is not unlimited, so it is divided among competing
  characters. Game-specific terms can be mixed in — how much of a threat the
  character is, how rapid his motions are.
- **Memory / return time** — how well the player has memorised facts about this
  character, and how attenuated that memory will be when (or whether) they come
  back.
- **Duration** — how much time and attention the player has already spent on this
  character.

**Every one of these is a better answer to "how important is this agent" than
distance, and three of them have nothing to do with where the camera is now.**

#### 7.3.4 The optimisation

Each character carries a **criticality vector** (three scores). Each detail level
carries an **audacity vector** (three scores — how *audaciously* unrealistic that
level is per category). The probability of a BIR is the **dot product** of the
two. *"Linear algebra — it's not just for geometry anymore!"*

The Trader's *"portfolio"* is the assignment of detail levels to characters. Each
run it looks for **trades**:

> *"First it considers upgrades. It repeatedly picks the most valuable available
> upgrade to add to its set of trades until it has overspent its resource budget.
> Then, it repeatedly picks the most valuable available downgrade to add to its
> set of trades until it has not overspent its resource budget."*

Greedy, with priority queues over available upgrades and downgrades. Not optimal
— *"the optimality of its LOD solutions is approximate"* — but a knapsack solved
approximately every frame beats a fixed threshold.

Four extensions that are each worth more than the base algorithm:

- **Multiple features under one budget.** Pathfinding quality (mostly ULTB) and
  hand IK (mostly US) are separate LOD features, but sharing one budget lets the
  system *"automatically shift resources between pathfinding and IK"*, and trade
  a pathfinding downgrade on one character for an IK upgrade on another. Separate
  per-system budgets cannot do this.
- **Transitions carry cost and audacity, not just levels.** A transition can be
  charged for the work it takes *and* for the risk it carries — *"if the
  transition is liable to produce a visible 'pop' or if it involves a loss of
  character information which could later lead to an FD or ULTB BIR."*
  One-way transitions are expressible by discarding the reverse. **This is the
  §5.5 swap problem, priced.**
- **Existence as a LOD feature.** A `yes`/`no` feature where `no` costs zero and
  has zero audacity, *but the transition from `yes` to `no` carries US and FD
  audacity*. Deleting a distant character becomes a **priced decision inside the
  same optimisation** rather than a separate simulation-bubble hack.
- **"Save space" as a second resource**, constrained only when the game is about
  to save — so the state that gets kept is the state the player is most likely to
  remember. And in multiplayer, criticality is simply **summed over all observing
  players**.

#### 7.3.5 What it cost, and what it bought

| | |
|---|---|
| Deployment | a free-roaming game, hundreds of characters, **8 LOD features**, hundreds of feature solutions |
| Trader cost | **57 µs per frame average — 0.17% of target frame time** |
| Memory | **500 kB** of transition data + **48 bytes per entity** (author notes both could be halved) |
| Result | a **controlled, blinded study**: viewers watching distance-based LOD were consistently more likely to notice problems, and noticed them more often, than viewers watching the LOD Trader |
| Hardest part | *"tuning the criticality metrics and audacity vectors, due to their subjectivity"* |

And his own honest summary, which is the right expectation to carry into any
budgeted LOD system:

> *"The goal of the LOD Trader is not to make wildly audacious detail reductions
> and get away with them. Rather, the goal is to be clever enough to do detail
> reduction in the right places in those moments when detail reduction has to
> happen somewhere."*

### 7.4 The thing that actually breaks: what survives the tier change

This is §5.5's problem again, one layer up, and it is where AI LOD systems fail
in practice. When an agent moves between tiers, these must be preserved or
plausibly reconstructed:

| State | What goes wrong if it is dropped |
|---|---|
| Position and facing | the classic teleport-on-promotion |
| Path / goal | the agent forgets where it was going and restarts, visibly |
| Alert level, target, last-known-position | the enemy that was hunting you becomes an idler — an FD in Sunshine-Hill's terms, and in a tactics game a fairness bug |
| Inventory, ammo, cooldowns, wounds | *"regaining the use of a limb that had been broken"* |
| Elapsed time | the agent that was frozen while you were away — the single most common off-screen artefact |

**The design consequence: make the cheap tier a *reduced update* of the same
state, not a different representation of a different thing.** If the low tier
stores the same fields and merely updates them rarely, promotion is free and
demotion is lossless. If it stores a different structure, every promotion is a
reconstruction and every reconstruction is a bug. Hitman's ability to *possess* a
crowd member and upgrade it in place (§7.2) is exactly this property, and it is
worth designing for even at small agent counts.

### 7.5 The genre rule — presentation versus outcome

The rule that decides how far any of this may go:

> An AI LOD is safe if the degraded agent's *observable contribution to game
> state* is unchanged, or if the game does not care that it changed.

Left 4 Dead does not care — a zombie two blocks away has no state worth
preserving, so it can cease to exist and be respawned somewhere more useful; the
Director *wants* that. A free-roaming crowd game does not care much either, which
is why the LOD Trader can price existence itself as a feature.

**A tactics game cares absolutely.** The enemy behind the wall has a position, a
facing, an alert level, an ammo count and a plan, all of which the player will be
tested on and can verify. There is no camera-distance argument that makes it
acceptable for that agent to think worse — the player will be shot by it.

So for this codebase the line is:

- **Legal:** changing *when* an agent thinks (rate, phase, budget), *how often it
  re-plans*, how much *animation and effects* it gets, and how expensive its
  *perception queries* are **when they cannot change the result** — e.g. skipping
  a visibility test against a player the agent already cannot possibly see.
- **Illegal:** changing *what it concludes*. Different decisions, cheaper
  targeting, a coarser cover choice, "close enough" pathing that walks into fire.

That makes the invariant testable, which is the real prize: **the same decisions,
later.** A deterministic replay at full rate and at throttled rate should reach
the same game state, and that is a test you can actually write — the same
discipline `spatial_queries.md` applies to derived caches, applied to time.

### 7.6 Hazards, collected

- **Thrashing at the boundary.** An agent oscillating between tiers pays both
  costs and shows the transition artefact repeatedly. Needs a dead band, or a
  minimum dwell time, or the Trader's explicit transition cost.
- **Synchronised thinking.** Bucketing by rate without **staggering the phase**
  converts a smooth load into a periodic spike. This is the most common
  implementation bug in the whole area.
- **The world that only lives where you look.** Traffic that dissolves in the
  mirror, a patrol that never actually completed its route. These are FD BIRs,
  and no amount of distance tuning fixes them — they need the *memory* term.
- **Budget starvation under convergence.** The case Epic's docs name: everything
  interesting piles into one room. A per-tier **maximum count** (§3.6) or a
  budget (§3.4) is the only real defence; a distance threshold has none.
- **Determinism and replay.** If tier assignment depends on the camera, and
  behaviour depends on tier, then behaviour depends on the camera — which breaks
  replays, and in multiplayer means two clients disagree. Either the tier must
  not affect outcome (§7.5), or tier assignment must be part of the simulation
  and identical on every machine.
- **Fairness in competitive play.** An enemy that thinks at 2 Hz because it is
  off-screen is an exploitable enemy.

### 7.7 What this project should build

Small, and in this order:

1. **Rate buckets with staggered phase**, driven by a significance score, with a
   per-frame agent budget rather than a distance threshold.
2. **Significance = observability × attention, plus a memory term**, not
   distance. Even the crude version — screen coverage, an EMA of it, and "is this
   agent currently in combat with the player" — is closer to §7.3.3 than to a
   distance check, and costs a few lines.
3. **One shared cheap-state representation** so promotion is an update-rate
   change and nothing else (§7.4).
4. **The outcome invariant asserted in a test** (§7.5).

The LOD Trader itself is not the recommendation — at tens of agents its 57 µs
would be a larger fraction of the work than the work it saves. **Its *model* is
the recommendation**: measure significance as probability-of-being-noticed,
decompose it into observability, attention and memory, and spend a budget rather
than compare a distance.

---

## 8. The selection metric — the actual interesting question

Every system above compares *something* against a threshold. What that something
is has been quietly improving for twenty years, and the direction is consistent.

| Metric | Who | What it costs you |
|---|---|---|
| **Distance** | Source 2 (`switch_threshold`, metres), Total War (`lod_zoom_factor`), Mass | a big object and a small object at the same distance get the same treatment, so **every asset needs hand-tuned thresholds**, and they are wrong at a different FOV or resolution |
| **Object radii** | GPU Gems 3 crowd (§5.3) | scale-invariant, still resolution-blind — a cheap 80% fix |
| **Screen size** (projected bounds as a fraction of the screen) | Unreal static meshes (§3.1) | removes object scale and FOV; still says nothing about *how wrong* the cheaper mesh is |
| **Screen-space error in pixels** | Nanite (§3.2) | the honest metric — "using this level costs at most 1 pixel of error". Requires the generator to *measure and store* error, in absolute units, monotonically |

**The progression is from tuning a number per asset to computing it from a
quantity the generator measured.** Each step deletes a category of authoring
work and a category of bug. The reason Total War's 237 models all carry
`100/200/400/500` (§4.1) is that a raw distance threshold is not really tunable
per asset — nobody can meaningfully choose it, so everybody uses the default.

Two supporting details:

- **Hysteresis / dithering.** Switching on a single threshold pops, and worse,
  an object hovering at the threshold flickers between levels. The two answers
  are a dead band (switch up at a different value than you switch down) and
  dithered cross-fade (draw both, screen-door blend over a short range). Source 2
  has explicit `Start Fade Dist` / `End Fade Dist` on `prop_static` (§1.5).
- **One global scalar for scalability.** Source 2's
  `sc_lod_distance_scale_override` (§1.4). Quality presets should scale the
  metric, never re-author thresholds.

---

## 9. Summary table — who solves what

| | Geometry | Aggregation | Deformation | AI | Metric |
|---|---|---|---|---|---|
| **Source 2** | discrete `LODGroup` mesh lists, generated by meshoptimizer, up to 7 levels | `CAggregateSceneObject`; LODs *disqualify* a prop from merging | per-bone LOD bitfield in the skeleton resource | — | distance |
| **Unreal (classic)** | discrete, pluggable reducer (Simplygon/InstaLOD) | HLOD: instance → merge → simplify → approximate | Bones to Remove; URO; ABA at 1 ms | Significance Manager | screen size |
| **Unreal (Nanite)** | continuous cluster DAG, boundary-locked, monotonic error | per-cluster; weak on many small instances | arriving via skinned foliage, 5.5–5.7 | — | 1 px screen error |
| **Unreal (Mass)** | via representation swap | ISM at the bottom rung | VAT via AnimToTexture | 4 tiers + max counts + significance | distance, in/out of frustum |
| **Total War** | 4 fixed levels, ~0.68 / 0.33 / 0.12 of LOD0 | **`imposter_model`: variants collapse to a shared mesh** | none in the vertex format | — | zoom factor, uniform across all assets |
| **Hitman** | — | crowd rendered as one system | animation *"dialled down or turned off"* far | 3-state FSM crowd under 300+ BT agents, **promotion by possession** | distance-prioritised CPU share |
| **LOD Trader** | — | — | any feature, priced | 8 features under one budget, existence included | **probability the player notices** |

---

## 10. What this project should do

Reading the above against `cromwell`'s three target genres (RTS, FPS,
third-person) and this game's actual shape.

### 10.1 The ranking

1. **Discrete LOD chains, generated at bake time with meshoptimizer.** MIT, no
   runtime component, no licence tail, and it is what Source 2 itself calls
   (§1.2). Copy the flag set from §1.2's table — it is a shipped engine's list of
   which knobs turn out to matter, obtained for free.
2. **Select on screen size, not distance** (§8). It is the same amount of code —
   projected bounds radius over distance — and it deletes per-asset tuning
   forever. Doing it later means re-authoring every threshold.
3. **Aggregation before decimation** (§1.5, §3.3). For an RTS the count problem
   arrives before the triangle problem, and the first rung — instance identical
   things together — costs no quality at all.
4. **Reserve the per-bone LOD mask in the model format now** (§1.3), even if
   nothing reads it for a year. Retrofitting a per-bone bitfield into a shipped
   skeleton format is the expensive kind of change; a `u8` per bone is the cheap
   kind. This is `nav_architecture.md` §10's "free now, expensive later" rule
   applied to the model pipeline.
5. **Animation LOD as update rate, budgeted** (§3.4, §6). ABA's shape — a
   millisecond budget plus a significance ranking, with measured per-component
   costs — before any authored per-character distances.
6. **AI LOD as scheduling only** (§7.5). Same decisions, later; never different
   decisions, and assert it in a test. Rank agents by **observability × attention
   plus memory** rather than by distance (§7.3.3) — the crude version is a few
   lines and is still the right axis — and spend a per-frame budget rather than
   compare a threshold. Build order in §7.7.

### 10.2 What not to build

- **Not Nanite, and not a cluster DAG.** §2.3's numbers are the argument: the
  representation is a cache several times larger than the source art, built by a
  multi-minute multi-threaded job. That is a pipeline, not a feature. If the day
  comes, take `vk_lod_clusters` or `clusterlod.h` (§2.2) rather than writing one.
- **Not VATs, yet.** §5.2's arithmetic: they cost ~30× the memory of a bone
  texture across a clip library and a LOD chain, and they buy exactly one thing
  — the ability to render through a path that cannot skin. Until there is a
  measured draw-call wall with thousands of bodies behind it, bone-count LOD and
  update-rate LOD (§6) are strictly better trades.
- **Not per-asset LOD distances.** See §8 and Total War's 237 identical
  thresholds (§4.1). If the metric needs per-asset tuning, the metric is wrong.

### 10.3 The idea worth stealing regardless of scale

Nanite's **monotonic conservative error** (§3.2). Store, per level, a bound on
the error of using that level, forced to be conservative up the hierarchy; then
a globally consistent choice decomposes into independent local ones, and the
selection is parallel and crack-free by construction.

That is the same rule this codebase already applies to derived caches — *the
fast path may only skip work that provably does nothing* (CLAUDE.md) — and to
Killzone 2's one-sided visibility approximation (`spatial_queries.md`). Three
unrelated systems, one rule: **make the cheap answer provably conservative, and
correctness stops being a coordination problem.**

### 10.4 When any of this is added

Per CLAUDE.md: **an LOD selection pass that runs per frame gets a profiler zone
in the same commit.** One zone named for the system (`lod select`), nested under
`render`. Sub-zones only if a measurement points at it — LOD selection over a few
hundred objects is a candidate for "under 1% of the frame, fold it into its
parent", and the thing that would make it expensive (per-object bounds
projection in a scalar loop) is exactly the kind of shape decision §10.1 point 2
should get right the first time.

---

## Sources

**Read on this machine (primary):**

- s&box install, `E:/SteamLibrary/steamapps/common/sbox/` — `citizen.vmdl`,
  `citizen_lodgrouplist.vmdl_prefab`, `bin/win64/tools/modeldoc_editor.dll`,
  `bin/win64/engine2.dll`, `bin/win64/meshsystem.dll`,
  `bin/win64/bakedlodbuilder.dll`
- Total War: Rome II, `data/ancient_sea_empires_unit_pack.pack` — 251 RMV2
  models parsed for LOD headers, zoom factors, vertex/index counts, and
  `imposter_model` variant definitions

**Published:**

- Karis, Stubbe, Wihlidal — *A Deep Dive into Nanite Virtualized Geometry*,
  SIGGRAPH 2021 Advances:
  https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- Nanite explained — https://cs418.cs.illinois.edu/website/text/nanite.html and
  https://www.thecandidstartup.org/2023/04/03/nanite-graphics-pipeline.html
- meshoptimizer v1.0 notes — https://meshoptimizer.org/v1.html ; cluster LOD demo
  https://meshoptimizer.org/demo/clusterlod.h
- Kapoulkine — *Billions of triangles in minutes*,
  https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/
- NVIDIA `vk_lod_clusters` — https://github.com/nvpro-samples/vk_lod_clusters ;
  `nv_cluster_lod_builder` — https://github.com/nvpro-samples/nv_cluster_lod_builder
- Epic — *Animation Budget Allocator*,
  https://dev.epicgames.com/documentation/unreal-engine/animation-budget-allocator-in-unreal-engine
- Epic — *Significance Manager*,
  https://dev.epicgames.com/documentation/unreal-engine/significance-manager-in-unreal-engine
- Epic — *Creating and Using LODs*,
  https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-and-using-lods-in-unreal-engine
- Epic — *World Partition HLOD*,
  https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine
- Epic — *Overview of Mass Gameplay*,
  https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine ;
  Mass visualisation defaults, https://vrealmatic.com/unreal-engine/mass/visualization
- Epic — *AnimToTexture for crowds*,
  https://dev.epicgames.com/community/learning/tutorials/3xKm/unreal-engine-animtotexture-plugin-how-to-use-it-to-make-vertex-animation-textures-for-crowds
- Epic — *Nanite Foliage* (5.7),
  https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage
- Sunshine-Hill — *Phenomenal AI Level-of-Detail Control with the LOD Trader*,
  Game AI Pro ch. 14 (read in full; text extracted from the PDF),
  http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter14_Phenomenal_AI_Level-of-Detail_Control_with_the_LOD_Trader.pdf
  — and the underlying study, [Sunshine-Hill 11]
- Brockington — *Level-of-Detail AI for a Large Role-Playing Game*, AI Game
  Programming Wisdom (2002), pp. 419–425 (Neverwinter Nights)
- *The AI of Hitman (2016)*,
  https://www.gamedeveloper.com/design/the-ai-of-hitman-2016-
- NVIDIA GPU Gems 3 ch. 2 — *Animated Crowd Rendering*,
  https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-2-animated-crowd-rendering
- Rabel (Creative Assembly) — *Anatomy of the Total War Engine*, parts I–V,
  https://gpuopen.com/learn/anatomy-total-war-engine-part/
- *Designing Total War: Warhammer II to handle tons of units and massive
  battles*,
  https://www.gamedeveloper.com/design/designing-i-total-war-warhammer-ii-i-to-handle-tons-of-units-and-massive-battles
- Valve Developer Community — *VMDL LodGroup*, *prop_static (Source 2)*;
  s&box ModelDoc docs, https://sbox.game/dev/doc/editor/model-editor
- VAT vs BAT — https://github.com/mbmtrex/Bone-Animation-Texture-BAT ;
  https://stoyan3d.wordpress.com/2021/07/23/vertex-animation-texture-vat/
