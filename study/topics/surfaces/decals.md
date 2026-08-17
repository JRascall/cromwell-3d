# Decals: projected, or clipped to geometry

Whether to build a **mesh decal system** — clip the decal against the receiving
triangles at placement and store the resulting geometry, the way Source does —
or keep the **projected DBuffer** pass cromwell already has.

Asked because the game wants scorch marks and damage decals, and the wrap over
folded geometry is not perfect. Written after a session of fixing the projected
path, with the two engines' actual implementations read from the copies on this
machine rather than recalled.

**The short answer is no, and the reason is that a clipper does not buy the
thing it appears to buy.** Clipping and wrapping are different properties. A
clipper gives exact clipping and *no* wrapping — it is a single planar
projection per decal, chopped to triangle boundaries. Everything in this note
follows from that one fact.

---

## 1. The two architectures

**Projected (what we have).** A box is rasterised, the depth buffer is
unprojected inside it to recover whatever surface is really there, and the
decal's material is written into three planes that the lit pass blends over its
own inputs. Nothing is clipped, nothing is stored, and the receiver is whatever
the camera can see this frame.

**Clipped.** At placement, the decal's quad is projected onto the receiving
surfaces, clipped against each triangle, and the resulting vertices are kept in
a per-decal buffer. Drawing is ordinary geometry with a depth bias.

The interesting differences are not the ones the names suggest.

| | projected | clipped |
|---|---|---|
| lands on the surface actually there | yes, per frame | yes, at placement |
| follows geometry that later changes | **yes, free** | **no — must be rebuilt** |
| cost per decal at rest | one draw | one draw |
| cost per decal at placement | none | a clip against N triangles |
| lit correctly | receiver's own lighting, once | needs its own shading or a second pass |
| bleeds onto surfaces it should not | **the whole problem** | cannot, by construction |
| wraps a mark round a fold | approximately, one fold | **no** |

## 2. What Source actually does

`[SOURCE]` Read from `source-sdk-2013-master` on this machine. The engine's
world-surface path is **not** in the public SDK — `src/engine` contains only
`audio`, and `public/iefx.h` is an interface — so what follows is the client
half plus the model path, which is enough to settle the architecture.

A decal is aimed by a **trace** and addressed to **the entity that was hit**:

```cpp
// game/shared/util_shared.cpp
void UTIL_DecalTrace( trace_t *pTrace, char const *decalName )
{
    if (pTrace->fraction == 1.0) return;
    CBaseEntity *pEntity = pTrace->m_pEnt;
    pEntity->DecalTrace( pTrace, decalName );
}
```

and applied by a **planar projection along that ray** onto that one model's
triangles:

```cpp
// public/istudiorender.h — comment is Valve's
// Add decals to a decal list by doing a planar projection along the ray
virtual void AddDecal( StudioDecalHandle_t handle, studiohdr_t *pStudioHdr,
    matrix3x4_t *pBoneToWorld, const Ray_t & ray, const Vector& decalUp,
    IMaterial* pDecalMaterial, float radius, int body,
    bool noPokethru = false, int maxLODToDecal = ADDDECAL_TO_ALL_LODS ) = 0;
```

**One ray, one up vector, one radius — therefore one basis for the whole
decal.** That is the decisive detail. A single planar basis cannot bend around
a corner; it can only be clipped to the triangles it covers. Whatever the BSP
path does to *find* candidate faces, the projection it applies is this one, and
a face at ninety degrees to it receives a smear or nothing.

Valve's own name for ink arriving where it should not is in the API —
`noPokethru` — and where they genuinely do project, the cure is an extra plane:

```cpp
// public/engine/ishadowmgr.h
// Set extra clip planes related to shadows...
// These are used to prevent pokethru and back-casting
virtual void AddExtraClipPlane( ShadowHandle_t shadow, const Vector& normal, float dist ) = 0;
```

`C_BaseEntity::AddStudioDecal` goes further and rebuilds the ray from the
trace's plane normal *specifically* to avoid it, passing `noPokethru = true`
when it has an accurate normal to do it with — so even inside a clipper, with
full geometry in hand, Valve are still fighting the same artefact.

## 3. What Unreal actually does

`[EPIC]` Read from UE 5.7's shaders, `Engine/Shaders/Private`, on this machine.

A deferred decal's entire containment test is the box, and nothing else:

```hlsl
// DeferredDecal.usf
// clip content outside the decal
clip(OSPosition.xyz + 1.0f);
clip(1.0f - OSPosition.xyz);
```

There is **no solidity test, no receiver-normal rejection, and no wrap** — the
same position cromwell's pass was in before this session. A UE decal projected
onto a corner puts ink on the far side of the wall exactly as ours did; what
keeps it acceptable in practice is authored angle fades in the material and
artists keeping projector boxes shallow.

Epic *do* ship a second path, `MeshDecals.usf`, and it is worth being precise
about what it is: it draws **ordinary mesh geometry** through the decal pass,
with a vertex factory. The geometry is **authored** — an artist models the grime
strip or panel line and fits it to the kit — not clipped from a receiver at
runtime. So Unreal has no runtime clipper either. Two paths, neither of them
the system in question.

## 4. Wrapping is not clipping, and only one of them is on offer

The reason our stairs look the way they do is that the unwrap carries the
texture by its distance from the **placement plane**. That is exactly right for
one fold and meaningless for eight, because each riser sits a different height
above that plane and its UV is displaced by that height until it leaves the
texture.

Doing it properly means measuring distance **along the surface** — geodesic,
over connected geometry. Neither engine above does this. A clipper does not do
it either: it would clip the same single planar projection onto the treads and
the risers, and the risers would take the stretched, edge-on version that the
angle fade exists to remove.

**So a mesh decal system would not fix the case that prompted the question.**
It would fix bleed — which is the part we have now fixed another way.

## 5. What the projected path costs to make honest, and where it now is

Three defects, in the order they were found, because the order is instructive —
each was only findable once the previous one stopped masking it.

1. **The projector cube had four of six faces wound backwards.** With
   `cull = Front` the correctly wound faces contributed their far side and the
   reversed ones their near side; either set alone covers the silhouette of a
   convex solid, a mixture covers neither, and the decal was absent in wedges
   that moved with the camera. Not a decal bug at all — a bounding-volume bug.
2. **Ink on surfaces facing away from the placement point.** Cured by Valve's
   clip plane, chosen per pixel from the receiver rather than fixed per
   projector: the signed distance of the decal's centre from the plane the
   receiver lies in must be positive. One dot product. This is the test that
   makes a corner behave.
3. **Wraps reaching through the floor into the storey below.** A surface behind
   the placement plane can legitimately face the decal — it is on the other side
   of something solid, which the pass cannot see. Capped at 2 cm, under the 6 cm
   of a floor slab and the 9 cm of a wall. Free, because after (2) the only ink
   left behind the plane is on the far side of solid.

Plus one that was not in the renderer at all: **the trace tested wall slabs that
straddle a cell boundary from only one of the two cells**, so a ray could cross
the half in the non-owning cell and leave without the wall ever being tested.
The cursor slid off plain walls onto the ground behind them at particular
angles, and the decal inherited the ground's normal. Every trace through
`WorldTrace` had it.

## 5.1 And then the third defect turned out to be unfixable by any formula

The plane test in (2) has a cost stated at the time: a fold that turns *away*
from the decal — a stair riser, a kerb edge, the outside corner of a building —
is refused, because its shape is identical to a wall seen from the wrong side.

**That is not a tuning problem and no threshold reaches it.** A riser and the
back of a wall present the same normal at the same angle at the same distance.
The only difference between them is whether anything is standing in the way, and
no function of a normal and a position can see that. Two tests were shipped and
reverted proving it.

So the pass was given the missing knowledge directly: **when a decal is placed,
the world around it is rendered from its own position into a small cube of
distances**, and a surface is inked only if it is no further away than what that
capture saw in its direction. The far side of a wall is not — the wall is in the
way, at a shorter distance, in that exact direction. A stair riser is. So is a
crate, a beam, a kerb and a rubble pile, at any orientation, with the pass
knowing what none of them are.

This is Source's rule — the surfaces reachable from the impact point — reached
with a rasteriser instead of a triangle list. It costs six 32-pixel faces per
decal, rendered **once at placement** rather than per frame, because a mark that
has settled is looking at geometry that is not moving.

**What it does not do yet**: notice geometry changing under a settled decal.
That wants a scene geometry version to compare against, which belongs with
whatever owns destruction.

**Note what this does NOT change**: the wrap over many folds. A staircase is
still parameterised from the placement plane, so the ink on each riser is
correct where it lands and the *unwrap* still runs out of texture. Visibility
and parameterisation are separate problems, and only the first one is solved.

## 6. What a clipper would cost here specifically

**Invalidation, and this is the one that decides it.** This game demolishes
geometry. A projected decal re-lands on whatever is there next frame, for free —
blow out the wall under a scorch mark and the mark goes with it, with no code.
Clipped geometry is a cache of a shape that no longer exists: every wall
destroyed, floor removed, door opened or storey cut has to find and rebuild the
decals attached to it. That is a derived cache with an invalidation boundary
running through the destruction system, which is precisely the maintenance
liability CLAUDE.md's derived-cache rules are about — and the fast path here
cannot be "provably does nothing", because the geometry genuinely changed.

**Lighting.** The DBuffer's real advantage is not clipping, it is that the decal
changes what the material *is* and the surface then lights **once**, with its
own shadow, probe and occlusion already in hand. Clipped decal geometry is a
second surface that has to acquire all three again, or be drawn unlit and look
stuck on.

**Placement cost and storage.** A clip against the receiving triangles per
decal, plus a vertex buffer per decal, against a POD projector in an array.

## 7. Recommendation

**Keep the projected DBuffer.** For scorch marks and damage decals on a tile
world of floors and walls — which is nearly all of them — it is now correct,
and it is what shipped games use for exactly these two effects.

Cheaper things that improve the same content, in order of value:

1. **Let placement use the world.** The tile grid knows what is solid; the
   projector's depth is currently `max(1, size)` regardless of what it is stuck
   to. Sizing the box from the geometry it was placed on — never deeper than the
   solid behind it — removes a whole class of reach artefacts at the only place
   that has the information, and costs a grid query in cold code.
2. **Several small decals rather than one large one** for an explosion. Shipped
   games do this anyway for variety, and it also happens to keep every projector
   box small enough that reach never becomes a question.
3. **Per-surface placement where a mark must cross complex geometry** — one
   decal per tread. This is Source's answer, done at authoring time.

## 8. What would change the answer

- **Marks on units or props become important.** A decal on a moving skinned
  mesh cannot be a world-space box; that is what Source's studio path is for,
  and it is a genuinely different problem from marks on a static tile world.
- **A mark must visibly run across many folds** as a designed effect rather than
  an occasional accident — a spray trail down a staircase, a banner over
  scaffolding. That needs geodesic parameterisation, which is more than either
  engine here does and would be a real piece of research, not a port.
- **The decal count grows past what one draw each can carry.** That argues for
  instancing or clustering the projected path, not for clipping.

---

## 9. Geodesic parameterisation: the paper, and whether this world can feed it

§8's second bullet — a mark that must visibly run across many folds — is the one
open case, and this section is the feasibility pass on it. Two questions were
asked before any code was written: what does the paper actually say, and is the
receiving geometry in a shape the algorithm can walk. **The first answer is
better than expected and the second is worse.**

## 9.1 What the paper actually says

`[PAPER]` Schmidt, Grimm & Wyvill, *Interactive Decal Compositing with Discrete
Exponential Maps*, SIGGRAPH 2006 / ACM TOG 25(3). Read from the authors' own PDF
(Grimm's copy at Oregon State), not recalled.

**It is not a triangle unfold, and that is the first correction.** There is no
walk over triangles across shared edges and no unfolding of one triangle into
another's plane. The algorithm runs **Dijkstra over surface SAMPLES** — points
with normals — and computes the parameterisation as a by-product of the same
propagation. §4.2's own summary: *"The resulting algorithm requires only a simple
vector addition of the piecewise-linear geodesics produced by Dijkstra's
algorithm."*

**Mesh connectivity is explicitly optional.** *"If mesh connectivity is
unavailable, nearest Euclidean neighbours are assumed to be geodesic
neighbours"* — they use k = 15. Their own system has none: it visualises implicit
surfaces through marching cubes, and *"since our marching cubes implementation
does not generate mesh connectivity data, Euclidean neighbours are used to create
the k-NN lists"*. **So the adjacency question this was braced for is not a
requirement of the algorithm at all.** It is a requirement on *sampling density*,
which is a different and much easier thing to satisfy.

**The frame propagation, which is the part worth getting exactly right.** For a
seed p, every other point q gets 2D normal coordinates `u(p,q)` in p's tangent
plane. Along the Dijkstra path `{p_i}` from p to q,

```
u(p,q) = û(p0,p1) + SUM over i>=1 of  Rot2D(theta(p,p_i)) * û(p_i, p_i+1)     (Eq. 3)
```

Three details in that line, each the difference between the paper's result and a
plausible-looking wrong one:

1. **Every segment is lifted DIRECTLY into T_p — never composed through the
   intermediate tangent planes.** The paper is explicit, and the reason is error:
   *"Note that û(p_i,p_i+1) is mapped directly into the tangent plane at p,
   rather than incrementally mapping to each previous tangent plane.
   Transforming the vector through each previous tangent plane results in much
   higher total error."* Composing frame-to-frame is the obvious implementation
   and it is the wrong one.
2. **`theta(p,r)` is defined by two rotations and only the second survives.**
   Rotate T_r about any vector perpendicular to both n_r and n_p by
   `acos(n_r . n_p)`, which makes it coplanar with T_p and gives a new in-plane
   basis `x'_r`; then rotate about n_p by `acos(x'_r . x_p)` to align the
   in-plane axes. The second is a **2D** rotation in the tangent plane and is
   applied directly to the 2D vector — so the state carried per point is
   `(u, v)` plus one angle, not a matrix.
3. **The local map `û(r,q)` uses the straight-line length as the distance.** Take
   `|r - q|` as the geodesic estimate, find the angle `theta_q` between `rq` and
   T_r, rotate `rq` by `theta_q` about `rq × n_r`. *"The rotation must always be
   in the direction of the normal, hence if n_p . n_q < 0 the rotation angle is
   pi - theta_q."* That sign case is the one that bites on any fold sharper than
   a right angle — which is every stair riser.

**Then the decal proper.** Run Dijkstra to a geodesic radius `r + delta`, delta
being the largest neighbour distance (*"necessary to ensure that the disc of
radius r is contained within the decal, since the particular discretization may
otherwise result in clipping"*). Scale the parameterisation by `1 / (sqrt(2) r)`
and translate by `(0.5, 0.5)`, so the geodesic square inscribed in the disc lands
on `[0,1]²`. Store as a local parameterisation of the covered geometry and
composite by alpha blending.

**Holes are free, and they are the escape hatch.** §5.2: to avoid the distortion
a protrusion causes, *"the unwanted points are removed from the Dijkstra
computation and left unparameterized"* — they halt at points whose absolute
Gaussian curvature exceeds a threshold, and note that other criteria (creases, a
painted boundary) work identically. The mark then passes *around* the feature.
Because the accumulation is a vector sum in 2D rather than a true surface
geodesic, the parameterisation behind the hole is continuous **as if the feature
were not there** — true geodesics would collide behind it and tear.

**Cost is a non-issue at our scale.** O(N log N), one Dijkstra with a priority
queue, computed in-line with the propagation. Their 2006 numbers on a 1.6 GHz
laptop: whole-decal parameterisations at 121 and 362 fps.

**And the property that makes this the right algorithm for THIS world rather
than merely a famous one:** §4.3 — *"fully developable surfaces (surfaces with
zero Gaussian curvature everywhere) are parameterized with no distortion. Our
discrete approximation reproduces this property"* — demonstrated in Figure 7 on a
swiss roll, a cone and **a box**. This world is axis-aligned boxes. A stair
flight is developable along every fold it presents: treads and risers meet along
parallel horizontal edges, and the map across them is exact rather than
approximate. The paper's error terms — Gaussian curvature, irregular sampling,
noisy normals, error accumulating along long paths — are **zero** here except
where three faces meet at a corner, where they are concentrated rather than
spread.

The caveat that does apply, stated so it is not discovered later: *"Noise in the
point set affects geodesic distances and causes the parameterization to quickly
degenerate."* A sample set generated from tile solids has no noise. One generated
by sampling a depth buffer would.

## 9.2 Whether the receiving geometry can feed it — and it cannot, as it stands

The question was whether the receiver is reachable at placement as triangles
**with adjacency** or merely as a vertex buffer, because that decides whether the
walk is fifty lines or three hundred. **The answer is neither, and the reason is
structural rather than a missing feature.**

**Every element of the world is a CLOSED box.** `cromwell/geometry/BoxEmitter.hpp`
emits all six faces — 12 triangles, 36 vertices — for a floor slab, a wall slice,
a crate, a ramp tread. Faces buried inside adjacent solid are emitted along with
the visible ones.

**There are no indices.** `MeshVertexBuffer.hpp` says so in as many words: *"NO
INDICES, and that is not an omission: the box emitter produces triangle soup"*.

**And there is nothing to weld them into.** This is the finding that settles it.
Welding by position is the standard repair for triangle soup, and here it
recovers almost nothing, because **abutting geometry does not abut**:

| element | extent | consequence |
|---|---|---|
| floor slab | `0.995 × t × 0.995` centred on a **1.0** tile | a **5 mm gap** between neighbouring floor tiles |
| wall slice | `0.09` thick, `1.04` long, centred on the tile boundary | **overhangs** the tile by 2 cm at each end |
| floor against wall | floor edge at 0.9975, wall outer face at 0.955 | they **interpenetrate by ~4 cm**; there is no shared edge |

Two floor tiles side by side share no vertex and do not touch. A floor and the
wall standing on it share no vertex and pass *through* each other. A
position-weld over this soup yields a graph with essentially no edges — so
building adjacency from the render geometry is not a three-hundred-line job, it
is not possible without re-authoring the emitter to produce welded, watertight,
exterior-only geometry. That would be a change to how the whole world is built in
service of one decal feature, and it would cost the emitter the property that
makes it cheap: **the gaps and the overlaps are deliberate.** They are what let
each tile emit its own geometry and nobody else's, which is what makes a chunk
rebuild after a grenade a local operation.

**What IS reachable at placement**, since the parts are all there:

- `StoreyGeometryEmitter::emit(storey, minX, minY, maxX, maxY, out)` re-emits any
  rectangle of tiles on demand in **final world coordinates**, cold-code cheap.
  Nothing is retained CPU-side after upload — `RhiStatics::rebuild` builds into a
  local `SurfaceBuffers`, interleaves, uploads and drops it — but re-emitting a
  few tiles is a walk over a few tiles.
- `game/picking/WorldTrace.hpp` describes the same world **analytically**: six
  solid kinds per cell — floor slab, ramp *plane*, mass box, wall slab, window,
  canopy — with a `layer::kPaint` layer that already means *"where a decal may
  stick"*.

**So the sample set comes from the second of those, not the first.** The paper
needs points with normals at a known density; the tile solids are an exact
generator for them, and the game already owns that description. That also puts
the engine boundary in the right place: the game hands cromwell a point set, and
cromwell — which may not know what a tile is — runs a walk that needs no notion
of one.

**One trap in that, named before it is met.** A sample set taken from every face
of every solid includes faces buried inside other solids, and a k-NN graph over
it will happily tunnel from one side of a wall to the other — §5.1's far-side
leak returning in a new form, in a system with no depth buffer to catch it. The
fix is already built: **prune the sample set with the per-decal visibility cube**,
the same capture the DBuffer pass reads. A sample the capture says is occluded
from the seed point is not on the surface the mark was thrown at. The two systems
then share one definition of reachable — which is the definition Source's
`UTIL_DecalTrace` has, and the reason any of this works.

## 9.3 The cheap test that has to come first

Before building any of it there is a discriminating observation available,
because reading the shader turned up a discrepancy nobody has looked at.

The wrap carries the texture by `|local.z|` — distance from the **placement
plane**, along the box's own W axis (`decal.fs.glsl`, "THE FOLD IS local.z == 0").
And for a ramp this world has **two different surfaces in the same place**: the
renderer emits a *staircase*, a stack of separate tread and riser boxes,
`kRampArtSteps` of them, while `WorldTrace` treats a ramp as a **smooth inclined
plane** — and it is the trace that supplies the placement point and normal. So a
decal on a stair is currently parameterised from a plane the visible geometry
only zigzags about.

If the box is instead **aligned to the incline** — W along the ramp plane's
normal, U down the slope — every tread and riser sits within about one riser
height of `local.z == 0` for the whole length of the flight, however long, rather
than drifting monotonically away from a horizontal placement plane until it
leaves the texture. **That is a placement change, in the game, with no renderer
work at all**, and the game is exactly the layer allowed to know that the thing
under the cursor is a ramp.

It splits the hypotheses cleanly:

- **If the mark then runs the whole flight** and the only remaining artefact is a
  small per-step wobble, the stair case never needed geodesic parameterisation —
  it needed a basis, and §9's justification narrows to genuinely arbitrary folded
  geometry: rubble heaps, stacked crates, a mark crossing a kerb *and* a wall.
- **If it still breaks up**, the failure is the tread/riser zigzag rather than the
  wrap budget, no choice of plane fixes it, the walk is warranted — and §9.2
  already says where its input has to come from.

This is the discipline the last three defects were found by: get the observation
that names the fault before writing the fix. The test costs one decal placed at
the top of a flight.

## 9.4 The invalidation answer, which §6 says is owed before it ships

Geodesic decal geometry is a derived cache of a shape destruction can delete, and
§6 is right that this is the real cost of the feature. The answer that fits this
codebase's own rules:

**Keep the seed as the authoritative data.** A geodesic decal stores its seed
point, normal, rotation and geodesic radius — POD, like the projector — and the
parameterised geometry is *derived* from those. Invalidating it therefore never
loses the decal; it costs one re-walk, on a placement path already measured in
microseconds.

**And it wants the same version counter the capture already needs.**
`rhi/MIGRATION.md` §4.6 records that a settled decal's visibility capture goes
stale when geometry changes under it, and asks for a scene geometry version to
compare against. That is the identical trigger: a version bump invalidates the
capture and the walk together, because both are answers to *"what was here when
the mark landed"*. One counter serves both — which is an argument for building it
**first**, since it turns §9's headline liability into a re-use of something
already on the list rather than a new invalidation boundary running through the
destruction system.

Both derived things then satisfy CLAUDE.md's derived-cache rules honestly: the
authoritative data is the seed and the tiles, the fast path (a cached walk) is
skipped only when the version says it provably still applies, and the slow path
is the original placement code unmodified.
