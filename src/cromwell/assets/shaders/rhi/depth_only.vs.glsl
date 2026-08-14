#version 450 core
/* depth_only.vs.glsl - position in, depth out, nothing else.
 *
 * THE FIRST SHADER CONVERTED TO THE DEVICE DIALECT. Compare it against
 * ../depth_only.vs.glsl, which is the raylib version and still the one the
 * shipping renderer uses; both exist until the migration reaches parity.
 *
 * WHAT CHANGED, AND WHY EACH ONE HAD TO:
 *
 *   #version 450 core     what glslang consumes for Vulkan-flavoured SPIR-V.
 *                         GL 4.3 accepts everything used here.
 *
 *   layout(location = 0)  the attribute is matched BY INDEX, against
 *                         MeshVertexBuffer::deviceLayout(). Matching by name is
 *                         a reflection feature the explicit APIs do not have.
 *
 *   a std140 block        `uniform mat4 mvp;` cannot be expressed in SPIR-V at
 *                         all - there is no default uniform block - and it
 *                         cannot be driven by ICommandEncoder, which binds by
 *                         numbered slot rather than by name. This is the change
 *                         that forced the whole conversion.
 *
 * Used by the two passes that want depth and no colour: the sun's shadow map,
 * and the prepass whose depth the ribbon samples to fade itself behind
 * geometry. Running either through the lit shader would pay for a whole BRDF -
 * and, in the shadow map's case, for shadow lookups into the buffer being
 * written - to produce a value neither pass reads.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect and the binding table.
 */

/* Only position is read, but the LAYOUT still describes the whole vertex: the
 * mesh is shared with the lit pass, so the stride and offsets must match
 * deviceLayout() exactly. Declaring one attribute and ignoring the rest is
 * correct and costs nothing - unread attributes are not fetched. */
layout(location = 0) in vec3 inPosition;

/* BINDING 1 IS THE PASS BLOCK - once per pass, per the frequency table in
 * CONVENTIONS.md. The shadow map's "camera" is the sun, which is exactly why
 * this is a pass-level matrix rather than a frame-level one: several passes in
 * a frame draw the same world from different viewpoints. */
layout(std140, binding = 1) uniform PassBlock {
    mat4 uViewProjection;
};

/* THE OBJECT'S TRANSFORM, and a depth pass needs it as much as a lit one does -
 * a body that casts its shadow from the wrong place is the same bug as a body
 * DRAWN in the wrong place, and harder to see. */
#include "rhi/object.glsl"

void main()
{
    gl_Position = uViewProjection * objectTransform() * vec4(inPosition, 1.0);
}
