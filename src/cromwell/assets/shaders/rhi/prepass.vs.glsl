#version 450 core
/* prepass.vs.glsl — the G-buffer's vertex stage: position and normal, nothing else.
 *
 * Converted from ../prepass.vs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * NO matNormal UNIFORM, and that is a real simplification rather than a
 * rename. The raylib version needed one because raylib draws every mesh with a
 * model matrix and passes the inverse transpose beside it. Here the object's
 * transform arrives in push constants and `mat3` of it is used directly, which
 * is exact for everything drawn today — see rhi/object.glsl for why, and for
 * the one case that will eventually need the real inverse transpose.
 */

/* THE FULL LAYOUT IS DECLARED even though only two attributes are read: the
 * mesh is shared with the shadow and lit passes, so stride and offsets must
 * match MeshVertexBuffer::deviceLayout() exactly. Unread attributes are not
 * fetched, so declaring them costs nothing. */
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(std140, binding = 1) uniform PassBlock {
    mat4 uViewProjection;
};

#include "rhi/object.glsl"

layout(location = 0) out vec3 vNormal;

void main()
{
    mat4 model = objectTransform();

    /* NOT normalised here. The fragment stage normalises what it interpolates
     * anyway, and a unit vector at each of three vertices does not interpolate
     * to a unit vector across the triangle between them — so normalising twice
     * costs an instruction and changes nothing. */
    vNormal = mat3(model) * inNormal;

    gl_Position = uViewProjection * model * vec4(inPosition, 1.0);
}
