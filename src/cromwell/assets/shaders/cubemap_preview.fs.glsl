#version 400
/* cubemap_preview.fs.glsl - one reflection probe, unrolled so it can be looked at.
 *
 * ImGui can display a 2D texture and nothing else, so the six cubemap faces are
 * blitted side by side into one strip: +X -X +Y -Y +Z -Z, left to right, the
 * same order GL numbers them.
 *
 * 400 RATHER THAN 330, and one probe rather than the probe: the probes now live
 * in a cubemap ARRAY with a layer per room, and samplerCubeArray is GL 4.0.
 * uLayer picks which room is on show - a cubemap array you cannot look at is
 * worse to debug than a single cubemap was, because now "which layer" is also a
 * question that can have the wrong answer.
 *
 * MAGENTA MEANS EMPTY. The probe's alpha channel is coverage - 1 where the
 * capture found geometry, 0 where it saw open sky, which is how the lit shader
 * knows to fall back to the analytic sky. Painting the uncovered texels a
 * colour nothing in the world is makes "the capture never ran" instantly
 * different from "the capture ran and that direction is sky".
 */
in vec2 fragTexCoord;
out vec4 finalColor;

uniform samplerCubeArray uCubemap;
uniform float uLayer;           /* which probe, i.e. which room */

void main()
{
    vec2 grid = fragTexCoord * vec2(6.0, 1.0);
    int  face = int(floor(grid.x));
    vec2 st   = vec2(fract(grid.x), grid.y) * 2.0 - 1.0;

    float s = st.x;
    float t = st.y;

    /* GL's own face parameterisation. The sign flips are not decorative: a
     * cubemap is defined in a left-handed space, so a face sampled with the
     * intuitive mapping comes out mirrored. */
    vec3 direction;
    if      (face == 0) direction = vec3( 1.0,   -t,   -s);
    else if (face == 1) direction = vec3(-1.0,   -t,    s);
    else if (face == 2) direction = vec3(   s,  1.0,    t);
    else if (face == 3) direction = vec3(   s, -1.0,   -t);
    else if (face == 4) direction = vec3(   s,   -t,  1.0);
    else                direction = vec3(  -s,   -t, -1.0);

    vec4 probe = texture(uCubemap, vec4(direction, uLayer));

    /* The probe is linear HDR and this preview is eight bits, so it needs a
     * curve of its own - Reinhard is enough to see structure by. */
    vec3 colour = probe.rgb / (probe.rgb + vec3(1.0));
    colour = pow(clamp(colour, 0.0, 1.0), vec3(1.0 / 2.2));

    finalColor = vec4(mix(vec3(0.35, 0.0, 0.35), colour, clamp(probe.a, 0.0, 1.0)), 1.0);
}
