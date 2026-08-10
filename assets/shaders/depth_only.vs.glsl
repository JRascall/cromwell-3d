#version 330
/* depth_only.vs.glsl - position in, depth out, nothing else.
 *
 * Used by two passes that both want a depth buffer and no colour: the sun's
 * shadow map, and the prepass whose depth the ribbon samples to fade itself
 * behind geometry. Running either through the lit shader would pay for a
 * whole BRDF - and, in the shadow map's case, for shadow lookups into the
 * buffer being written - to produce a value neither pass reads.
 */
in vec3 vertexPosition;

uniform mat4 mvp;

void main()
{
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
