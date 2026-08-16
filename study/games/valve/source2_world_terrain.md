# Source 2's world and terrain — what replaced BSP, and why there is no terrain system

Working notes on how Source 2 represents a level, read against Unreal's
Landscape. Written because two questions kept coming back — "does Source 2 still
use BSP?" and "is its terrain just a mesh?" — and both have the same answer for
the same reason, which is worth writing down once.

Companion to [`source2_rendering.md`](source2_rendering.md), which covers how
Source 2 *lights* a scene. This one covers what it lights.

**[VDC]** is Valve's own documentation on the Valve Developer Community wiki.
**[VRF]** is ValveResourceFormat — the reverse-engineered reader for Source 2's
compiled resources; accurate in structure and field naming but not Valve's
source. **[EPIC]** is Epic's Unreal Engine documentation. **[COMMUNITY]** is
widely-repeated but not first-party. **[inferred]** is our reading.

> **Status, 2026-08-16.** No frame capture behind this. Neither CS2 nor Dota 2
> is actually installed on this machine — both Steam folders are leftover config
> stubs with no game content — so nothing here was checked against a live VPK.
> The evidence is Valve's shipped level-design documentation, VRF's decoder, and
> one decompiled `world.vwrld_c` dump committed to VRF's test corpus. Where that
> runs out, it says so.

---

## 1. The short answer to both questions

**BSP is gone, and there is no terrain system.** Both fall out of a single
decision Valve made when they rewrote the tools: *the compiler no longer gets to
constrain what the level is made of.*

Source 1's BSP demanded convex, sealed, plane-bounded brushes, because
everything downstream — the visibility flood fill, the collision hulls, the
lightmap surface subdivision, the render sort — was built on the split planes.
Displacements existed *because* of that: a heightfield-on-a-brush-face was the
only sanctioned way to get a non-planar surface past a compiler that could not
represent one.

Source 2 removes the constraint at the source, and then every system that leant
on it has to be rebuilt to work on arbitrary triangles. That rebuild is this
document.

| Job BSP did in Source 1 | Source 2's answer |
|---|---|
| authoring primitive | editable meshes — concave and non-planar faces legal **[VDC]** |
| spatial partition | a bounded, parented **world node** tree, `.vwnod_c` **[VRF]** |
| PVS | a **voxelised** inside/outside vis bake over cluster volumes, `.vvis_c` **[VDC][VRF]** |
| render sort | z-buffer; nodes carry draw-distance and aggregate/atlas params **[VRF]** |
| collision | compiled physics meshes and hulls, `.vphys_c` **[VRF]** |
| lightmap surfaces | per-mesh lightmap UVs (see `source2_rendering.md`) |
| non-planar surfaces (displacements) | ordinary mesh geometry |

## 2. Authoring: brushes are not the building block any more

Valve says this outright in the Hammer documentation, and it is the root fact:

> Hammer's geometry tools have undergone a significant overhaul. The usage of
> brushes is no longer the primary building block to construct levels and
> instead it is all faces, edges, and vertices. This is very similar to how
> modeling software works except Hammer is organized and setup in such a way to
> make geometry construction quick and effective for level designers without the
> limitations of brushes. **[VDC]**

And, from the same page's gallery of legal shapes: *"Faces can be removed."*,
*"Concave meshes are valid."*, *"Non-flat faces are valid and are converted to
triangles when compiled."* **[VDC]**

Every one of those three is a BSP compile error in Source 1. A mesh with a face
removed is not sealed; a concave mesh has no single plane set; a non-flat face
has no plane at all. Hammer 2 accepts all three and triangulates at compile.

**There is no displacement tool in the Source 2 level-design documentation.**
The full page list under `Source 2 Docs/Level Design` runs Basic Construction
(four Mesh Editing pages plus Mesh Texturing), Hotspot Texturing, Lighting, Mesh
Entities, Navigation, Prefabs and Instances, Visibility, and the skybox and
import pages — no displacement, no sculpt, no terrain. The wiki's `Displacement`
article never mentions Source 2 at all. **[VDC]**

That is an argument from absence, so treat it as strong-but-not-proof: the
positive evidence is that Valve documents ordinary mesh editing as the way to
make sloped ground, and documents a vertex-paint blend workflow (§4) as the way
to texture it.

> **Aside, and it is the tell.** Dota 2's Source 1 → Source 2 map importer
> "especially likes to shred decompiled displacements" **[VDC]** — the porting
> guide's words. A format that had displacements as a first-class primitive
> would not shred them; it converts them to mesh geometry and the conversion is
> lossy at the seams.

## 3. The world: nodes, aggregates, and a voxel vis bake

### 3.1 What a compiled map actually is

A Source 2 map compiles to a set of resources in a VPK, not a monolithic `.bsp`.
The relevant types **[VRF]**:

| Extension | Name |
|---|---|
| `vmap` | Map (the source file) |
| `vwrld` | World |
| `vwnod` | World Node |
| `vvis` | World Visibility |
| `vphys` | Physics Collision Mesh |

Compiled forms take the `_c` suffix — `.vwrld_c`, `.vwnod_c`. **[VRF]**

### 3.2 World nodes are a bounded tree, not a plane tree

VRF's test corpus contains a decompiled Dota 2 `world.vwrld_c`. Its structure,
verbatim **[VRF]**:

```kv3
m_builderParams =
{
    m_flMinDrawVolumeSize   = 1024.0
    m_flMinDistToCamera     = 1024.0
    m_flMinAtlasDist        = 1000.0
    m_flMinSimplifiedDist   = 8192.0
    m_nAtlasTextureSizeX    = 2048
    m_nUniqueTextureSizeX   = 1024
    m_vWorldUnitsPerTile    = [ 10000.0, 10000.0, 1000.0 ]
    m_nMaxTexScaleSlots     = 128
}
m_worldNodes =
[
    {
        m_Flags            = 192
        m_nParent          = -1
        m_vOrigin          = [ 0.0, 0.0, 0.0 ]
        m_vMinBounds       = [ -8368.4375, -8256.0, -3711.965332 ]
        m_vMaxBounds       = [ 9216.0, 8323.829102, 3584.0 ]
        m_flMinimumDistance = -1.0
        m_ChildNodeIndices = [ ]
        m_worldNodePrefix  = resource:"maps\\dota\\worldnodes\\node000"
    },
]
m_entityLumps = [ resource_name:"maps/dota/entities/default_ents.vents" ]
```

Read what is there:

- **`m_nParent` + `m_ChildNodeIndices`** — an explicit node hierarchy, with
  min/max **bounds** per node. A bounding-volume tree over the map, built by
  tiling the world (`m_vWorldUnitsPerTile`), not by choosing split planes.
- **`m_flMinimumDistance`** — per-node draw distance. Culling is bounds and
  distance, not tree traversal order.
- **`m_flMinSimplifiedDist`, `m_flMinAtlasDist`, `m_nAtlasTextureSizeX`** — the
  world builder does distance-keyed geometry simplification and texture atlasing
  at compile time. This is the machinery behind Source 2's aggregate static
  geometry, and it is a *build* step, not a runtime one.
- **`m_entityLumps`** — entities live in separate `.vents` resources. "Lump" is
  the only BSP word that survived, and it survived as a name only.

Inside a node **[VRF]**, `m_sceneObjects` holds placements — each with a
transform, tint, layer index, a baked `m_nLODOverride`, and precomputed
handshakes tying it to a cubemap and a light-probe volume — and
`m_aggregateSceneObjects` holds the merged batches with their fragments. Nothing
in there is a face, an edge or a plane. **A world node is a container of placed
renderables with bounds**, which is what a modern renderer's scene graph looks
like, and is not what a BSP leaf looks like.

### 3.3 Visibility: still a PVS, computed by voxelising

Source 2 kept precomputed visibility. It did not keep the way it was computed.
Valve's Visibility page **[VDC]**:

> The vis solution in Source 2 works on an inside/outside algorithm and tries to
> figure out what's "inside" the map when doing visibility calculations.
> Effectively anything inside will render and anything outside will be dropped.

> This is done to compensate for **not having any constraints on how levels are
> constructed**. Levels will often include geometry that isn't closed or have
> geometry that overlaps in some way. These cases should be handled without
> forcing the system to store visibility info below floors or terrain just
> because some valid geometry is poking through those surfaces.

That paragraph is the whole thesis of this document in Valve's own words. The
new vis exists *because* the new authoring tools stopped guaranteeing sealed
convex geometry.

The method is voxelisation, stated in passing while explaining a tool texture:

> When voxelizing the world, voxels get subdivided when they contain geometry.
> If the only geometry in a voxel is toolsskybox, the voxelization will stop at
> a more coarse resolution. This improves performance of the visibility
> compiler. **[VDC]**

The output is **clusters** — volumes of space, each storing the set of other
clusters visible from it — plus a separate lines-of-sight file. From the Hammer
side **[VDC]**: *"Each Vis Cluster represents a volume of space for which the
visibility has been computed. The visibility is defined as the set of other
clusters which can be seen from that cluster."* Loading the compiled data draws
the selected cluster yellow and everything it can see blue; with the `.los` file
present, the actual sightlines between two clusters draw as green lines.

Two details worth having:

- **`vis_debug_tracelos`** traces a grid of rays over the current screen, adds
  any newly-found sightlines to the solution, and **records them in `los.bin`
  for future map compiles** **[VDC]**. The vis solve is *accumulative across
  compiles* and hand-correctable. That is a very different contract from a
  deterministic BSP portal flood fill, and it implies the voxel solve is
  conservative-but-incomplete in a way the level designer is expected to patch.
- **The whole thing is optional** — Map Properties → Precomputed Visibility →
  Disabled **[VDC]**. A Source 1 map without vis is a functional impossibility;
  a Source 2 map without vis just draws more.

The rest of the page is a list of ways to get the inside/outside classifier
wrong — holes in the shell, outward-facing boundary geometry, vis contributors
poking through the level bounds — which is a fair summary of what you trade for
dropping the sealed-brush requirement: the compiler no longer *enforces* the
seal, so the failure moves from a compile error to a silent performance
regression.

### 3.4 Collision

Collision compiles to `.vphys_c`, "Physics Collision Mesh" **[VRF]** —
triangle meshes and convex hulls generated from the world geometry, consumed by
Source 2's physics engine (called Rubikon **[COMMUNITY]**; the name does not
appear in the VDC documentation). There is no plane-set-per-brush collision
representation to inherit, because there are no brushes.

## 4. Terrain: a mesh, plus a vertex-painted blend material

There is no terrain system. There is a **terrain workflow**, and it is two
things: mesh geometry, and a blend material driven by per-vertex data.

### 4.1 The blend material

From the `VR Standard` shader reference **[VDC]**:

> **Blend** — Adds secondary layers to the material. The layers can be painted
> in Hammer *Terrain Blending* tool or added to a DMX mesh using the
> `VertexPaintBlendParams` vertex attribute.
> Choices: *None* / *2-Layer* / *3-Layer*

So: **three layers maximum** on `VR Standard`, and the blend weight lives in a
**vertex attribute**, not a splat texture. Per non-base layer the shader adds a
*Reveal mask* texture ("Modulates at what value the layer becomes visible.
Equivalent to the Source 1 `$blendmodulatetexture`"), *Blend softness*, and a
border colour/offset/softness/strength group for edge staining **[VDC]** — a
height-blend on top of the vertex weight, which is what stops vertex-resolution
blending from looking like a linear crossfade.

Dota's painting tool is documented in more detail, and is a straight vertex
paint brush **[VDC]**: radius and strength, *Paint On* scoping (everything /
selected objects / selected faces), and two modes — *Blend* ("change the
contribution of the selected texture channel at the vertex level") and *Color*
("color tinting applied at the vertex level"). It exposes **four** channels,
one more than `VR Standard`'s three.

The consequence is the one that matters: **blend resolution is mesh resolution.**
A dense mesh can hold a detailed transition; a sparse one cannot, and the only
fix is more triangles. Unreal decouples these — weightmaps are textures with
their own resolution — which is why a UE landscape can carry a crisp path across
a coarse mesh and a Source 2 one cannot.

### 4.2 The one heightfield in Source 2 is Dota's, and it is an editor

Dota 2's Hammer has a **Tile Editor** with a *Height Brush* — and it is a
genuine quantised heightfield **[VDC]**:

> The tile editor will allow the height of the ground to be raised **15 steps**
> above or below the initial ground plane height. […] Each height step of the
> ground is equivalent to **128 game units**.

It has *Raise Height* versus *Add Height* (plateaus versus hills), tile-set
assignment under the brush, a water brush that guarantees water is ringed by
ground exactly one step higher, a path brush that edits *tile edges* rather than
corners and inserts ramps, and tree placement locked to a 64-unit grid because
trees affect navigation and fog of war.

But it is an **authoring** structure, not a runtime one. Tiles **collapse**:
*"Collapsing items creates an editable copy of the selected items which can be
modified individually outside of the tile editor"* **[VDC]**, and the blend
brush exists inside the tile editor specifically so you can paint blends
*"without having to first collapse the tiles"* **[VDC]**. The tile grid is a
generator that emits meshes; by the time the engine sees it, it is meshes.

**[inferred]** This is the closest thing in shipped Valve tooling to what this
project's world already is — a discrete-height lattice with per-cell material
assignment and gameplay rules (navigation, vision) enforced by the editor at
placement time. Worth reading the Tile Editor page in full before designing any
in-house level tooling; the water-must-be-one-step-below and
path-modifies-edges-not-corners rules are the kind of constraint you only invent
after shipping without it.

### 4.3 What the terrain shaders actually do

There are four blend shaders in the family, and VRF's name mapping is the way to
tell them apart **[VRF]** — `vr_standard.vfx` (Alyx / SteamVR Home, §4.1),
`multiblend.vfx` (Dota), `csgo_environment.vfx` and `csgo_environment_blend.vfx`
(CS2, sharing one implementation), and `environment_blend.vfx`, which VRF maps
separately and which is the most capable of the four. It sits next to
`citadel_overlay.vfx` in the same table, so Deadlock is the likely home
**[inferred]** — not confirmed.

Read from VRF's reimplementations, which are decompiled from Valve's compiled
shaders and so are accurate on feature flags and parameter names **[VRF]**:

**Height-driven blending, not height-driven geometry.** `csgo_environment`
carries a height map per layer — `g_tHeight1..3`, with `g_flHeightMapScale` and
`g_flHeightMapZeroPoint` — and spends them entirely on deciding *which layer
wins per pixel*. The newer path (`F_USE_NEW_BLENDING`) is a height-band blend
where each layer's weight comes from a height difference measured against the
layers below it, **carried upward** through the stack, with
`g_flUnderlyingHeightMapInfluence` and `g_flMaskWithHeight` per layer. This is
what stops gravel-over-concrete looking like a crossfade — the gravel appears in
the concrete's low spots first.

**Reveal masks and decorated seams.** `environment_blend` gives each layer above
the first a reveal mask with offset, softness, invert and its own UV transform,
then adds a **border effect** around the resulting edge: width and softness on
each side, a tint with three blend modes (`Multiply` / `Mod2x` / `Blend`), and a
separate roughness override at the seam **[VRF]**. Blend edges are treated as
something to *author*, not merely to soften — which is a reasonable response to
blend weights that live at vertex resolution (§4.1).

**Two overlay systems on top of the layers.** A *UV overlay* — one colour and
normal-roughness texture laid across every layer with its own reveal — and a
*world overlay*, up to two textures projected in world or object space and
modulated over everything, masked per layer **[VRF]**. Both exist to break up
tiling across large surfaces, which is the thing the layer stack alone cannot do.

**Detail normals** on their own UV set (`F_DETAIL_NORMAL`, `vDetailTexCoords`),
and per-layer contrast/brightness pairs for roughness, metalness and tint mask,
plus self-illumination scale and tint **[VRF]**. Dota's `multiblend` is the
simple end of the family: four layers, optional normal map, and
`F_WORLDSPACE_UVS` — world-space planar UVs, which is a projection, not a
triplanar blend.

### 4.4 Triplanar, parallax, tessellation — the specific answers

**Triplanar: no. Biplanar, in one shader, and cheapened further.**
`environment_blend` has `F_LAYER1_BIPLANAR` / `F_LAYER2_BIPLANAR`, with a single
`g_flBiPlanarTiling` (default 196) replacing the layer's UV transform **[VRF]**.
Biplanar is Inigo Quilez's two-sample alternative to triplanar's three. Valve
then cut it further for the layers — VRF's comment on the reimplementation:

> Layers only ever sample the dominant plane. Valve swaps in the median plane's
> derivatives when that plane wins outright but keeps the dominant plane's
> coordinate, so there is only ever one set of texels to fetch. **[VRF]**

So a "biplanar" layer costs **one** fetch, not two or three; the second plane
exists only to keep the derivatives — and therefore the mip selection and
anisotropy — sane on a steep face. The genuine two-plane blend is reserved for
the *world overlay*, where `SampleBiPlanar` blends the major and median planes
with weights that only start mixing once a plane is more than ~35° off its axis
(`saturate((absNormal - 0.5773) * 2.3657)`, i.e. cosine 1/√3) **[VRF]**.

That is a very deliberate cost curve: full projection blending on the cheap
modulation pass, single-fetch projection on the expensive layered pass.

**Parallax / POM: not on terrain.** No parallax term appears in any of the four
blend shaders — the height maps go to blending (§4.3), and `VR Standard`'s only
use of the word is *"Specular cube map projection… enables parallax corrected
cubemaps"* **[VDC]**, which is a reflection fix, not a surface one.

CS2 *does* ship a parallax shader, and its scope is the point. VDC on
**`Csgo Simple 3layer Parallax`**:

> a shader in Counter-Strike 2 used to give simple fake depth to flat surfaces.
> This shader is used on **most windows on de_Inferno and de_Italy**. **[VDC]**

Three colour layers, each with a mask and an *offset*, optionally on a secondary
UV set **[VDC]** — a layered interior-offset trick for windows, not a
height-marched POM, and not applied to ground. Compare
[`../../topics/surfaces/surface_depth.md`](../../topics/surfaces/surface_depth.md),
which works through the real POM ladder in Unreal: Source 2 sits at the bottom
rung of it, deliberately.

**Tessellation: no evidence of any.** No tessellation feature in any documented
Source 2 shader, nothing in the material editor's feature lists **[VDC]**, and a
VDC search returns nothing. **[inferred]** This is the expected answer rather
than a surprising one: §2 established there is no displacement pipeline to
tessellate *toward*. Tessellation is only worth wiring up when a height field or
displacement map is the authoritative surface and triangles are generated from
it — which is Unreal's position (§5.2, Nanite tessellation), and precisely not
Valve's.

**The shape of it.** Source 2's terrain shading is sophisticated in exactly one
direction — **combining and de-tiling layers** — and absent in the other, *faking
or generating depth*. Height maps, reveal masks, borders, detail normals and two
overlay projections all serve the first; nothing serves the second. Which is
consistent: the geometry is already real triangles an artist placed, so the
shader's job is to make a small texture set cover a large surface without
repeating, not to pretend the surface is bumpier than it is.

## 5. Unreal's Landscape, for contrast

Landscape is the opposite bet: constrain the representation hard, and get an
entire system for free.

- **Components are the unit.** *"Landscapes are divided into multiple
  Components, which are Unreal's base unit of rendering, visibility calculation,
  and collision."* **[EPIC]** Epic recommends a maximum of 1024 of them for the
  largest landscapes, and *"each section is a draw call"* **[EPIC]**.
- **Height lives in a texture.** *"Each component's height data is stored in a
  single texture. Because of this, its size has to be a power-of-two number of
  vertices."* **[EPIC]** Heights are *"stored with 16-bit precision"*, mapped to
  a −256…255.992 range and multiplied by the import Z scale **[EPIC]**.
- **LOD is the mipmap.** Sections must be power-of-two vertices *"so that the
  different LOD levels can be stored in the mipmaps of the texture"* **[EPIC]**,
  and transitions morph: mips for both LODs are sampled and heights and XY
  offsets interpolated in the vertex shader **[EPIC]**. That is the payoff of
  height-as-texture — LOD costs a mip bias, not a second mesh.
- **Layers are weightmap textures**, independent of mesh density, blended by the
  landscape material.

> **Caveat.** UE4-era documentation additionally described the packing as height
> in the R+G channels and normal XY in B+A of a 32-bit texture. The current
> pages no longer state this, and it was not verified here — treat the exact
> channel layout as unconfirmed. The 16-bit height and the per-component texture
> *are* confirmed in current docs. **[EPIC]**

### 5.1 "Infinite" — worth being precise about

Landscape is not infinite. It is a bounded grid of components, sized at creation
(the docs work through the arithmetic: 32×32 components of 4 subsections × 64×64
vertices gives a 4033×4033-vertex landscape **[EPIC]**). What makes large worlds
practical is two orthogonal things — **World Partition** streaming cells and
HLOD, so the bound need not be resident, and UE5's **Large World Coordinates**,
which removes the float-precision ceiling that used to make ~20 km the practical
limit. Neither is a property of the heightfield.

### 5.2 Nanite Landscape does not replace the heightfield — it duplicates it

As of UE 5.6, Nanite can be enabled on a Landscape actor at parity with normal
Landscape rendering **[COMMUNITY]**, and it is documented as a checkbox plus a
build step **[EPIC]**. The interesting part is the cost, in Epic's words:

> Nanite Landscapes stream on top of the existing Landscape data streaming since
> both Nanite and non-Nanite data are necessary at runtime. Non-Nanite Landscape
> data is required for Runtime Virtual Textures, water rendering, and more. This
> means twice the amount of data is streamed. One set of data for Nanite
> streaming and another set for texture streaming. Enabling Nanite causes both
> sets of data to reside in memory. **[EPIC]**

**[inferred]** That is the cleanest possible statement of why these two engines
are not converging. Even when Unreal renders its terrain as a Nanite mesh, the
heightfield stays — because collision, RVT and water are all *queries*, and a
query wants `height(x, y)`, not a BVH. Nanite Landscape is a rendering strategy
laid on top of the representation; it is not a replacement for it. Source 2
never had the representation, so it has nothing to lay anything on.

(Nanite Landscape brings its own seam problem, which is a nice illustration of
what you give up: because Nanite decimates rather than using a regular grid,
neighbouring proxies do not share vertices at the border, and Epic's fix is a
literal skirt — an extra row of vertices pushed down by a configurable depth
**[EPIC]**. Heightfield LOD morphing never has this failure mode, because both
sides are sampling the same texture.)

## 6. Head to head

| | UE Landscape | Source 2 |
|---|---|---|
| Representation | 16-bit heightfield, one texture per component **[EPIC]** | arbitrary triangle mesh **[VDC]** |
| Geometry source | shared grid, position sampled from heightmap in the VS **[EPIC]** | real vertices in a vertex buffer |
| LOD | continuous, per-section, morphed between heightmap mips **[EPIC]** | discrete mesh LODs; compile-time simplification past `m_flMinSimplifiedDist` **[VRF]** |
| Layer blending | weightmap textures, resolution-independent **[EPIC]** | per-vertex weights, 3–4 channels **[VDC]** |
| Collision | component-level heightfield **[EPIC]** | triangle mesh / hulls in `.vphys_c` **[VRF]** |
| Editing | sculpt, erosion, splines, non-destructive edit layers | move vertices; Dota's tile editor is the exception **[VDC]** |
| Streaming | World Partition cells + HLOD | world node tree with draw distances **[VRF]** |
| Overhangs, caves, arches | impossible — needs separate meshes | free |
| Terrain-specific engine code | a large system | none |

The trade is legible in one line: **a heightfield buys cheap LOD, cheap
collision, cheap streaming and procedural authoring, all of which fall out of
"height is a function of XY" — and charges for it with that same restriction.**
Valve can pay that price because a CS2 map is a hand-built box measured in
hundreds of metres and full of interiors, overhangs and multi-storey geometry
that a heightfield could not express anyway. Epic cannot, because its customers
ship open worlds where terrain is most of what exists.

## 7. What this means for this project

**[inferred]**, and the reason the comparison was worth writing down.

This project's world is neither of these, and that is a good position rather than
an awkward one. A tile lattice **is already a heightfield with overhangs**:
`(x, y, z) → index` is arithmetic, exactly as `height(x, y)` is, so the cheap
queries Unreal buys with its restriction are already ours *without* the
restriction. `OcclusionGrid`, `ReachField` and the rest are the terrain-query
half of Landscape, arrived at from the other direction.

Three things follow.

1. **Neither engine's LOD strategy transfers.** Unreal's morph needs height in a
   texture; Source 2's mesh LODs need an artist-authored chain. A voxel/tile
   world's LOD problem is a third thing — merging runs of identical cells — and
   the answer will come from measurement, not from either of these.
2. **Vertex-resolution blending is the trap to avoid.** Source 2's material
   blending is bounded by mesh density, which is fine for hand-built maps and
   wrong for a lattice whose surface tessellation is fixed by cell size. If
   surface variety is wanted, it belongs in the material as a function of world
   position or cell state — not in painted vertex attributes we would then have
   to store per cell. That is the same conclusion the `.mat` material work
   reached from the material side; this is it arriving from the geometry side.
3. **Source 2's vis is the more relevant of the two visibility stories**, but
   only as a warning. It is a *bake* over static geometry, and it degrades to
   "draws more" when disabled. A destructible lattice cannot bake, so the
   comparison to draw is against the failure mode, not the technique: what
   Source 2 gets from the bake is exactly what a runtime occlusion query has to
   earn every frame.

The Dota Tile Editor (§4.2) is the one piece of shipped Valve work that is
directly applicable, and it is applicable to the *editor*, not the renderer.

## 8. Sources

Valve documentation **[VDC]** — behind an Anubis proof-of-work challenge;
`WebFetch` returns 307. Solve it in Python and fetch `?action=raw` (see the
`fetching-documents-verbatim` note):

- [Source 2 Docs/Level Design/Visibility](https://developer.valvesoftware.com/wiki/Source_2_Docs/Level_Design/Visibility)
- [Source 2 Docs/Level Design/Visibility/Load Compiled Vis Data](https://developer.valvesoftware.com/wiki/Source_2_Docs/Level_Design/Visibility/Load_Compiled_Vis_Data)
- [Source 2 Docs/Level Design/Basic Construction/Mesh Editing 1](https://developer.valvesoftware.com/wiki/Source_2_Docs/Level_Design/Basic_Construction/Mesh_Editing_1)
- [Source 2 Docs/Porting Legacy Content/Maps](https://developer.valvesoftware.com/wiki/Source_2_Docs/Porting_Legacy_Content/Maps)
- [VR Standard (Source 2 Shader)](https://developer.valvesoftware.com/wiki/VR_Standard_(Source_2_Shader))
- [Dota 2 Workshop Tools/Level Design/Terrain Blending](https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Terrain_Blending)
- [Dota 2 Workshop Tools/Level Design/Tile Editor](https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Tile_Editor)

ValveResourceFormat **[VRF]**:

- [Resource Types](https://github.com/ValveResourceFormat/ValveResourceFormat/wiki/Resource-Types)
- [`Renderer/Renderer/World/WorldNodeLoader.cs`](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/master/Renderer/Renderer/World/WorldNodeLoader.cs)
- [`Tests/Files/ValidOutput/world.vwrld_c/DATA.txt`](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/master/Tests/Files/ValidOutput/world.vwrld_c/DATA.txt) — the decompiled Dota world quoted in §3.2

Epic **[EPIC]**:

- [Landscape Technical Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-technical-guide-in-unreal-engine)
- [Landscape Technical Guide (4.27)](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-technical-guide?application_version=4.27) — the component/section/LOD text quoted above
- [Using Nanite with Landscapes](https://dev.epicgames.com/documentation/unreal-engine/using-nanite-with-landscapes-in-unreal-engine)

## 9. What would settle the open questions

Install CS2 and open its VPKs in Source 2 Viewer. Three things are unverified
here and all three are one capture away:

1. Whether CS2's Hammer has gained any heightmap import or sculpt convenience on
   top of plain mesh editing since the Alyx-era docs were written — the
   Visibility page still carries the *"originally written for Half-Life: Alyx"*
   note **[VDC]**, so the level-design docs may simply be stale.
2. Which game `environment_blend.vfx` belongs to (§4.3) — the biplanar and
   border machinery is the most advanced in the family and it would be useful to
   know whether CS2 has it or only Deadlock does. VRF maps it separately from
   `csgo_environment_blend.vfx`, so they are not the same shader.
3. What the world node tree looks like on a large modern map — the Dota dump in
   §3.2 has a **single** root node with no children, so it proves the fields
   exist but says nothing about how deep the hierarchy actually goes in practice.
