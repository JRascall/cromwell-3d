#version 330
/* glass_shadow.vs.glsl - the sun's view of a window, with its UVs.
 *
 * depth_only.vs.glsl would do for the position, and did until the grime layer
 * had to affect what passes THROUGH the pane rather than only how it looks.
 * Dirt is a texture, so the shadow pass needs the same UVs the lit pass uses -
 * and they have to be the same ones, sampled at the same scale, or a streak of
 * grime would darken one patch of floor while appearing somewhere else on the
 * glass.
 *
 * Still no normal and no tangent: this pass has no lighting to do.
 */
in vec3 vertexPosition;
in vec2 vertexTexCoord;

uniform mat4 mvp;

out vec2 vUv;

void main()
{
    vUv = vertexTexCoord;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
