#version 330
/* pbr.vs.glsl - lit world geometry, vertex stage.
 *
 * Every attribute here is one raylib's DrawMesh binds by itself from the Mesh
 * arrays, at the standard locations - no custom attribute setup anywhere on
 * the C++ side. It is also exactly the set a glTF import arrives carrying, so
 * generated placeholder boxes and authored meshes feed the same program.
 */
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexTangent;      /* xyz = u direction, w = bitangent handedness */
in vec2 vertexTexCoord;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;

out vec3 vWorldPosition;
out vec3 vNormal;
out vec4 vTangent;
out vec2 vUv;
out vec4 vColour;

void main()
{
    vWorldPosition = (matModel * vec4(vertexPosition, 1.0)).xyz;

    /* matNormal is the inverse transpose raylib builds per draw. The units are
     * drawn as one shared cube scaled non-uniformly per body part, and under
     * non-uniform scale the model matrix does not preserve normals - a barrel
     * scaled long and thin would light as though it were still a cube. */
    vNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);

    /* The tangent is a DIRECTION along the surface, so it transforms with the
     * model matrix, not with the inverse transpose the normal needs. The
     * handedness in w is a sign and is carried through untouched. */
    vTangent = vec4((matModel * vec4(vertexTangent.xyz, 0.0)).xyz, vertexTangent.w);

    vUv     = vertexTexCoord;
    vColour = vertexColor;

    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
