#version 330
/* ribbon_glow.fs.glsl - the blur behind the ribbon's emissive halo.
 *
 * XCOM does not have this shader. UI_3D.Tile.MovementBorder is MLM_Unlit and
 * emits a flat colour; the glow around the ribbon in-game is the scene's bloom
 * doing its job on that emissive. We have no bloom chain, so the ribbon is
 * re-drawn overbright into a half-res RGBA16F target and smeared here instead -
 * a one-light bloom that touches nothing else in the frame.
 *
 * Nine taps, Gaussian, run separably. uTexelDir carries BOTH the axis and the
 * tap spacing, so the C side widens the kernel by doubling it each iteration
 * rather than by adding taps. Spacing of zero collapses the whole thing to a
 * scaled copy, which is how the accumulate and composite blits reuse this.
 */
in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2  uTexelDir;    /* (step/w, 0), then (0, step/h) */
uniform float uScale;

const float W[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);

void main()
{
    vec3 sum;
    if (dot(uTexelDir, uTexelDir) == 0.0) {
        sum = texture(texture0, fragTexCoord).rgb;
    } else {
        sum = texture(texture0, fragTexCoord).rgb * W[0];
        for (int i = 1; i < 5; i++) {
            vec2 o = uTexelDir * float(i);
            sum += texture(texture0, fragTexCoord + o).rgb * W[i];
            sum += texture(texture0, fragTexCoord - o).rgb * W[i];
        }
    }
    /* alpha is never read downstream - every consumer blends ONE:ONE - but it
     * has to be something, and 1 keeps the target debuggable. */
    finalColor = vec4(sum * uScale * fragColor.rgb, 1.0);
}
