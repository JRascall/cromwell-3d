#version 330
/* prepass.vs.glsl - the scene prepass: depth, and the normal that goes with it.
 *
 * One pass, two customers. The ribbon has always needed the scene's depth as a
 * sampleable texture to fade itself behind geometry; SSAO needs that same depth
 * AND a normal per pixel. Since the target has to carry a colour attachment
 * anyway (see DepthTarget.hpp), the normal rides along for the cost of an
 * interpolator and a write - no second pass, no MRT, no G-buffer.
 */
in vec3 vertexPosition;
in vec3 vertexNormal;

uniform mat4 mvp;
uniform mat4 matNormal;

out vec3 vNormal;

void main()
{
    vNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
