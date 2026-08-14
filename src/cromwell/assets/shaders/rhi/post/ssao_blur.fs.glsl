#version 450 core
/* ssao_blur.fs.glsl — the box filter that pays off the per-pixel rotation.
 *
 * Converted from ../ssao_blur.fs.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * ================= WHY THIS PASS IS NOT OPTIONAL POLISH ===================
 *
 * The occlusion pass rotates its sample kernel by a hash of the pixel, which
 * turns banding into noise ON PURPOSE. This is the second half of that bargain,
 * and half a bargain is worse than neither: without it the rotation's noise is
 * simply left on screen, a grainy field with a four-pixel period lying over
 * every lit surface in the frame.
 *
 * That is exactly what it looks like, too — and it does NOT look like an AO
 * problem. It reads as the whole image being fuzzy and edges being jagged, it
 * is spread evenly over every surface rather than concentrated anywhere, and it
 * survives raising the shadow resolution, refocusing the shadow projection and
 * turning on supersampling, because none of those are what is wrong. The device
 * path shipped without this pass for exactly that reason: the occlusion looked
 * plausible on its own and the artefact was read as a shadow-filter problem.
 *
 * A 4x4 box exactly the size of the rotation's correlation window averages the
 * noise back out, and because AO is a low-frequency signal nothing of value goes
 * with it. See kRotationPeriod in ssao.fs.glsl — the two constants are one
 * decision written in two files and must move together.
 *
 * ============================ BILATERAL, AND WHY ==========================
 *
 * Depth-aware, and it has to be. A plain box average does not know that two
 * neighbouring pixels can be metres apart in the world, so it carries occlusion
 * straight across a silhouette: the darkening computed for whatever is BEHIND an
 * edge bleeds onto the surface in front of it, and because the thing behind is
 * recognisable the bleed reads as the far geometry showing THROUGH the near
 * surface rather than as a soft edge.
 *
 * The weights are renormalised by how many taps survived, so a pixel beside a
 * silhouette blurs over fewer neighbours rather than darkening because the
 * rejected ones contributed zero.
 */

layout(binding = 0) uniform sampler2D uOcclusion;   /* the raw occlusion */
layout(binding = 1) uniform sampler2D uDepth;       /* the depth the occlusion read */

layout(location = 0) out vec4 outOcclusion;

layout(std140, binding = 1) uniform PassBlock {
    mat4 uInverseProjection;
    vec4 uResolution;   /* xy = the occlusion plane's size in pixels. zw spare */
};

/* A quarter of a tile. Wide enough that a wall's own gentle recession is still
 * blurred as one surface, tight enough that anything across a silhouette is a
 * different surface and is dropped. */
const float kDepthThreshold = 0.25;

float viewDepthAt(vec2 uv)
{
    float rawDepth = texture(uDepth, uv).r;

    /* z IS ALREADY 0..1 — the engine's Mat4 produces that range and the GL
     * backend sets glClipControl to match, so the raylib version's
     * `rawDepth * 2.0 - 1.0` would map the whole scene to the wrong half of the
     * frustum and hand back depths that reject every tap. See Mat4.hpp. */
    vec4 clip = vec4(uv * 2.0 - 1.0, rawDepth, 1.0);
    vec4 view = uInverseProjection * clip;
    return view.z / view.w;
}

void main()
{
    vec2 uv    = gl_FragCoord.xy / uResolution.xy;
    vec2 texel = 1.0 / uResolution.xy;

    float centreDepth = viewDepthAt(uv);

    float sum = 0.0;
    float weight = 0.0;

    for (int y = -2; y < 2; y++) {
        for (int x = -2; x < 2; x++) {
            vec2 tapUv = uv + vec2(float(x), float(y)) * texel;

            /* THE REJECTION. A tap on the far side of a silhouette describes a
             * different surface, and averaging it in is what carries occlusion
             * through geometry. */
            if (abs(viewDepthAt(tapUv) - centreDepth) > kDepthThreshold) continue;

            sum    += texture(uOcclusion, tapUv).r;
            weight += 1.0;
        }
    }

    /* Renormalised by the taps that survived. Dividing by 16 regardless would
     * darken every pixel near an edge in proportion to how many neighbours it
     * lost, which is a black outline around everything. */
    outOcclusion = vec4(vec3(weight > 0.0 ? sum / weight : texture(uOcclusion, uv).r), 1.0);
}
