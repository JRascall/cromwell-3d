#version 450 core
/* lit.vs.glsl — the shaded pass's vertex stage.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * WHAT IT CARRIES FORWARD: world position (for the shadow lookup and, later,
 * the view vector), the world normal, and the vertex colour the box emitter
 * baked in. Nothing is transformed on the way — the static world is built in
 * world space, so position and normal arrive final.
 */

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;    /* declared for stride; unused here yet */
layout(location = 3) in vec2 inTexCoord;   /* likewise, until materials land */
layout(location = 4) in vec4 inColour;

#include "rhi/include/scene_block.glsl"

#include "rhi/include/object.glsl"

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vColour;

/* THE SHADOW COORDINATE IS COMPUTED HERE, not in the fragment stage. It is a
 * matrix multiply per vertex instead of per pixel, and the interpolation
 * between them is exact because the transform is affine in clip space. */
layout(location = 3) out vec4 vShadowClip;

void main()
{
    mat4 model = objectTransform();
    vec4 world = model * vec4(inPosition, 1.0);

    vWorldPosition = world.xyz;
    vNormal        = mat3(model) * inNormal;

    /* TINT TIMES VERTEX COLOUR, which is what lets one shader draw both the
     * world and a body — see rhi/object.glsl. */
    vColour     = inColour * objectTint();

    /* THE SHADOW COORDINATE FROM THE TRANSFORMED POSITION, not the raw one.
     * Using inPosition here would shadow every body as though it stood at the
     * origin: the lattice would look correct, because its transform is
     * identity, and only the moving things would be wrong. */
    vShadowClip = uSunViewProjection * world;

    gl_Position = uViewProjection * world;
}
