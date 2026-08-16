#version 450 core
/* bloom_down.fs.glsl — one level of the chain, halved.
 *
 * See assets/shaders/CONVENTIONS.md. Second of the three bloom stages; the
 * chain is built in ScenePipeline::drawBloom.
 *
 * ================= WHY A CHAIN AND NOT ONE BIG BLUR =======================
 *
 * A glow worth having reaches tens of pixels, and a Gaussian that wide is
 * hundreds of taps per pixel at full resolution. Halving the image doubles the
 * effective radius of the SAME small filter, so six levels of a cheap kernel
 * span a radius no direct blur could afford — and every level costs a quarter
 * of the one above it, so the whole chain costs about a third of its first
 * level. This is what every shipped bloom does.
 *
 * ================ THE THIRTEEN TAPS, AND WHY NOT FOUR =====================
 *
 * A four-tap box halves cleanly and is what the prefilter uses, where the input
 * is a full-resolution image and there is plenty of signal. Deeper in the chain
 * it is not enough: a box filter's frequency response has lobes, and repeated
 * boxing turns a smooth highlight into a blocky one that visibly PULSES as the
 * camera moves, because which source texels fall in which box changes with
 * sub-pixel motion.
 *
 * The thirteen-tap pattern below — a centre group of four at half-texel offsets
 * plus a 3x3 ring — is the Call of Duty filter, and its whole purpose is to be
 * stable under motion at a cost of thirteen bilinear reads. The overlapping
 * inner square is weighted half, which is what removes the ringing a plain 3x3
 * would leave.
 */
layout(binding = 0) uniform sampler2D uSource;

layout(std140, binding = 1) uniform BloomBlock {
    vec4 uBloom;         /* threshold, knee, intensity, radius — unread here */
    vec4 uSourceTexel;   /* xy = one SOURCE texel in UV, zw = source size    */
    vec4 uTargetSize;    /* xy = this target's size in pixels                */
};

layout(location = 0) out vec4 outColour;

vec3 tap(vec2 uv) { return texture(uSource, uv).rgb; }

void main()
{
    vec2 uv = gl_FragCoord.xy / max(uTargetSize.xy, vec2(1.0));
    vec2 t  = uSourceTexel.xy;

    /* The 3x3 ring, at whole source texels. */
    vec3 a = tap(uv + t * vec2(-2.0,  2.0));
    vec3 b = tap(uv + t * vec2( 0.0,  2.0));
    vec3 c = tap(uv + t * vec2( 2.0,  2.0));

    vec3 d = tap(uv + t * vec2(-2.0,  0.0));
    vec3 e = tap(uv);
    vec3 f = tap(uv + t * vec2( 2.0,  0.0));

    vec3 g = tap(uv + t * vec2(-2.0, -2.0));
    vec3 h = tap(uv + t * vec2( 0.0, -2.0));
    vec3 i = tap(uv + t * vec2( 2.0, -2.0));

    /* And the inner four, at HALF-texel offsets so each is a bilinear average
     * of the four source texels around it — sixteen source texels for four
     * reads, and the reason this filter is thirteen taps rather than
     * twenty-five. */
    vec3 j = tap(uv + t * vec2(-1.0,  1.0));
    vec3 k = tap(uv + t * vec2( 1.0,  1.0));
    vec3 l = tap(uv + t * vec2(-1.0, -1.0));
    vec3 m = tap(uv + t * vec2( 1.0, -1.0));

    /* THE WEIGHTS SUM TO ONE and are not arbitrary: the inner square carries
     * half the result, the outer corners and edges the rest. Changing them
     * without recomputing the sum is how a bloom quietly gains or loses energy
     * per level, which shows up as the glow's brightness depending on the
     * chain's LENGTH rather than on the intensity knob. */
    vec3 colour = (j + k + l + m) * 0.5 * 0.25
                + (a + c + g + i) * 0.125 * 0.25
                + (b + d + f + h) * 0.25 * 0.25
                + e * 0.125;

    outColour = vec4(colour, 1.0);
}
