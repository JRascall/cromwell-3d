#version 450 core
/* decal_visibility.vs.glsl — the world, seen from where a decal was thrown.
 *
 * ONE FACE OF ONE DECAL'S CAPTURE. The pass renders the geometry around a decal
 * six times, once per cube face, and stores the DISTANCE from the decal's own
 * origin to the nearest surface in every direction. The decal pass then inks a
 * recovered surface only if it is no further away than that — which is to say,
 * only if it could be SEEN from the point the decal was thrown at.
 *
 * WHY A CAPTURE AND NOT ANOTHER TEST IN THE PROJECTION. Because the thing the
 * decal pass is missing is not a formula, it is knowledge of what is solid. A
 * stair riser and the far side of a wall present identical normals at identical
 * angles; the only difference between them is whether anything stands in the
 * way, and a depth buffer rendered from the decal's own position is exactly the
 * answer to that. It is Source's rule — the surfaces reachable from the impact
 * point — arrived at with a rasteriser rather than a triangle list, and it asks
 * nothing of the game: no grid, no collision, no notion of a tile.
 *
 * IT IS RENDERED WHEN THE DECAL IS PLACED, not per frame. A mark that has
 * settled on a wall is looking at geometry that is not moving; the capture is
 * only redone when the decal is new, when it moves (the dev tool's preview does
 * both every frame) or when the world under it changes.
 *
 * POSITION ONLY, and the world position is the whole output — the fragment
 * stage measures from it. */
layout(location = 0) in vec3 inPosition;

layout(std140, binding = 1) uniform DecalCaptureBlock {
    mat4 uViewProjection;

    /* xyz = where the capture is taken from, w = how far it reaches. The origin
     * is the decal's centre pushed slightly OUT of the surface it was placed on:
     * measured from the surface itself, every ray along that surface grazes it
     * and the test degenerates into shadow acne on the decal's own receiver. */
    vec4 uOrigin;
};

#include "rhi/include/object.glsl"

layout(location = 0) out vec3 vWorld;

void main()
{
    vec4 world = objectTransform() * vec4(inPosition, 1.0);
    vWorld = world.xyz;
    gl_Position = uViewProjection * world;
}
