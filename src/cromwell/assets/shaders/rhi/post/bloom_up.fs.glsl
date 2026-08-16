#version 450 core
/* bloom_up.fs.glsl — one level of the chain, added back into the one above it.
 *
 * See assets/shaders/CONVENTIONS.md. Third of the three bloom stages, and the
 * composite into the scene uses it too — see ScenePipeline::drawBloom.
 *
 * ================= ADDITIVE, AND THE BLEND STATE SAYS SO =================
 *
 * This shader outputs only its own contribution; the pipeline blends ONE / ONE
 * so it ACCUMULATES onto whatever the level already holds. That is what makes
 * the chain a sum over radii rather than a single blur at the widest one, and
 * it is why a bloom built this way has a long soft tail with a bright core
 * instead of a uniform smear — each level contributes its own scale.
 *
 * Reading it as "the shader forgot to add the destination" is the mistake
 * available here; the destination is the blender's job and doing it in the
 * shader would need the target bound as a texture while it is an attachment,
 * which is undefined on every backend.
 *
 * ==================== A TENT, AND WHAT `radius` MOVES ====================
 *
 * Nine taps in a 3x3 with tent weights, spread by `uBloom.w` texels of the
 * SOURCE. A tent is the right filter going up because it is exactly what
 * bilinear magnification would do at radius 1 — so the knob starts at "no
 * extra blur" and widens from there, rather than starting at some arbitrary
 * softness that has to be tuned back out.
 *
 * THE RADIUS IS IN SOURCE TEXELS, WHICH MEANS IT IS RESOLUTION-INDEPENDENT
 * BY CONSTRUCTION. A radius in pixels would make the glow's shape change with
 * the window size — wider on a big monitor, tighter on a small one — which is
 * the class of bug §4.11 keeps naming: a constant that quietly encodes one
 * display.
 */
layout(binding = 0) uniform sampler2D uSource;

layout(std140, binding = 1) uniform BloomBlock {
    /* x = threshold, y = knee — unread here.
     * z = INTENSITY, applied only by the final composite into the scene; the
     *     intermediate upsamples pass 1.0 so the chain sums to itself.
     * w = the tent's radius, in source texels. */
    vec4 uBloom;

    vec4 uSourceTexel;   /* xy = one SOURCE texel in UV, zw = source size */
    vec4 uTargetSize;    /* xy = this target's size in pixels             */
};

layout(location = 0) out vec4 outColour;

vec3 tap(vec2 uv) { return texture(uSource, uv).rgb; }

void main()
{
    vec2 uv = gl_FragCoord.xy / max(uTargetSize.xy, vec2(1.0));
    vec2 t  = uSourceTexel.xy * max(uBloom.w, 0.0);

    /* The 3x3 tent: corners 1, edges 2, centre 4, over sixteen. */
    vec3 colour =
          tap(uv + t * vec2(-1.0,  1.0)) * 1.0
        + tap(uv + t * vec2( 0.0,  1.0)) * 2.0
        + tap(uv + t * vec2( 1.0,  1.0)) * 1.0

        + tap(uv + t * vec2(-1.0,  0.0)) * 2.0
        + tap(uv)                        * 4.0
        + tap(uv + t * vec2( 1.0,  0.0)) * 2.0

        + tap(uv + t * vec2(-1.0, -1.0)) * 1.0
        + tap(uv + t * vec2( 0.0, -1.0)) * 2.0
        + tap(uv + t * vec2( 1.0, -1.0)) * 1.0;

    colour *= 1.0 / 16.0;

    /* THE INTENSITY IS ONE EXCEPT ON THE LAST PASS. Scaling it into every
     * upsample would compound: six levels at 0.5 would leave the widest
     * contribution at 0.015 and the glow would be a hard core with no tail,
     * which reads as the radius knob doing nothing. */
    outColour = vec4(colour * uBloom.z, 1.0);
}
