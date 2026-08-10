#version 330
/* debug_preview.fs.glsl - render targets, remapped into something an eye can read.
 *
 * Most of what this renderer produces is NOT viewable as a colour. An object id
 * of 1 is 1/255 and reads as black. A depth buffer spends almost its whole
 * range within a hair of 1.0 and reads as white. Showing those raw in the
 * inspector is technically honest and practically useless — worse than useless,
 * because "black" looks exactly like "nothing was written", which is the bug
 * the panel exists to rule out.
 *
 * So each buffer is drawn through the mapping that makes its content visible,
 * and the mapping is chosen per buffer by the code that owns it.
 */
in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

/* 0 raw, 1 stencil ids, 2 depth, 3 alpha only */
uniform int   uMode;
uniform float uNear;
uniform float uFar;
uniform float uDepthScale;   /* world units mapped across the full ramp */

/* Distinct, stable colours from an integer. Neighbouring ids must not land on
 * neighbouring colours or a buffer full of 1,2,3 looks like a smooth gradient
 * and tells you nothing — hence the hash rather than a ramp. */
vec3 idColour(int id)
{
    float h = fract(sin(float(id) * 127.1) * 43758.5453);
    float s = 0.55 + 0.45 * fract(sin(float(id) * 311.7) * 24634.6345);
    vec3 rgb = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return mix(vec3(1.0), rgb, s);
}

void main()
{
    vec4 texel = texture(texture0, fragTexCoord);

    if (uMode == 1) {
        /* Coverage first: a real id of 0 and an untouched texel are the same
         * number, and only alpha separates them. */
        if (texel.a < 0.5) { finalColor = vec4(0.08, 0.08, 0.10, 1.0); return; }

        int id = int(texel.r * 255.0 + 0.5);
        finalColor = vec4(idColour(id), 1.0);
        return;
    }

    if (uMode == 2) {
        /* A depth buffer is hyperbolic: linearising is what turns a field of
         * 0.997 into metres that spread across the ramp. */
        float ndc = texel.r * 2.0 - 1.0;
        float linear = (2.0 * uNear * uFar) / (uFar + uNear - ndc * (uFar - uNear));
        float value = clamp(linear / max(uDepthScale, 0.001), 0.0, 1.0);

        /* Banded, because a smooth grey ramp hides the discontinuities that
         * matter — a silhouette edge is exactly a depth step. */
        float bands = fract(value * 8.0);
        finalColor = vec4(vec3(value) * (0.75 + 0.25 * bands), 1.0);
        return;
    }

    if (uMode == 3) {
        finalColor = vec4(vec3(texel.a), 1.0);
        return;
    }

    finalColor = texel * colDiffuse * fragColor;
}
