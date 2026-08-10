#version 330
/* decal.vs.glsl — the projector box, vertex stage.
 *
 * It does almost nothing on purpose. All a decal's geometry contributes is
 * WHICH PIXELS TO CONSIDER: the box is a bounding volume, and every fragment
 * inside it re-derives the real surface by unprojecting the depth buffer. So
 * there is no interpolated UV, no interpolated normal and no tangent frame
 * coming out of here — a decal that took its UVs from the box's own faces would
 * be a painted crate, not a decal.
 */
in vec3 vertexPosition;

uniform mat4 mvp;

void main()
{
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
