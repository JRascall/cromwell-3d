/* Decal.hpp — one projector, and where it points.
 *
 * SINGLE RESPONSIBILITY: describe a single decal — the box it projects through,
 * which material it wears, and the scalars that multiply that material. No
 * textures, no draw call, no GPU state.
 *
 * A DECAL IS A BOX, NOT A QUAD, and everything good about this technique
 * follows from that. The pass rasterises the box, unprojects the depth buffer
 * inside it, and inks whatever real surface it finds — so the decal lands on a
 * kerb, a stair nose and a rubble pile with no clipping, no per-receiver
 * geometry and no z-fighting, because at no point is a coplanar triangle
 * submitted. `depth` is how far into the surface the box reaches, and it is the
 * dial that decides how far a decal is willing to wrap.
 *
 * LOCAL SPACE. The unit cube spans -0.5 to +0.5 on every axis.
 *   local +Z   out of the receiving surface; the box projects along -Z
 *   local  X   the decal's U, left to right
 *   local  Y   the decal's V, bottom to top
 * So `transform`'s three columns are the decal's tangent, bitangent and normal
 * scaled to its size, and its fourth is the centre of the box.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

/* Index into DecalSet's material table. */
using DecalMaterialId = int;
constexpr DecalMaterialId kInvalidDecalMaterial = -1;

struct Decal {
    /* Unit cube to world. Build it with onSurface() rather than by hand.
     *
     * Written out rather than MatrixIdentity() so this header does not have to
     * pull in raymath.h — which is not a declaration header but a body of
     * static function definitions, and dragging it into everything that merely
     * wants to name a Decal is a real compile-time cost for one constant.
     *
     * Braces fill raylib's Matrix in DECLARATION order, which is row-major
     * (m0, m4, m8, m12 first), so the ones land on m0/m5/m10/m15 either way. */
    Matrix transform{ 1.0f, 0.0f, 0.0f, 0.0f,
                      0.0f, 1.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 1.0f, 0.0f,
                      0.0f, 0.0f, 0.0f, 1.0f };

    DecalMaterialId material = kInvalidDecalMaterial;

    /* Multiplies the albedo map, exactly as colDiffuse does for a surface. */
    Color tint = WHITE;

    /* Master coverage. Everything else the decal computes — the texture's own
     * alpha, the angle fade, the depth fade — multiplies this. */
    float opacity = 1.0f;

    /* Multiplies the packed map's blue channel to give the emissive mask.
     * The GLOW TAKES ITS COLOUR FROM THE ALBEDO, which is why one scalar is
     * enough: see DecalBuffer.hpp for why that costs one channel instead of a
     * fourth attachment. Zero on everything that is not meant to be self-lit,
     * and the term is then exactly zero rather than merely dim. */
    float emissive = 0.0f;

    /* Factors on the packed map, the same factor-times-texture rule the
     * surface materials follow. With no map these ARE the values, so a decal
     * with an albedo alone can still make the surface under it wetter.
     *
     * METALNESS DEFAULTS TO ZERO, WHICH BREAKS THE glTF RULE ON PURPOSE. That
     * rule would say 1.0 — factor times a white fallback — and the consequence
     * here is not a subtle one: the overwhelming majority of decals have an
     * albedo and no packed map at all, and every one of them would arrive fully
     * CONDUCTIVE. A conductor has no diffuse, so a printed label or a blood
     * pool would render as a sheet of coloured chrome that vanishes wherever
     * there is nothing to reflect. MaterialLibrary makes the same call for the
     * same reason — every built-in surface is pinned at 0 and only the ladder
     * asks for 1. A decal with a real packed map sets this explicitly. */
    float roughness = 0.9f;
    float metalness = 0.0f;

    /* Scales the tangent-space normal's xy. The frame it is applied in is the
     * RECEIVER's, not the projector's — a flat decal normal therefore leaves
     * the surface underneath exactly as it was, at any angle. */
    float normalStrength = 1.0f;

    /* WHETHER THE DECAL GOES ROUND CORNERS, which is the difference between the
     * two things people mean by "a projected decal".
     *
     * With it off there is ONE projection direction — the box's -Z — and any
     * surface roughly parallel to it stretches into a streak and is rejected by
     * the angle fade below. A decal on the ground therefore stops dead where the
     * ground meets a wall. That is Unreal's DBuffer behaviour and it is right for
     * anything that is conceptually a flat sheet: a poster, a road marking, a
     * sign. It should stop at the edge of the thing it is printed on.
     *
     * With it on the projection is resolved PER PIXEL against the surface
     * actually found there, and the texture is unwrapped across the fold — so
     * the same decal runs up the wall, over the kerb and across the stair nose
     * as one continuous mark. That is Source's behaviour, and it is right for
     * anything thrown at the world rather than printed on it: a scorch, a
     * splatter, a stain. See decal.fs.glsl for the unwrap.
     *
     * DEPTH IS THE WRAP BUDGET when this is on. The unwrap carries the texture
     * by the distance travelled from the placement plane, and that distance is
     * capped by the box, so a decal meant to climb a wall needs a box deep
     * enough to reach up it. */
    bool wrap = true;

    /* HOW SQUARELY A SURFACE MUST FACE THE PROJECTOR TO TAKE ANY INK, as the
     * cosine between the receiver's normal and the projection axis. Below
     * `angleFadeStart` the decal is absent, above `angleFadeEnd` it is at full
     * strength, and between them it ramps.
     *
     * THIS IS NOT POLISH — it is the difference between a scorch mark that
     * wraps onto the kerb and one that streaks a metre down the wall behind it.
     * A box projection alone smears its texture across every surface roughly
     * parallel to the projection axis, because those surfaces occupy almost no
     * area in the projected UV and stretch one column of texels along their
     * whole length. Rejecting them is the only fix; there is no filtering that
     * makes a one-texel-wide streak into a decal.
     *
     * The default rejects anything past ~66 degrees off-axis and reaches full
     * strength by ~46, which keeps a decal across a stair nose intact while
     * killing the vertical smear off its ends. */
    float angleFadeStart = 0.40f;
    float angleFadeEnd   = 0.70f;

    /* How much of the box's depth is spent fading out at the near and far
     * faces, as a fraction of `depth`. Without it a decal on a surface the box
     * only just reaches ends in a straight cut across the geometry, which reads
     * as a rectangle rather than as a stain.
     *
     * IT EATS INTO THE USABLE DEPTH, so it is small. The box is centred on the
     * surface, which leaves half the depth on each side, and this consumes the
     * outer 0.15 of that — so a decal reaches cleanly to 70% of `depth` and
     * fades over the last 30%. Raising it toward 0.5 leaves a box with no
     * full-strength region at all: the surface it was placed on is at the very
     * centre and everything else is already fading. */
    float depthFade = 0.15f;

    /* Later decals blend over earlier ones. Kept explicit rather than relying
     * on insertion order so a scorch mark can be made to sit under the blood
     * that was there first. */
    int sortOrder = 0;

    /* ---- placement --------------------------------------------------------
     * `point`   where the decal's centre lands, on the surface
     * `normal`  the receiving surface's normal, world space, unit length
     * `rotation` radians about that normal, 0 = the decal's V runs "up"
     * `size`    width and height in world units (tiles)
     * `depth`   how far through the surface the box reaches, total. Bigger
     *           wraps further round a corner and risks reaching a surface
     *           behind; the angle fade is what stops that being visible. */
    static Decal onSurface(Vector3 point, Vector3 normal, float rotation,
                           Vector2 size, float depth);
};

}  // namespace cromwell
