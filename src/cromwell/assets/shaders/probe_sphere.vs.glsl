#version 330
/* probe_sphere.vs.glsl - the chrome ball that stands in for a reflection probe.
 *
 * Nothing here is specific to a sphere; it is the plainest possible world
 * position + normal pass. The sphere is a mesh raylib generated and the
 * transform comes in through matModel, so one program draws every probe.
 */
in vec3 vertexPosition;
in vec3 vertexNormal;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;

out vec3 vWorldPosition;
out vec3 vNormal;

void main()
{
    vWorldPosition = (matModel * vec4(vertexPosition, 1.0)).xyz;
    vNormal        = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);

    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
