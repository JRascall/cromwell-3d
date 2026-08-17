#version 450 core
/* decal_visibility.fs.glsl — how far the nearest surface is, per direction.
 *
 * RADIAL DISTANCE, NOT DEPTH, and that is what makes the consumer a single
 * compare. A cube face's depth buffer is distance along that face's axis, so
 * reading it back needs the face's own matrix and a reconstruction; the decal
 * pass has a direction and a length in hand and nothing else. Writing the
 * length here moves that work to a pass that runs once per decal instead of
 * once per pixel, and removes the chance of the two disagreeing about which
 * face a direction belongs to.
 *
 * THE DEPTH TEST STILL DOES THE SORTING. This writes the distance of whatever
 * fragment survives, and the nearest one survives — so the stored value is the
 * nearest surface in that direction, which is the only one that can occlude.
 *
 * CLEARED TO A LARGE VALUE, meaning "nothing in the way": a direction with no
 * geometry in it must let the decal through, not block it. */
layout(location = 0) in vec3 vWorld;

layout(location = 0) out float outDistance;

layout(std140, binding = 1) uniform DecalCaptureBlock {
    mat4 uViewProjection;
    vec4 uOrigin;       /* xyz = capture origin, w = reach */
};

void main()
{
    outDistance = length(vWorld - uOrigin.xyz);
}
