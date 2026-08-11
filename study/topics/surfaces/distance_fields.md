# Distance fields

Why cromwell stores some boundaries as distance rather than coverage, what the
encoding is, and which problems it is and is not the answer to.

Written before the implementation, because the decisions that matter here are
about what generalises — and the tempting generalisation is the wrong one.

---

## 1. The problem it solves

A texture normally stores **coverage**: "this texel is 40% ink", "this texel is
70% land". That is a picture of a boundary at one resolution. Magnify it and
bilinear filtering interpolates the *blur*, giving a soft ramp instead of an
edge. Minify it and it aliases.

That is fine whenever the on-screen size is known in advance, which is why the
UI kit rasterises text at exactly the size it will be drawn at and why that is
the right answer there.

It stops being fine the moment the on-screen size changes per frame:

- a unit nameplate as the camera pushes in
- floating damage numbers at any depth
- a coastline seen from the strategic zoom and from ground level

There is no size to bake, so nothing can be baked as coverage.

## 2. Store distance instead

A **signed distance field** stores, per texel, how far that point is from the
nearest boundary — negative outside, positive inside. The texture stops being a
picture of the shape and becomes a description of where its edge is.

The reason this survives magnification is that **distance is locally linear and
coverage is not**. Halfway between two texels the true distance genuinely is
about the mean of the two, so bilinear interpolation *reconstructs* the edge
position rather than smearing it. The geometry is stored implicitly and the GPU
rebuilds a sharp edge from a low-resolution texture.

`[PAPER]` Green, C. *Improved Alpha-Tested Magnification for Vector Textures
and Special Effects*, SIGGRAPH 2007 — Valve's original, shipped in Team
Fortress 2 for signage and decals.

## 3. Why plain SDF is not enough for text

One texel holds one number: distance to the *nearest* edge. Along a smooth
curve that is exact. At a **corner**, two edges meet and the true field has a
crease — it is the intersection of two linear ramps. Bilinear interpolation
cannot represent a crease, so it smooths it, and every sharp corner rounds off.

On text this is immediately visible: the points of `E`, `T`, `L`, `7`, `M` and
`W` get shaved and letters read as slightly melted. It is the single reason
plain-SDF text keeps getting adopted and then abandoned.

**MSDF** stores three distances, in R, G and B, each carrying a different
subset of the shape's edges, assigned so that two channels disagree in a
specific way at every corner. The shader takes the **median**. Along a smooth
edge all three agree and the median is just the distance — identical to plain
SDF. At a corner the median of three linear functions reproduces the crease
exactly, because that is what the intersection of two half-planes is.

`[PAPER]` Chlumský, V. *Shape Decomposition for Multi-Channel Distance
Fields*, MSc thesis, Czech Technical University in Prague, 2015 — and the
`msdfgen` library that came out of it, which is the reference implementation
everyone uses.

## 4. The encoding contract

Stated in code twice, in the two languages that have to agree:
`cromwell/sdf/DistanceField.hpp` and `assets/shaders/common/sdf.glsl`.

| | |
|---|---|
| Edge value | **0.5**, not 0 — the field lives in an unsigned 8-bit texture, so both signs need range. Also means a field debugs as a greyscale image where mid grey is the outline. |
| `pxRange` | How many **texels** the 0..1 range spans. A property of the **bake**, so it travels in the atlas metrics and is never typed twice. |
| Channels | 1 from a raster mask, 3 from vector outlines. |

`pxRange` is the number that goes wrong quietly. Past that distance the field
clamps and carries no gradient at all, so an effect reaching further than the
range simply stops — a glow that wants eight pixels needs a range that reaches
eight pixels. But every texel of range is a texel not spent on the shape. Text
wants 2–4; a coastline sampled well inland and well out to sea wants far more.

A producer baking at 4 and a shader decoding at 2 does not crash and does not
warn. It looks like slightly crunchy edges, which reads as "the antialiasing
needs tuning" and sends you to the wrong file.

## 5. Antialiasing is screen-space, and that is the whole point

The softening band is derived per fragment from how fast the field changes
**across the screen**, so it stays one pixel wide whether the shape is eight
pixels tall or fills the display.

Derived from `fwidth(uv)` rather than `fwidth(decodedValue)`. The shortcut of
taking the derivative of the decoded value skips `pxRange` entirely and very
nearly works; it fails in two places. The median jumps where *which channel is
the median* changes, putting a bright speck at some corners, and in the clamped
region beyond `pxRange` the field is flat, so the derivative is zero and the
divide explodes. UV derivatives are well behaved everywhere because UVs are
linear across a triangle.

## 6. Two inputs, therefore two generators

There is deliberately **no single generator**, because the inputs share nothing
but their output format:

**Vector outlines → MSDF.** Contours from FreeType, edge-coloured, then the
distance to each coloured subset solved per texel. This is `msdfgen` /
`msdf-atlas-gen` territory and not worth reimplementing. Offline, at build time.

**Raster mask → SDF.** A binary land/sea or material mask, distance-transformed.
Two standard routes:

- `[PAPER]` Felzenszwalb & Huttenlocher, *Distance Transforms of Sampled
  Functions* — exact Euclidean distance transform in O(n) per row and column,
  CPU, trivially correct. The right default for a bake.
- `[PAPER]` Rong & Tan, *Jump Flooding in GPU with Applications to Voronoi
  Diagram and Distance Transform*, I3D 2006 — approximate, O(log n) passes,
  runs on the GPU. Wanted only if a mask changes at runtime (terrain
  deformation, a coastline that erodes).

cromwell has compute (`gpu/compute/`), so jump flooding is available if a
runtime case appears. It should not be built before one does.

## 7. What baking costs, and what drawing costs

These are not the same thing and conflating them is the usual confusion.

**Baking** is expensive: for every texel, find the nearest point on the input's
contours, plus MSDF's edge-colouring pass. Offline, once, at build time.

**Drawing** is two triangles and a fragment shader that is a median, a
derivative and a clamp. Cheaper than most of what the renderer already does per
frame. The atlas is static data — a texture you ship.

So floating damage numbers are dynamic **strings**, not dynamic **glyphs**.
`"1,247"` is five quads pointing at five cells baked months ago. Two hundred
numbers is four thousand vertices against one bound texture.

Runtime generation is only needed when the glyph set is not knowable in advance
— user-supplied fonts, or CJK where you cannot bake tens of thousands of glyphs
and must cache on demand. Neither applies here.

## 8. Consumers

| Consumer | Channels | Input | Status |
|---|---|---|---|
| World-space text — nameplates, damage numbers, map markers | Multi | Glyph outlines | planned |
| Coastlines, terrain material boundaries | Single | Raster mask | planned |
| Decal and effect masks, soft edges that stay soft | either | either | speculative |

## 9. What this is NOT for

**The UI widget kit.** `ui/shape/Shapes.hpp` documents the rejection at length
and it stands: a rounded-box shader evaluates an analytic distance in the
shape's **local** space, so its antialiasing spread is fixed there — a 3 px
spoke gets the same absolute softening as a 300 px panel and turns to mush —
and rotated shapes render visibly slimmer because the field is evaluated on a
quad that no longer aligns with it. Exact geometry with a one-pixel feather has
neither problem.

That rejection is about **analytic distance in local space**. A sampled field
decoded through `sdf.glsl` derives its band from the screen-space derivative,
which is a different technique that shares a name. The two conclusions are
consistent, and the earlier one is the reason the later one must use `fwidth`.

**Small screen-space text.** No hinting, and below roughly 12–14 px the field
cannot carry enough detail. The UI kit rasterises per size with FreeType light
hinting instead — see `ui/paint/UiFontSet.hpp`.

## 10. Known limits

- Very thin features collapse if the atlas resolution is too low for them.
  Inter is a sturdy sans and unlikely to bite; a hairline-serif face would be a
  different conversation.
- Three channels is three times the memory of one. One atlas serving every size
  usually wins that trade outright, but it is a real cost for a single-size use.
- MSDF is not a magic sharpener. It reconstructs the boundary the bake was
  given; it cannot recover detail finer than `pxRange` allowed for.
