#version 450 core
/* transmission.vs.glsl — the sun's view of a translucent surface.
 *
 * depth_only.vs.glsl would do for the position and does not carry what the
 * fragment stage needs: its own position in the sun's clip space, so the depth
 * test against the shadow map can be done by hand.
 *
 * DERIVING THAT FROM gl_FragCoord INSTEAD WOULD NEED THE TARGET'S SIZE, and the
 * target is deliberately a different size from the map being sampled — see
 * transmission.fs.glsl. A varying costs one interpolator and is right at any
 * pair of resolutions, which is the point.
 *
 * See assets/shaders/CONVENTIONS.md.
 */

/* The full layout, though only position is read: the mesh is shared with every
 * other pass, so the stride and offsets must match deviceLayout() exactly. */
layout(location = 0) in vec3 inPosition;

layout(std140, binding = 1) uniform PassBlock {
    mat4 uViewProjection;   /* the SUN's, in this pass */
};

#include "rhi/object.glsl"

layout(location = 0) out vec4 vLightClip;

void main()
{
    vec4 clip = uViewProjection * objectTransform() * vec4(inPosition, 1.0);

    vLightClip  = clip;
    gl_Position = clip;
}
