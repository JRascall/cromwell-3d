#version 450 core
/* decal.vs.glsl — the projector box, and nothing else.
 *
 * Converted from ../decal.fs.glsl's raylib companion, which had no vertex
 * shader of its own: raylib's DrawMesh supplied one and set matModel for it.
 * See assets/shaders/CONVENTIONS.md.
 *
 * IT PASSES NOTHING TO THE FRAGMENT STAGE, and that is the whole technique
 * rather than an omission. A decal does not shade its own box — the box is a
 * bounding volume whose faces are never seen. Every fragment recovers the REAL
 * surface underneath it by unprojecting the depth buffer, so an interpolated
 * position, normal or UV from these vertices would describe the wrong surface
 * entirely. gl_FragCoord is the only input the fragment stage wants and the
 * hardware provides it.
 *
 * POSITION ONLY. The mesh is a unit cube with no normals, no UVs and no
 * colours, because nothing reads them. */
#include "rhi/include/decal_blocks.glsl"

layout(location = 0) in vec3 aPosition;

void main()
{
    gl_Position = uViewProjection * (uModel * vec4(aPosition, 1.0));
}
