#version 330
/* ribbon.vs.glsl - movement-coverage ribbon, vertex stage.
 *
 * Port of UI_3D.Tile.MovementBorder's WorldPositionOffset input. See
 * ../../study/xcom2_movement_border.hlsl for the recovered material.
 *
 * XCOM's z is up and measured in unreal units; ours is y and measured in tiles
 * (1 tile = 96uu), so every recovered constant arrives here already divided by
 * 96 from the C side.
 */
in vec3 vertexPosition;
in vec2 vertexTexCoord;

uniform mat4 matModel;
uniform mat4 matView;
uniform mat4 matProjection;

uniform vec3  uCamPos;
uniform float uWpoPush;     /* Constant_5: 8uu toward the eye */

out vec2  vUV;              /* u across the width, v along the length in tiles */
out float vWorldY;

void main()
{
    vec3 world = (matModel * vec4(vertexPosition, 1.0)).xyz;

    /* Multiply_8 = normalize(CameraWorldPosition - WorldPosition) * 8uu. The
     * strip already floats 4uu off the floor, but at grazing angles 4uu of
     * vertical lift projects to nothing - the push along the eye ray is what
     * actually keeps it off the surface it lies on. */
    world += normalize(uCamPos - world) * uWpoPush;

    vUV     = vertexTexCoord;
    vWorldY = world.y;

    gl_Position = matProjection * matView * vec4(world, 1.0);
}
