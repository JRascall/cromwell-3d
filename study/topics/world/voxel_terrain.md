# Voxel terrain — the destructible-volume pipeline, end to end

How a game turns **an editable scalar field** into a triangle mesh, a collision
shape, a lit surface and a save file — at planet scale, at sixty hertz, while the
player is digging a hole in it.

**Space Engineers is the worked example**, because SE1's C# source is public and
the whole pipeline can be read rather than guessed at. Around it sits the
algorithmic canon — marching cubes, surface nets, dual contouring, Transvoxel —
and the two GPU Gems chapters that are the usual entry point into this subject,
read for **what they actually solve and what they do not**.

> **Read alongside:** [`space_engineers.md`](../../games/space/space_engineers.md) — the wider SE
> note. Its §4.1–4.2 has the constants table and the
> **provider-plus-diff storage** idea, which is the single best thing in either
> game and is not repeated here at length. This note is the layer *below* it: how
> the field becomes geometry.
> [`elite_dangerous.md`](../../games/space/elite_dangerous.md) §3 for the contrasting planet
> pipeline — ED spherifies a quadtree of *patches* and never stores anything, so
> its terrain is not editable and it pays none of this note's costs.
> [`rainbow_six_siege.md`](../../games/shooters/rainbow_six_siege.md) for the other destruction model:
> planar 2D cutting with faked debris, which is the right answer for buildings
> and the wrong one for terrain.
> [`re_engine_rendering.md`](../../games/rendering/re_engine_rendering.md) for signed distance fields
> used for lighting rather than geometry.

---

## Sourcing

| tag | source | strength |
|---|---|---|
| **[SRC]** | **SE1's released C# source**, read file by file — meshers, clipmap, storage, physics body, shaders | **Primary.** Everything tagged this way is quoted or paraphrased from real code. |
| **[PAPER]** | Lorensen & Cline, Ju et al., Lengyel, Losasso & Hoppe, Geiss | Published algorithms. Solid. |
| **[KEEN]** | Keen dev blogs and diaries | Strong for *what*, near-silent on cost. |
| **[COMMUNITY]** | Modding wikis, guides | Useful for numbers Keen never published. Verify. |
| **[inferred]** | Our reading and our arithmetic | Not anybody's word. |

**The one gap that matters.** SE1's dual contouring mesher is a thin C# wrapper
around `VRage.Native` — and **`VRage.Native` is not in the published source
tree**. `Sources/` contains twenty projects and that is not one of them. So the
DC *inner loop* — how the vertex inside each cell is actually placed — is the one
part of this pipeline that cannot be read, only inferred from its interface and
its asserts. Everything else in §1–§8 is source.

---

## 0. Three corrections before anything else

**Minecraft does not use marching cubes.** It has no isosurface at all. A
Minecraft voxel is a *boolean block* and its mesh is literally the block's faces,
with hidden faces culled; the classic "naive mesher" emits one quad per exposed
face and is what the game still ships. **[COMMUNITY]** Greedy meshing — merging
runs of identical faces into larger quads — is the well-known alternative, and
the reason it is not universally adopted is instructive: **a greedy quad spans
many cells, so editing one cell invalidates a large merged region, whereas a
naive mesh only invalidates its own neighbourhood.** That is a pure
edit-cost-versus-draw-cost trade, and a game where the player edits constantly
lands on the naive side of it.

So "marching cubes like Minecraft" conflates two entirely different families.
The distinction is the whole subject:

| | Cubic voxels (Minecraft) | Density field (Space Engineers) |
|---|---|---|
| Cell payload | material id, or a bit | **a scalar**, 0–255 |
| Surface | the cell's own faces | the **isosurface** where the scalar crosses a threshold |
| Mesher | face culling / greedy quads | marching cubes, surface nets, dual contouring |
| Looks like | blocks | smooth, mineable terrain |
| Edit granularity | a whole cell | a *fraction* of a cell — sub-voxel |

**Second: SE1 ships two meshers and the one it renders with is dual contouring,
not marching cubes.** **[SRC]** `Sources/Sandbox.Game/Engine/Voxels/` contains
both `MyMarchingCubesMesher.cs` and `MyDualContouringMesher.cs` behind one
interface, `IMyIsoMesher`. §3 is about which does what and why the answer is
"both, for different reasons".

**Third: the two GPU Gems chapters solve different problems, and only one of them
is about destructible terrain at all.** GPU Gems 3 ch. 1 is a density-field
marching cubes generator — directly on topic. GPU Gems 2 ch. 2 is **geometry
clipmaps over a 2D heightfield**, which cannot express a cave and is not editable
in the way this project would need. Its transferable content is the *LOD
structure*, and SE uses exactly that structure over 3D data. §5 reads both
properly.

---

## 1. The representation

**[SRC]** SE1's numbers, from `MyVoxelConstants.cs` (also in
[`space_engineers.md`](../../games/space/space_engineers.md) §4.1, repeated because everything
below depends on them):

| Constant | Value |
|---|---|
| `VOXEL_SIZE_IN_METRES` | **1.0** |
| `VOXEL_CONTENT_EMPTY` / `_FULL` | **0 / 255** — one byte |
| `VOXEL_ISO_LEVEL` | **127** |
| `DATA_CELL_SIZE_IN_VOXELS` | **8** |
| `GEOMETRY_CELL_SIZE_IN_VOXELS` | **8** |
| `GEOMETRY_CELL_MAX_TRIANGLES_COUNT` | **2560** = 512 × 5 |

**[SRC]** And a fact that is easy to miss and shapes everything: there are
**three** data types in storage, not two —

```
MyStorageDataTypeEnum : Content, Material, Occlusion
```

Content is density. Material is which rock. **Occlusion is baked ambient
lighting, stored in the voxel field itself.** §8 is where that pays off.

**[SRC]** `MyStorageData` holds each type as its own flat `byte[]`, indexed
`x*sX + y*sY + z*sZ` with the steps precomputed, allocated to the next power of
two. That is exactly the layout `CLAUDE.md` §"Where DOD belongs" prescribes for a
spatial query layer, arrived at independently, and the separation by type matters:
**a mesher that only needs content never touches the material array**, which is
two thirds of the bandwidth saved on the most common query.

**[inferred] Why a byte rather than a bit.** The byte is what makes the terrain
*mineable rather than blocky*. Content 200 in one cell and 40 in its neighbour
places the surface between them at a fractional position, so a drill that removes
30% of a voxel visibly moves the surface by 30% of a metre. A boolean field can
only ever delete a whole cubic metre. The precision is not for looks; it is the
difference between carving and demolishing.

**[inferred] Why 127 and not 128.** With `EMPTY=0` and `FULL=255`, the midpoint
is 127.5. Choosing 127 makes a voxel of exactly 128 unambiguously *inside*, which
removes a tie case from every comparison in the mesher. A one-off decision worth
half a sentence of thought and never revisited.

---

## 2. Storage — the diff, and the octree that holds it

[`space_engineers.md`](../../games/space/space_engineers.md) §4.2 has the architecture: **the
procedural provider is the authority and the octree stores only the player's
diff.** What that note does not have is the machinery, which is worth reading
because it is a well-built version of a structure this project will eventually
want.

**[SRC]** `MyOctreeStorage`:

- `LeafLodCount = 4`, so `LeafSizeInVoxels = 1 << 4 = **16**`. A leaf is a
  16³ = 4096-voxel block.
- **Content and material are two separate trees**, four dictionaries:
  `m_contentNodes` / `m_contentLeaves` / `m_materialNodes` / `m_materialLeaves`.
- Tree height is derived from the map size — `while (lodSize != Zero) { lodSize >>= 1; ++m_treeHeight; }`.
- A leaf is either a `MyMicroOctreeLeaf` (real stored data, a
  `MySparseOctree` inside) or a **`MyProviderLeaf`** — a leaf that has no data
  and calls the generator instead.

**[SRC]** The compression is in two places and both are worth stealing:

1. **`MyMicroOctreeLeaf.TryGetUniformValue`** — `if (m_octree.IsAllSame)`, the
   whole 16³ block collapses to one byte held in the parent. Solid rock and empty
   space cost nothing.
2. **Writes propagate bottom-up**, and after a write the leaf is re-tested for
   uniformity and **deleted if it became uniform**. So the structure *shrinks*
   when a player fills a hole back in, not just when they dig one.

**[SRC]** And `ReadRange` decides per-node whether to descend into stored data or
call the provider, so a query straddling edited and pristine ground is answered
from both without the caller knowing.

**[inferred] The transferable shape**, stated generally because it is not really
about voxels:

> A derived-from-function world wants **three** node states, not two: *stored*,
> *uniform* (one value, no storage), and *ask the generator*. Most
> implementations have the first two. The third is what makes a pristine world
> free, and it is also what makes the generator's determinism load-bearing.

---

## 3. Polygonisation

### 3.1 The canon, in the order it was invented

| Algorithm | Vertex lives | Sharp features | Manifold | Notes |
|---|---|---|---|---|
| **Marching cubes** (1987) **[PAPER]** | on cell **edges** | no — rounds them off | yes (with the fixed tables) | 256 cases → 15 classes; ≤5 triangles per cell |
| **Surface nets** (1998) | one per **cell**, at the centroid of edge crossings | no | yes | the cheap dual method; no normals needed |
| **Dual contouring** (2002) **[PAPER]** | one per **cell**, placed by **QEF** minimisation over the edge planes | **yes** | **not guaranteed** | needs *Hermite* data — position **and normal** at each crossing |
| **Transvoxel** (2009) **[PAPER]** | MC vertices + **transition cells** | inherits MC | yes | the LOD-crack fix, see §4.4 |

**[PAPER]** The dual contouring idea in one sentence: instead of putting vertices
where the surface crosses cell edges, put **one vertex inside each cell**,
positioned to minimise the summed squared distance to all the *tangent planes*
implied by the crossings on that cell's edges. When those planes meet at an
angle, the minimiser lands on the crease and **the sharp edge survives**;
marching cubes averages it away. The paper's stated costs are the ones that bite
in practice: the QEF needs a normal per crossing (Hermite data — more than a
density field alone provides), the result **is not guaranteed manifold**, and
self-intersection can appear at coarse resolutions and across octree levels.

**[inferred] Why a game with mineable rock picks dual contouring anyway.** Two
reasons and only one is about sharp edges. The obvious one: a player-cut tunnel
mouth or a flattened building site *has* creases, and MC turns every one of them
into a soft blob. The less obvious one: **DC emits one vertex per cell, so the
mesh is smaller and its topology is stable under small edits** — nudge a density
value and the vertex moves; under MC the case index can flip and the local
triangulation changes wholesale. Stable topology is worth a great deal when the
same mesh is also a collision shape being rebuilt continuously.

### 3.2 What SE1 actually ships

**[SRC]** One interface, two implementations:

```csharp
public interface IMyIsoMesher
{
    int InvalidatedRangeInflate { get; }
    MyIsoMesh Precalc(MyIsoMesherArgs args);
    MyIsoMesh Precalc(IMyStorage storage, int lod, Vector3I lodVoxelMin, Vector3I lodVoxelMax,
                      bool generateMaterials, bool useAmbient, bool doNotCheck = false,
                      bool adviseCache = false);
}
```

`MyMarchingCubesMesher` and `MyDualContouringMesher` both implement it, and
`MyPrecalcComponent` holds the chosen one in a `[ThreadStatic]` field — **one
mesher instance per worker thread**, with its own scratch cache and output
buffer, which is how the whole thing runs without allocating.

**[SRC]** The output buffer, `MyIsoMesh`, is the interesting object:

```csharp
public readonly List<Vector3>  Positions;
public readonly List<Vector3>  Normals;
public readonly List<byte>     Materials;
public readonly List<Vector3I> Cells;
public readonly List<float>    Ambient;
public readonly List<MyVoxelTriangle> Triangles;
```

**Struct of arrays, reserved once, cleared and reused.** And `WriteVertex`
carries the giveaway:

```csharp
Debug.Assert(position.IsInsideInclusive(ref Vector3.MinusOne, ref Vector3.One));
```

**[inferred]** A vertex whose position is asserted to lie within ±1 of its own
cell is a *dual* vertex by definition — an MC vertex lives on an edge and would
be expressed differently. Together with the `Cells` list (each vertex records
which cell produced it, which only makes sense one-vertex-per-cell) this is
strong evidence that the native mesher is real dual contouring, even though the
QEF itself is not readable.

**[SRC]** `MyIsoMesh` also carries a note from Keen worth reproducing, because it
is the sort of thing that only shows up in shipped code:

> `// mk:TODO Make indices 1 int to prevent copying during HkGeometry creation if possible. Unfortunately, Havok winding order is different than ours.`

**[inferred]** i.e. the mesh is copied on its way into the physics engine purely
because of triangle winding. A whole buffer copy per collision cell, for a
convention mismatch. Worth remembering when choosing a physics library's mesh
interface.

### 3.3 The margin rule — and it is the same rule as GPU Gems'

**[SRC]** `MyDualContouringMesher`:

```csharp
const int AFFECTED_RANGE_OFFSET      = -1;
const int AFFECTED_RANGE_SIZE_CHANGE =  5;
```

and, in `Precalc`, with Keen's own comments:

```
voxelStart -= 1;                 // change range so normal can be computed at edges
voxelEnd   += 1;                 //   (expand by 1 in all directions)
                     + 1         // overlap to neighbor so geometry is stitched together within same LOD
                     + 1;        // for eg. 9 vertices in row we need 9 + 1 samples (voxels)
```

**Three separate +1s, for three separate reasons**, and confusing them is a
classic source of seams:

1. **±1 for normals.** A central-difference gradient at a boundary voxel needs
   its neighbour. Without this the normal is wrong on the cell's outer shell and
   you get a visible lighting seam on a mesh that is geometrically perfect.
2. **+1 for stitching within the LOD.** The mesher generates one cell's worth of
   geometry *plus the first row of the neighbour*, so adjacent cells agree
   exactly on the shared vertices. **Not** shared vertex buffers, **not** a
   welding pass — just deliberate overlap and identical arithmetic on both sides.
3. **+1 for the fencepost.** N vertices need N+1 samples.

**[PAPER]** GPU Gems 3 ch. 1 arrives at the identical conclusion from the other
end: it generates density in a **44³ margin volume** for a 32³ block — six voxels
of margin per side — because its per-vertex ambient occlusion rays leave the
block. Same principle, bigger margin, because AO needs more reach than a gradient
does.

**[inferred] The rule, stated so it survives leaving voxels behind:**

> **A chunked derived structure must be generated over a halo whose width is the
> reach of the widest operator applied to it.** Gradient → 1. Blur → kernel
> radius. AO rays → the ray length. Get the halo wrong and the artefact appears
> exactly on chunk boundaries, which is the hardest place to notice in a
> screenshot and the easiest to notice in motion.

### 3.4 Ambient occlusion is baked into the mesh, at mesh time

**[SRC]** `MyMarchingCubesMesher`, with Keen's comment left exactly as written:

```csharp
// Ambient light calculation is same for LOD and no-LOD. This formula was choosen by
// experiments and observation, no real thought is behind it.
for (ambientX = -VOXELS_CHECK_COUNT; ambientX <= VOXELS_CHECK_COUNT; ambientX++) { ... }
ambient /= MyVoxelConstants.VOXEL_CONTENT_FULL_FLOAT * 27;
ambient  = 1.0f - ambient;
ambient  = MathHelper.Clamp(ambient, 0.4f, 0.9f);
```

`VOXELS_CHECK_COUNT = 1`, so this is **a 3×3×3 box average of the density field,
inverted, clamped to [0.4, 0.9]**, written per vertex into `MyIsoMesh.Ambient`
and carried all the way to the GPU in the vertex stream.

**[inferred]** It is nine lines and it is most of what makes SE's terrain read as
solid. The honesty of "no real thought is behind it" is worth taking seriously:
**the density field is already an occupancy field, so a box filter over it is a
free approximation of local occlusion**, and at 27 samples of an array you are
about to read anyway it costs nothing. It is a much better deal than SSAO for a
static-until-edited surface, because it is exact per vertex, temporally stable by
construction, and recomputed only where the player digs.

The clamp is the tell that it is a *look* control rather than a physical
quantity — no crevice ever goes darker than 0.4, no face ever brighter than 0.9,
so the term can never fight the real lighting.

**[SRC]** And the long-range half is elsewhere: the third storage type,
`Occlusion`, is produced by the **planet provider** (`ProvidesAmbient => true`),
so large-scale terrain shadowing is a *stored field* while small-scale contact
darkening is *computed at mesh time*. Two scales, two mechanisms, both cheap.

### 3.5 Materials

**[SRC]** Marching cubes picks the material of whichever end of the edge has more
content:

```csharp
float mu2 = contentB / (contentA + contentB);
edge.Material = (mu2 <= 0.5f) ? materialA : materialB;
```

**[SRC]** And the request carries `SurfaceMaterial | ConsiderContent` — the
storage is told materials are only needed **on the surface**, not throughout the
volume, which lets the planet's material provider skip everything below ground.
`MyVoxelRequestFlags` is a small masterclass in this kind of hint-passing:
`EmptyContent`, `FullContent`, `OneMaterial`, `ContentChecked`,
`ContentCheckedDeep`, `AdviseCache`, `DoNotCheck`. **[inferred]** Every one of
those exists so a *provider* can tell a *consumer* "do not bother iterating" —
the whole-range early-out is the algorithmic win and the flags are its interface.

---

## 4. LOD — the part everyone underestimates

### 4.1 The clipmap

**[SRC]** `VRage/Voxels/MyClipmap.cs`, and it is a clipmap in Hoppe's sense:
nested shells of cells at doubling resolution centred on the viewer, scrolled
rather than rebuilt.

| Thing | Value | Source |
|---|---|---|
| Max LODs | `MyCellCoord.MAX_LOD_COUNT = 1 << 4` = **16** | [SRC] |
| Render cell, LOD 0–4 | `1 << 5` = **32³ voxels** (32 m at LOD 0) | [SRC] |
| Render cell, LOD ≥ 5 | `1 << 4` = **16³ LOD-voxels** | [SRC] |
| Geometry cell | **8³ voxels** — 4×4×4 of them per LOD-0 render cell | [SRC] |
| Reclip threshold | camera moved `cellSizeHalf / 4`, or rotated `0.03` | [SRC] |
| Cross-fade | `CellsDitherTime = 1.5f` seconds, dither 0→4 | [SRC] |
| Scale groups | `MyClipmapScaleEnum { Normal, Massive }` — asteroid vs planet | [SRC] |

**[inferred] Two of those deserve comment.**

**The 32³ render cell is the same number GPU Gems 3 picked**, independently, and
for the same reason: it is the block size where the vertex buffer is worth a draw
call, the mesh job is worth a task, and the frustum-cull granularity is still
useful. Not a coincidence so much as a convergence.

**The halving at LOD 5 is the non-obvious one.** From LOD 5 up, a render cell is
built from *half* as many voxels per axis. `RenderCellSizeInLodVoxelsShiftDelta`
returns `-1` above `CELL_SIZE_THRESHOLD_LOD = 5`, and the comment says exactly
that. **[inferred]** The reason is that world extent per cell is doubling every
level regardless — a LOD-8 voxel is 256 m — so keeping 32³ would make a single
cell four kilometres across, which is useless as a culling unit and produces one
enormous mesh job. Keen trade cell *count* for cell *size* at the point where
size starts to hurt. **The lesson is that a LOD ladder's cell size should not be
constant in cells; it should be roughly constant in screen space, and that means
changing the ratio somewhere up the ladder.**

**[SRC]** Cell requests are batched through a `RequestCollector` and prioritised;
`m_invalidated == 2` is a special **"drill priority"** state that processes LOD 0
only. **[inferred]** That is a nice bit of pragmatism: when the player is
actively cutting, nothing matters except the cell under the drill, and coarse
levels can wait a frame.

### 4.2 Geomorphing — how SE hides the LOD switch

This is the single most transferable mechanism in the note, and it is spread
across three files.

**[SRC] Step 1, in the job.** `MyPrecalcJobRender` meshes the cell **twice**:

```csharp
if (m_args.Cell.Lod < 15 && MyFakes.ENABLE_VOXEL_LOD_MORPHING)
{
    min >>= 1; max >>= 1;      // the same region, one LOD coarser
    // ... second Precalc, producing "vertex morph targets"
}
```

**[SRC] Step 2, in `MyRenderCellBuilder`.** The coarse mesh's vertices are
transformed into the fine mesh's coordinate space —

```
x_h = x_l * (scale_l / scale_h) + ((offset_l - offset_h) / scale_h)
```

— and then, **for every fine vertex, the nearest coarse vertex is found by
squared distance** and its position, normal, material and ambient are copied in
as that fine vertex's morph target.

**[SRC] Step 3, in the vertex shader.** `VertexTransformations.hlsli`:

```hlsl
float voxel_morphing(float3 position_a, float2 bounds, float3 local_viewer)
{
    float3 diff = abs(position_a - local_viewer);
    float dist = max(diff.x, max(diff.y, diff.z));
    return saturate(((dist - bounds.x) / (bounds.y - bounds.x) - 0.35f) * 2.0f);
}
```

and then, in `VertexTemplateBase.hlsli`:

```hlsl
position_object   = lerp(__position_object, __position_object_morph, morphing);
__normal          = normalize(lerp(__normal, __normal_morph, morphing));
__material_weights = lerp(__material_weights, __material_weights_morph, morphing);
```

**[inferred] Read that as a design and it is very good.**

- The distance metric is **Chebyshev**, not Euclidean — `max(|dx|,|dy|,|dz|)`.
  That is the metric whose iso-surfaces are *cubes*, which is the shape a clipmap
  shell actually is. Using Euclidean here would make the morph front bulge
  through the cell boundaries and the transition would not line up with the LOD
  swap.
- The `-0.35` bias and `×2` mean the morph does nothing for the first ~35% of the
  LOD band and completes by ~85%. **The mesh is fully morphed to the coarse shape
  before the coarse mesh is ever swapped in** — so the swap is geometrically a
  no-op, which is the entire point.
- **Normal and material weights morph too.** Position alone would still pop the
  shading and the texture. This is the detail most implementations miss.
- The whole thing costs **one extra mesh job per cell and a doubled vertex
  stream**. That is not free, and it is the honest price of no popping.

### 4.3 And *then* a dither on top

**[SRC]** `MyClipmap.LodLevel` additionally:

- cross-fades a cell in and out over `CellsDitherTime = 1.5` seconds, dither
  value ramping 0→4;
- refuses to hide a parent until `ChildrenWereLoaded(childLod, ...)`;
- refuses to show a fine cell until `AllSiblingsWereLoaded(...)`.

**[inferred]** So there are **three** overlapping mechanisms guarding one
transition: morph the geometry so the swap is invisible, dither the swap so a
mistake is a fade rather than a pop, and gate the swap on the neighbourhood being
ready so a mesh job that is still queued never punches a hole in the world. That
redundancy is not over-engineering — it is what asynchronous meshing forces. The
mesh you want may simply not exist yet, and every one of those three exists to
answer "what do we draw in the meantime".

### 4.4 Cracks — and why SE does not use Transvoxel

**[PAPER]** The canonical fix is **Transvoxel** (Lengyel, 2009): between a cell at
one resolution and a cell at half that resolution, insert a **transition cell**
sampled at **nine points** on the high-resolution face and four on the low —
**512 cases, 73 equivalence classes** — whose triangles exactly fill the seam.
The high-resolution mesh is shrunk slightly inward to make room for the
transition band.

**[SRC/inferred] SE does not do this.** There are no transition tables anywhere
in the source, and the mechanisms above are sufficient because of a structural
choice: **the morph makes the fine mesh converge to the coarse mesh's geometry
before the boundary is ever crossed**, so at the moment two LODs are adjacent
their shared boundary vertices agree. Within a LOD, §3.3's one-voxel overlap
guarantees agreement exactly. The crack case Transvoxel exists for is designed
out rather than patched.

**[inferred] The trade, stated fairly**, because both answers are defensible:

| | Transvoxel | SE's morph-and-gate |
|---|---|---|
| Cost | 73 extra case tables, transition geometry per boundary cell | **a second mesh job per cell**, doubled vertex stream |
| Correctness | exact, watertight by construction | exact *if* the morph completes before the swap — a tuning dependency |
| Handles | arbitrary adjacent LOD pairs | adjacent levels only, gated by the loader |
| Also gives you | nothing else | **no popping**, which you needed anyway |

The second row is the real argument. If you are going to implement geomorphing
regardless — and a game with a moving camera over a LOD'd surface must — then the
crack fix comes free with it, and Transvoxel's tables are solving a problem you
have already paid to remove. **[inferred]** Transvoxel is the better answer if
you want hard LOD switches (cheaper vertices, no double meshing) and can tolerate
popping or hide it another way.

---

## 5. The two GPU Gems chapters, read properly

### 5.1 GPU Gems 3, ch. 1 — Geiss, *Generating Complex Procedural Terrains Using the GPU*

**[PAPER]** This one is genuinely about the same problem, on 2007 hardware, and
it is worth reading for two things: the density-function construction, and the
three-pass evolution of the mesher.

**Structure.** Infinite 1×1×1 world blocks, each tessellated into **32³ voxels**
(33³ density corners, plus margin). Density is evaluated by pixel shader into a
3D texture; marching cubes runs in a **geometry shader with stream-out**.

**The optimisation story is the useful part**, because it is a clean instance of
`CLAUDE.md`'s "do less work" ordering, measured on a GeForce 8800:

| Method | Passes | What changed | Blocks/sec |
|---|---|---|---|
| 1 | 2 | GS emits up to 15 vertices per voxel, straight to VB | **6.6** |
| 2 | 3 | GS emits only **5 marker uints**; vertex generation moved to a VS; stream-out query skips later passes when empty | **144** (22×) |
| 3 | 5 | **indexed** vertex pool — each vertex generated once instead of ~5× | **260** |

**[inferred]** Note what the 22× actually was: **not a faster inner loop, but
moving work out of the geometry shader** — which on that hardware had a
catastrophic output-size-dependent cost — plus an early-out that skips three
passes for empty blocks. The 80% on top was pure **de-duplication**: 5 redundant
vertices per shared vertex, removed by splatting vertex ids into a 3D texture and
building an index buffer from it. Both are structural. Neither is micro.

**The density function**, which is the part people quote:

```
float density = -ws.y;                        // ground plane
density += noise(ws * f) * a;                 // ×9 octaves, f doubling, a halving
// optional: warp ws by low-frequency noise before the octaves
density += saturate((hard_floor_y - ws.y) * 3) * 40;   // sediment floor
float density = rad - length(ws - centre);    // ...or a planet
```

**[inferred]** The warp is the single highest-value line in it — it is what turns
"fractal noise, which always looks like fractal noise" into arches and
overhangs, and it costs one extra noise evaluation. It is also **exactly what SE2
does not do**, and §7.2 explains why.

**Vertex format and AO.** 7 floats: `float4 wsCoordAmbo` (position + AO in w) and
`float3 wsNormal`. Normals from a six-tap central-difference gradient. AO from
**32 Poisson-distributed rays** per vertex — 16 short (against the density
volume) and 4 long (against the density function) — hence the **44³ margin
volume** for a 32³ block.

**[inferred] Read against SE:** the *format* is nearly identical (position,
normal, one AO scalar), and the AO is conceptually identical (occlusion baked at
mesh time, not screen space). SE spends 27 array reads on it; Geiss spends 32
rays. SE's is roughly free and gives contact darkening only; Geiss's is expensive
and gives real medium-scale occlusion. **[inferred]** For a game, SE's is the
better trade and the gap is filled by the *stored* occlusion field (§3.4) — a
third option neither chapter considers.

**Its LOD answer.** Geiss considers and rejects "fewer polygons per block"
(block count bloats) in favour of "bigger blocks" — constant 32³, world size
1/2/4 by distance — with alpha-fade during transitions and a z-bias so the
high-LOD block encases the low. **[inferred]** That is SE's clipmap without the
clipmap: same ladder, cross-fade instead of morph, and he names the z-fighting it
causes as a limitation. SE's morph is strictly the better version of this, and
the chapter's own "Limitations" section is where you can see why.

### 5.2 GPU Gems 2, ch. 2 — Asirvatham & Hoppe, *Terrain Rendering Using GPU-Based Geometry Clipmaps*

**[PAPER]** This is a **2D heightfield** technique. It cannot express a cave, an
overhang or a hole, and it is not editable in any interesting sense. What it
supplies is **the LOD structure SE's voxel clipmap is built on**, so it is worth
reading for the structure and not for the terrain.

The numbers, which are the reason to read it:

| | |
|---|---|
| Level size | **n = 255**, i.e. 2^k − 1, deliberately odd |
| Levels | L = 11 |
| Ring decomposition | **12 blocks of m×m**, m = (n+1)/4 = **64**, all sharing **one** canonical vertex/index buffer, scaled and translated in the shader |
| Plus | four m×3 fix-up strips, an L-shaped trim (4 variants), degenerate triangles on the perimeter |
| Transition band | **n/10** wide, blend `z' = (1−α)z_f + α z_c` |
| Dataset | 216,000 × 93,600 samples at 30 m, compressed **>100:1** into **355 MB** |
| Update cost | upsample 1.0 ms, decompress **8 ms (CPU-bound)**, normals 0.6 ms |
| Rate | 130 fps / **60 M tri/s**; 87 fps flying with decompression, 120 fps synthesised |

**[inferred] Four ideas transfer and the rest does not.**

1. **n = 2^k − 1, not 2^k.** The odd size guarantees the fine level's perimeter
   lands *on* coarse-level samples and that the fine level is never exactly
   centred. That is the crack fix and it is purely a choice of dimension. **The
   general lesson: alignment between LOD levels is a property of your grid sizes,
   and picking them wrong makes you write stitching code forever.**
2. **One canonical vertex buffer, twelve instances.** The entire ring is one
   64×64 grid drawn twelve times with a scale/offset in the shader. Nothing about
   this needs a heightfield.
3. **The blend band, `z' = lerp(z_f, z_c, α)` over n/10.** That is SE's
   `voxel_morphing` in 1996 clothing — same equation, same purpose. SE's addition
   is that a voxel surface has no natural "coarse elevation at this position", so
   the coarse value has to be found by nearest-vertex search on the CPU (§4.2)
   rather than read from a texture. **That is the whole difference between
   morphing a heightfield and morphing an isosurface, and it is why the voxel
   version costs a second mesh job.**
4. **Toroidal update.** As the viewer moves, the window wraps rather than
   translating — the update region is an L (usually written as a `+` via two quad
   renders). Any scrolling cache wants this and most implementations rebuild
   instead.

And the honest note: **the decompression at 8 ms is the CPU bottleneck in their
own numbers**, on a technique whose entire selling point is moving work to the
GPU. Streaming is the cost that survives every generation of this idea.

---

## 6. Editing — drills, explosions, and how a crater is actually made

**[SRC]** `MyVoxelGenerator` is the write path, and it has exactly three
operations:

| Operation | Effect |
|---|---|
| `CutOutShape` | for each voxel, compute the **volume of intersection** with the shape and subtract that fraction of content |
| `FillInShape` | the same, added, clamped to `FULL`, and sets material |
| `PaintInShape` | material only, on voxels with content > 0.5 — **no geometry change at all** |

**[SRC]** It processes in `const int CELL_SIZE = 16` blocks for cache coherence,
accumulates `originalSum` and `removedSum` so **mining yield is literally the
integral of removed density**, and returns the exact material breakdown in an
`exactCutOutMaterials` dictionary.

**[inferred]** The volume-fraction detail is what makes the drill feel like a
drill. A binary "is the voxel centre inside the sphere" test quantises removal to
whole cubic metres and the surface advances in visible steps; integrating the
overlap makes it advance smoothly, and it makes the yield exactly proportional to
the hole, which is the difference between a mining game and a hole-punching game.

**[SRC] `MakeCrater` — impact damage — is three concentric spheres**, and this is
the neatest thing in the file:

| Sphere | Radius | Job |
|---|---|---|
| addition | **1.5×** | adds material — the raised lip |
| deletion | **1.0×** | removes material — the bowl |
| material | **0.1×** | sets a scorched/damage material at the centre |

with the deletion sphere offset along the impact direction by
`digRatio = 1 - dot(normal, direction)`, so **a glancing hit gouges a long shallow
furrow and a perpendicular hit punches a round pit**, from one dot product.

**[SRC]** And afterwards, a cleanup pass whose comment is
`"Clear all small voxel that may have been created during explosion"` — removing
isolated sub-iso-level voxels so the blast does not leave a fog of one-metre
floaters. **[inferred]** Every subtractive-CSG destruction system needs this pass
and most discover it the hard way.

**[KEEN] SE2 generalises the same mechanism to all collisions:**

> furrows are created using "**metaballs (SDFs) composed out of spheres in contact
> points with radius based on their impulse**"

**[inferred]** which is `MakeCrater` promoted from a weapon effect to a physics
callback: the contact set supplies the sphere centres, the impulse supplies the
radii, and the subtraction is the operation the field already supports. One
mechanism, and the impulse maps onto the radius with no tuning curve in between.

**[SRC] Invalidation** is where the halo rule (§3.3) reappears:
`InvalidatedRangeInflate => AFFECTED_RANGE_SIZE_CHANGE + AFFECTED_RANGE_OFFSET`
= 5 + (−1) = **4 voxels**. An edit dirties a region four voxels wider than it
touched, in every direction, because that is the reach of the meshing operator.
Get this number too small and you get seams at the edge of every hole; too large
and you re-mesh cells that did not change.

---

## 7. Collision — and it is not the render mesh

**[SRC]** `MyVoxelPhysicsBody` builds `HkBvCompressedMeshShape` per geometry cell
— **a compressed BVH-over-triangles, not a compound of primitives, not a
heightfield**. Built from the *same* iso-mesher output, at geometry-cell
granularity (8 m), on worker threads.

**[SRC]** Four mechanisms make it affordable:

1. **Two physics LODs.** `UseLod1VoxelPhysics`: LOD 0 at 8 m cells, LOD 1 at
   16 m. **Characters always get LOD 0**; dynamic grids and floating objects get
   LOD 0; everything else may use LOD 1. **[inferred]** i.e. fidelity is chosen
   per *consumer*, not per distance — the thing that would notice a 16 m
   approximation is the thing that walks on it.
2. **Predictive prefetch.** `MyPrecalcJobPhysicsPrefetch` watches nearby entities
   and queues the cells they are about to enter, skipping any already in the work
   tracker. Collision geometry that arrives when you touch it has already
   arrived too late.
3. **Batched invalidation.** Edits mark cells invalid; a batch task per LOD
   regenerates them; `RunningBatchTask[lod]` ensures one in flight.
4. **Discard.** `SHAPE_DISCARD_CHECK_INTERVAL = 18` (in 10-frame ticks) with
   `SHAPE_DISCARD_THRESHOLD = 0` hits — a shape nothing has queried since the
   last check is thrown away.

**[SRC]** Separately, `MyVoxelGeometry` keeps an LRU cache of unpacked cell
geometry with a comment that settles what it is for:

> `"Don't store anything but the most detailed lod (used in physics and raycasts). This cache is mostly supposed to help physics and raycasts, not render."`

**[inferred] Which is [`navigation.md`](../agents/navigation.md)'s claim again, from a third
direction: spatial queries and rendering are different consumers with different
residency needs, and one cache cannot serve both.** The renderer wants coarse
data far away; the raycaster wants exact data near. Sharing a cache between them
means one of the two is always wrong.

---

## 8. Making it look like rock

### 8.1 Triplanar, with distance tiers

**[SRC]** `Shaders/Geometry/TriplanarSampling.hlsli`:

```hlsl
float3 w = saturate(abs(n) - 0.55);
w *= w; w *= w;              // sharpen
return w / dot(w, 1);        // normalise
```

**[inferred]** Compare GPU Gems 3's version — `(abs(n) - 0.2) * 7`, clamped and
normalised. Same idea; SE's `x⁴` falloff gives a much narrower blend band, i.e.
**less of the surface pays for three texture fetches**, at the cost of a slightly
harder transition. That is the right trade once you are sampling three materials
as well as three axes (§8.2) and the fetch count is multiplying.

**[SRC/COMMUNITY]** On top of that, **each material has three distance tiers plus
a flat colour**:

| Tier | Default scale | Default distance |
|---|---|---|
| Close | `InitialScale` 2 | `InitialDistance` 5 m |
| Far1 | 400 | cascaded by `DistanceMultiplier` 4 |
| Far2 | 2000 | " |
| Far3 | **solid colour** (`Far3Color`) | " |

blended by

```hlsl
float scaleWeight = saturate(((d - distanceNear) / (distanceFar - distanceNear) - 0.5f) * 2.0f);
[branch] if (scaleWeight <= 0.995f) { /* sample near */ }
[branch] if (scaleWeight >= 0.005f) { /* sample far  */ }
```

**[inferred] Three things worth taking.** The tiers exist because a texture tiled
for 1 m detail becomes visible repetition at 500 m and moiré at 5 km — different
distances need different *frequencies*, not different mip levels. The branches
mean **the two-sample cost is only paid inside the blend band**, which is most of
the win. And `Far3` collapsing to a constant colour is the correct terminal case:
at that range the surface is a few pixels and a texture fetch buys nothing.

### 8.2 Up to three materials per triangle

**[SRC]** `MyRenderCellBuilder` splits a cell's geometry into two kinds of batch:

- **single-material**, when all three vertices *and all three morph targets*
  agree — deduplicated through a `VertexInBatchLookup` whose "clear" is a counter
  increment rather than a memset;
- **multi-material**, up to **three** distinct materials, identified by
  `materials.X + ((materials.Y + (materials.Z << 10)) << 10)`, drawn by
  `TriplanarMulti` with per-vertex weights.

**[SRC]** `TriplanarMulti/Pixel.hlsl`:

```hlsl
[branch] if (mat_weights[t] >= 0.0005f)
{
    SampleTriplanarBranched(t, material, triplanarInput, triplanarOutput);
    triplanarWeighted.cm  += triplanarOutput.cm  * mat_weights[t];
    triplanarWeighted.ng  += triplanarOutput.ng  * mat_weights[t];
    triplanarWeighted.ext += triplanarOutput.ext * mat_weights[t];
}
```

**[inferred]** Worst case that is **3 materials × 3 axes × 2 distance tiers = 18
texture-set fetches for one pixel**, which is why every level of it is branched
and why the triplanar weights are sharpened to `x⁴`. The two batch types exist so
the overwhelming majority of the surface — one material, one triangle — never
enters that shader at all. **The general pattern: split the draw by how hard the
case is, not by what object it belongs to.**

**[SRC]** Limits: `MAX_VERTICES_COUNT = 65535` (16-bit indices),
`MAX_INDICES_COUNT = 100000`, batches closed at 65532 / 99997.
**[COMMUNITY]** and **128 visible voxel materials per world**, all textures
resident in VRAM, of which vanilla uses about half.

### 8.3 What SE2 changed

**[KEEN]** From the August 2025 diary, verbatim:

> "In Space Engineers 1 (SE1), material blending between voxels occurred simply
> over the surface of a single voxel (one triangle). In Space Engineers 2 (SE2),
> these transitions can extend across multiple voxels and triangles. Additionally,
> a **transition mask with height information** allows for significantly more
> natural-looking material transitions."

**[inferred]** "Height information" means height-blend / heightlerp: instead of
`lerp(a, b, t)`, compare each material's height channel offset by `t` and take
the winner. It costs one channel and it is the difference between sand *dissolving
into* rock and sand *fading over* rock — the latter reads as a decal, which is
exactly what SE1's one-triangle blend looked like.

**[KEEN/ENG]** SE2 also adds tessellated voxel materials, with the stabilisation
note that the displacement mip is chosen "using the approximate triangle size"
(see [`space_engineers.md`](../../games/space/space_engineers.md) §4.5), and ray-traced GI whose
stated constraint is visual stability inside *moving* interiors (§5.5 there).

### 8.4 Shadows

**[inferred]** Nothing SE does for shadows is voxel-specific — cascades, the
sampling work in [`space_engineers.md`](../../games/space/space_engineers.md) §5.4 — with one
exception worth naming: **a voxel surface is the ideal shadow-map client and the
worst shadow-map subject.** Ideal because it is static between edits, so a cached
cascade stays valid; worst because it is enormous, low-contrast and viewed at
grazing angles from orbit, which is where every cascade scheme has its moiré. The
grazing-angle fix Keen name — adjusting kernel *radius* rather than dropping to a
coarser mip — is a terrain fix specifically.

---

## 9. Where the field comes from

### 9.1 Asteroids are CSG plus two noise layers

**[SRC]** `MyCompositeShapes.cs`, and it is much simpler than it looks in game:

- **two filled shapes, unioned**, and **two removed shapes, subtracted**;
- the primary filled shape is a **torus 33%** of the time, a **sphere 67%**;
- the subtracted shapes are **sphere 14% / torus 43% / capsule 43%**;
- primitives available: `MyCsgShapeSphere`, `MyCsgShapeCapsule`, `MyCsgShapeTorus`,
  `MyCsgBox`.

Then two noise modules:

```
macro:  MySimplexFast(seed, frequency: 7f / size)
detail: MyRidgedMultifractalFast(frequency: rand*0.09 + 0.11, layerCount: 1)
     or MyBillowFast          (frequency: rand*0.07 + 0.13, layerCount: 1)
        // both quality: MyNoiseQuality.Low
```

**[SRC]** and `MyCsgShapeSphere.SignedDistance` applies them in two bands:

- **macro** always: perturb the radius by `m_halfDeviation` scaled by noise at
  `m_deviationFrequency`;
- **detail** only when the signed distance is already within `±m_detailSize` of
  the surface — **the fine noise is not evaluated anywhere it could not possibly
  matter**;
- with hard early-outs: `if ((m_innerRadius - lodVoxelSize) > distance) return -1f;`
  and the mirror for outside.

**[inferred] That is the whole asteroid**, and the shape of it is the lesson: a
handful of analytic primitives with signed distances, combined by min/max, with
noise added *only in the shell where the surface can be*, and every level
short-circuited by a radius test. `layerCount: 1` and `quality: Low` are the
tell that Keen were paying attention — this function is called per voxel per LOD
across a solar system, and one octave of cheap noise in a thin band beats nine
octaves everywhere by an enormous margin. **Compare Geiss's nine-octave
everywhere-evaluated density function (§5.1), which is beautiful and would be
ruinous at this scale.**

**[SRC]** Ore is placed as its own shapes — `MyCompositeShapeOreDeposit`,
`MyBoxShapeOreDeposit` — count `max((int)log(size), filledShapes.Length)`, with
iron in the core and uranium/ice entered twice in the draw to raise their odds.

### 9.2 Planets are a cube-map heightfield, sampled bicubically

**[SRC]** `MyPlanetShapeProvider` — six faces, and the sampling is better than the
usual bilinear:

- **Catmull–Rom bicubic**, `Tau = 0.5`, evaluated as `S · CR · C · CRᵀ · T` with
  the inner `Gz = CR·C·CRᵀ` **cached per 4×4 patch** in a thread-local
  **256-entry cache** keyed by (face, sx, sy);
- **Bézier bounds** per patch (`Min`/`Max` via `BInv`), so a whole voxel request
  can be rejected before any per-voxel work: `if (rate > cell->Max + lodHeight) return NegativeInfinity;`
- **bilinear fallback** when `lodSize >= m_pixelSize` — i.e. **bicubic is only
  paid for when the voxels are finer than the heightmap pixels**, which is the
  only situation where it can possibly show;
- a **detail sampler** applied only where the slope matches
  (`if (m_detail.Matches(Norm.Z))`), tiled by `faceSize / Size`;
- and the whole-region early-outs: outside `OuterRadius + lodVoxelSize` →
  `EmptyContent`, inside `InnerRadius - lodVoxelSize` → `FullContent`.

The final conversion, which is the line that turns a distance into a voxel:

```csharp
var fillRatio = MathHelper.Clamp(-signedDist, -1f, 1f) * 0.5f + 0.5f;   // → [0,255]
```

**[inferred] Five separate early-outs on one function.** That is what a density
provider looks like when it has been optimised by measurement rather than by
taste, and every one of them is `CLAUDE.md`'s first rule — do less work — rather
than its second.

### 9.3 The resolution question, resolved

The community modding guides give SE1 planets **2048² per cube face**, 16-bit
elevation. **[inferred] The arithmetic**, since nobody states it: a cube-sphere
face spans ~90° of arc, so its edge is about `(π/2)·R`.

| Body | Diameter | Face edge | Metres per heightmap pixel at 2048² |
|---|---|---|---|
| Earthlike / Mars / Alien | 120 km | ~94 km | **~46 m** |
| Moon | 19 km | ~15 km | **~7 m** |

So SE1's terrain detail floor is **the heightmap, not the voxel** — 1 m voxels
interpolating between samples ~46 m apart. That is why SE1 hills look like, in
Keen's own words, "pyramid-shaped hills and simple rock formations".

**[KEEN]** And the SE2 figure, from the August 2025 diary:

> "a voxel's size is 1 meter, and the current heightmap resolution is one pixel
> per 8 voxels, though a 'detail heightmap' ensures an effective resolution of
> one pixel per 1 voxel, resulting in 1-meter resolution"

**[inferred]** 8 m per pixel on a 120 km planet needs ~11,800 pixels per face —
roughly **6× SE1's linear resolution, 33× the data** — so either SE2's planets
are smaller, or the base map is streamed/tiled rather than resident. Keen have
not said. What is clear is the *shape* of the answer: **a coarse base heightmap
plus a tiled detail heightmap**, which is the heightfield equivalent of §9.1's
macro-plus-detail noise, and it is how you get metre-scale relief without storing
a metre-scale map.

### 9.4 The third dimension, bolted on

**[KEEN]** SE2's overhangs and caves, verbatim:

> "the game also spawns small voxel storages of overhangs and boulders throughout
> the environment... the surface is no longer merely 2.5D (the maximum achievable
> with a heightmap) but fully 3D. Additionally, we have caves, as the game now
> also **subtracts from the surface** to generate them."

**[inferred]** So the base is *still* a heightfield, and 3D-ness is **composited**
on: placed voxel storages add overhangs and boulders, and a subtractive pass cuts
caves. Which is the same operation as the player's drill and as §6's crater —
**the generator uses the edit path.** That is a genuinely elegant consequence of
storing edits as a diff: anything that can be authored as an edit is free to
generate, and it costs storage only where it happens.

It is also why SE2 did *not* go to Geiss's warped 3D noise (§5.1) despite that
being the "purer" answer. A full 3D density function over a 120 km planet is
evaluated everywhere; a heightfield plus placed exceptions is evaluated cheaply
everywhere and expensively in the 1% of places that need it. **The cheap answer
that covers 99% plus an escape hatch beats the general answer** — which is
`CLAUDE.md`'s derived-cache rule wearing a different hat.

---

## 10. The other shipped answers, briefly

**[COMMUNITY]** for all of these; none has a source release, and only No Man's
Sky has a proper talk.

| Game | Representation | Notes |
|---|---|---|
| **No Man's Sky** | voxel density → polygonisation → texturing, generated continuously as you fly | GDC 2017, Innes McKendrick, *Continuous World Generation in 'No Man's Sky'*. The talk's stated goal is "augmenting artists rather than replacing them" — the same conclusion [`elite_dangerous.md`](../../games/space/elite_dangerous.md) §3.6 records Frontier reaching. |
| **Astroneer** | density field, chunked, **re-polygonised per edit** | System Era rewrote it once between Early Access and 1.0. Their description of deformation is the canonical one: modify the *density*, not the mesh, then re-run polygonisation **for that chunk only**. |
| **Dual Universe** | **dual contouring**, **25 cm** voxels, unbounded LOD | The other shipped DC game, and the same 25 cm quantum SE2 chose independently. Their player-facing "vertex precision tool" is literally exposing the DC vertex position to the player, which is only possible *because* it is dual. |
| **Teardown** | cubic voxels, **ray-marched, no triangles at all** | The opposite architecture. No mesher, no LOD, no collision mesh — the GPU marches rays through voxel volumes. AO, soft shadows and specular occlusion are ray-traced in voxel space; 8-bit palette, 255 materials per volume. Worth knowing exists, because it makes every problem in §3–§5 disappear and replaces them with one enormous one. |
| **Minecraft** | cubic, naive per-face meshing | §0. |

**[inferred] The pattern across all of them:** every game that wants *smooth,
mineable* terrain converges on a density field plus a dual method (DC or surface
nets), chunked at 16–32 cells, re-meshed per chunk per edit, with a clipmap or
octree LOD. That convergence is strong evidence the design space is genuinely
narrow — which is useful, because it means the decisions worth agonising over are
the ones in §4 (how you hide the LOD switch) and §8 (how you texture it), not the
mesher.

---

## 11. What transfers here

**[inferred]** This project has a tile lattice, not a density field, and is not
going to grow voxel terrain. So most of §3 and §9 is background. These are the
parts that are about something else:

**Directly applicable now:**

1. **The halo rule (§3.3).** *Any* chunked derived structure must be generated
   over a margin as wide as the widest operator applied to it — 1 for a gradient,
   kernel radius for a blur, ray length for occlusion. This applies to
   `StoreyGeometryEmitter`, to any lightmap or AO bake, and to any future
   chunked recompute. Getting it wrong produces artefacts *exactly on chunk
   boundaries*, which is the hardest failure to see in a still.
2. **Cheap baked occlusion from the occupancy data you already have (§3.4).**
   SE's AO is a 3×3×3 box average of the density field, inverted and clamped —
   27 reads of an array the mesher is already touching. This project has
   `OccupancyGrid`, which is the same kind of field. A per-vertex or per-surface
   contact-darkening term computed at geometry-emit time would be nearly free and
   is the single cheapest thing that makes lattice geometry stop looking like
   cardboard — alongside the bevel trick in
   [`space_engineers.md`](../../games/space/space_engineers.md) §5.6, which it complements exactly.
3. **Split the draw by case difficulty, not by object (§8.2).** SE has a
   one-material fast batch and a three-material slow one, and the fast one covers
   almost everything. Any shader with an expensive general case wants this split.
4. **Volume-fraction edits, not binary ones (§6).** If anything in this project
   ever partially removes a cell — damage, terrain deformation, cover
   destruction — integrating the overlap rather than testing the centre is the
   difference between a smooth response and a stepped one, and it makes the
   "how much did I remove" question answerable exactly.

**If a LOD'd, chunked, editable surface is ever built:**

5. **Geomorph before you switch, and gate the switch on the neighbourhood
   (§4.2–4.3).** The full recipe: mesh at both levels, match each fine vertex to
   its nearest coarse vertex on the CPU, lerp position **and normal and material
   weights** in the vertex shader by **Chebyshev** distance through the LOD band,
   complete the morph before the swap, dither the swap anyway, and refuse to
   swap until children and siblings have loaded. Three redundant mechanisms
   because asynchronous generation means the geometry you want may not exist yet.
6. **Choose LOD grid sizes so the levels align (§5.2).** Hoppe's `n = 2^k − 1` is
   a crack fix that is entirely a choice of dimension. Alignment is cheaper than
   stitching.
7. **Change the cells-per-chunk ratio up the ladder (§4.1).** SE halves it at
   LOD 5 because world extent per chunk doubles regardless. A ladder that is
   constant in cells is not constant in screen space, and the coarse end is where
   it goes wrong.
8. **Physics fidelity chosen per consumer, not per distance (§7).** Characters
   always get LOD 0. What would notice the approximation is what walks on it.

**Not applicable, and worth saying so:**

- **Marching cubes, dual contouring, Transvoxel, QEFs.** Interesting, and this
  project has no isosurface to extract.
- **Geiss's nine-octave GPU density function.** Beautiful and, per §9.1, exactly
  the thing a shipped game had to avoid.
- **Teardown's ray-marched voxels.** A whole-engine commitment, not a technique.

---

## Sources

**SE1 released source — read directly (primary)**

- Meshers and jobs: [`IMyIsoMesher.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/IMyIsoMesher.cs) · [`MyDualContouringMesher.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyDualContouringMesher.cs) · [`MyMarchingCubesMesher.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyMarchingCubesMesher.cs) · [`MyPrecalcComponent.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyPrecalcComponent.cs) · [`MyPrecalcJobRender.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyPrecalcJobRender.cs) · [`MyRenderCellBuilder.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyRenderCellBuilder.cs) · [`MyVoxelVertex.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyVoxelVertex.cs)
- Clipmap and coordinates: [`MyClipmap.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyClipmap.cs) · [`MyClipmap.LodLevel.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyClipmap.LodLevel.cs) · [`MyVoxelCoordSystems.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyVoxelCoordSystems.cs) · [`MyCellCoord.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyCellCoord.cs) · [`MyVoxelConstants.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyVoxelConstants.cs) · [`Enums.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/Enums.cs) · [`MyStorageData.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage/Voxels/MyStorageData.cs)
- Storage: [`MyOctreeStorage.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Storage/MyOctreeStorage.cs) · [`MyMicroOctreeLeaf.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Storage/MyMicroOctreeLeaf.cs) · [`MyCsgShapeBase.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Storage/MyCsgShapeBase.cs) · [`MyCsgShapeSphere.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Storage/MyCsgShapeSphere.cs)
- Editing, physics, caching: [`MyVoxelGenerator.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyVoxelGenerator.cs) · [`MyVoxelPhysicsBody.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyVoxelPhysicsBody.cs) · [`MyVoxelGeometry.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/MyVoxelGeometry.cs)
- Generation: [`MyCompositeShapes.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Game/World/Generator/MyCompositeShapes.cs) · [`MyPlanetShapeProvider.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Planet/MyPlanetShapeProvider.cs) · [`MyPlanetStorageProvider.cs`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/Sandbox.Game/Engine/Voxels/Planet/MyPlanetStorageProvider.cs)
- Shaders: [`TriplanarSampling.hlsli`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage.Render11/Shaders/Geometry/TriplanarSampling.hlsli) · [`VertexTransformations.hlsli`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage.Render11/Shaders/VertexTransformations.hlsli) · [`VertexTemplateBase.hlsli`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage.Render11/Shaders/Geometry/VertexTemplateBase.hlsli) · [`Materials/TriplanarMulti/Pixel.hlsl`](https://github.com/KeenSoftwareHouse/SpaceEngineers/blob/master/Sources/VRage.Render11/Shaders/Geometry/Materials/TriplanarMulti/Pixel.hlsl)
- **Not published:** `VRage.Native`, which holds the dual contouring inner loop.

**Papers and chapters**

- [GPU Gems 3, ch. 1 — Ryan Geiss, *Generating Complex Procedural Terrains Using the GPU*](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu)
- [GPU Gems 2, ch. 2 — Asirvatham & Hoppe, *Terrain Rendering Using GPU-Based Geometry Clipmaps*](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry) · [Hoppe's own page and PDF](https://hhoppe.com/proj/gpugcm/) · [the original geometry clipmaps paper](https://hhoppe.com/proj/geomclipmap/)
- [Ju, Losasso, Schaefer & Warren — *Dual Contouring of Hermite Data*](https://people.engr.tamu.edu/schaefer/research/dualcontour.pdf)
- [Eric Lengyel — *The Transvoxel Algorithm*](https://transvoxel.org/) and [the thesis PDF](https://transvoxel.org/Lengyel-VoxelTerrain.pdf)
- [Mikola Lysenko — *Meshing in a Minecraft Game*, parts 1 and 2](https://0fps.net/2012/06/30/meshing-in-a-minecraft-game/) — the reference on cubic meshing and why greedy is not free

**Keen**

- [Jan Hloušek — VRAGE3 Engine Update](https://blog.marekrosa.org/2023/04/guest-post-jan-hlousek-vrage3/) — metaball/SDF voxel destruction, GPU-driven pipeline, light and shadow work
- [Marek's Dev Diary, August 21 2025](https://blog.marekrosa.org/2025/08/mareks-dev-diary-august-21-2025/) — the heightmap resolution figures, overhangs, caves, material transitions
- [SE2 VS2 — Planets & Survival Foundations](https://support.keenswh.com/spaceengineers2/pc/announcement/space-engineers-2-alpha-vs-2-planets-survival-foundations)
- [Super-large worlds, procedural asteroids and exploration (2014)](https://blog.marekrosa.org/2014/12/space-engineers-super-large-worlds_17/) and [Planets, oxygen, DirectX 11 (2015)](https://blog.marekrosa.org/2015/02/space-engineers-planets-oxygen-directx_18/) — the LOD rework and dithered transitions

**Community**

- [VoxelMaterial Definition](https://spaceengineers.wiki.gg/wiki/Modding/Reference/SBC/VoxelMaterial_Definition) — the Near/Far1/Far2/Far3 tiers, scales and distances, the 128-material cap
- [Modding: Creating a Planet](https://spaceengineers.wiki.gg/wiki/Modding/Tutorials/Creating_a_Planet) — 2048² cube faces, 16-bit elevation, the raycast ceiling
- [Voxel Technology — Dual Universe wiki](https://dualuniverse.fandom.com/wiki/Voxel_Technology) and [PC Gamer's dev-diary write-up](https://www.pcgamer.com/dual-universe-dev-diary-shows-off-the-voxel-technology-that-powers-the-game/)
- [Continuous World Generation in 'No Man's Sky' — GDC 2017](https://www.gdcvault.com/play/1024265/Continuous_World_Generation_in__No_Man_s_Sky_)
- [Voxagon Blog — Dennis Gustafsson on Teardown](https://blog.voxagon.se/)
