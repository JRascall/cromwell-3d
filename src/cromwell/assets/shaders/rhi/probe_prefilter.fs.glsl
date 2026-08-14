#version 450 core
/* rhi/probe_prefilter.fs.glsl — one mip level of one cube face, GGX-convolved.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ==================== WHY NOT glGenerateMipmap ============================
 *
 * Because a box filter is not a specular lobe. Box-filtered mips are one call
 * and they would "work" in the sense of producing progressively blurrier
 * images — but the mapping from a surface's ROUGHNESS to a mip LEVEL would then
 * mean nothing physical, and every rough reflection would be wrong by an amount
 * nobody can attribute later. It is the exact shortcut study/realtime_reflections.md
 * was written to refuse.
 *
 * This is the split-sum prefilter: importance-sample the GGX distribution for
 * the level's roughness, with the standard N = V = R simplification. Level L
 * holds the probe convolved for roughness L/(levels-1), so the lit shader reads
 * its roughness straight off as a LOD.
 *
 * ===================== ALPHA IS COVERAGE, NOT OPACITY =====================
 *
 * The capture clears to transparent black and lit geometry writes 1, so alpha
 * answers "was there world in this direction" and the lit shader blends to the
 * analytic sky wherever it is zero.
 *
 * A NAIVE BLUR DESTROYS THAT. Averaging colour and alpha independently mixes
 * the black of open sky into geometry, and every rough reflection comes out
 * darker than it should — worst exactly where the sky is largest, which is
 * outdoors, which is most of the board. So the accumulation is PREMULTIPLIED
 * and normalised by the coverage it actually gathered:
 *
 *     colour = sum(rgb * a) / sum(a)      the average radiance over COVERED
 *                                         directions, sky excluded
 *     alpha  = sum(a) / sampleCount       what fraction of the lobe was covered
 *
 * which keeps `mix(sky, probe.rgb, probe.a)` meaning the same thing at every
 * level of the chain.
 *
 * ================== IT READS THE LEVEL ABOVE, NOT LEVEL 0 =================
 *
 * Each level convolves the PREVIOUS one rather than the base. Two reasons: the
 * previous level is already a partial convolution so far fewer samples are
 * needed for the same result, and it suppresses fireflies — a single blazing
 * texel at level 0 has already been averaged down before this level's lobe
 * reaches it, where sampling level 0 directly would smear it across the whole
 * lobe as a bright disc.
 *
 * The source level is bound through a sampler CLAMPED to that one level, which
 * is what makes rendering into level N while reading level N-1 of the same
 * texture legal rather than undefined. See DeviceProbeSet::levelSampler.
 */
#include "rhi/probe_face.glsl"

layout(binding = 0) uniform samplerCubeArray uSource;

layout(location = 0) out vec4 outRadiance;

/* THE HAMMERSLEY SEQUENCE, and the bit-reversal that makes it low-discrepancy.
 * Deterministic on purpose: a probe rebuilt twice must produce the same chain,
 * or a reflection shimmers every time the world is edited. */
float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint index, uint count)
{
    return vec2(float(index) / float(count), radicalInverse(index));
}

/* A HALF-VECTOR DRAWN FROM THE GGX DISTRIBUTION for this roughness, in the
 * tangent frame of `normal`. Karis' formulation — the importance sampling is
 * what turns a few dozen taps into a usable lobe where uniform sampling would
 * need thousands. */
vec3 importanceSampleGGX(vec2 random, vec3 normal, float roughness)
{
    float a = roughness * roughness;

    float phi = 6.2831853 * random.x;
    float cosTheta = sqrt((1.0 - random.y) / (1.0 + (a * a - 1.0) * random.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 halfVector = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    /* AN ARBITRARY TANGENT, chosen away from the normal so the cross product
     * cannot degenerate. Picking +Z unconditionally makes the frame collapse on
     * the two faces whose normal IS +Z, and the symptom is those two faces
     * coming out unfiltered while the other four look right. */
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangentX = normalize(cross(up, normal));
    vec3 tangentY = cross(normal, tangentX);

    return normalize(tangentX * halfVector.x + tangentY * halfVector.y + normal * halfVector.z);
}

void main()
{
    /* N = V = R, the standard simplification. It costs the stretched grazing
     * highlight — the lobe is symmetric about the normal where a real one
     * elongates as the view flattens — and it is what makes a single prefiltered
     * chain serve every view direction instead of one chain per angle. */
    vec3 normal = normalize(probeFaceDirection());

    float roughness = uProbePrefilter.x;
    uint  samples   = uint(uProbePrefilter.y);

    /* LEVEL 0 IS A COPY, not a convolution. Roughness zero is a mirror, its
     * lobe is a single direction, and importance-sampling it would spend the
     * whole budget re-fetching one texel with rounding between the taps. */
    if (roughness <= 0.0) {
        outRadiance = texture(uSource, vec4(normal, uProbePrefilter.z));
        return;
    }

    vec3  colour   = vec3(0.0);
    float coverage = 0.0;
    float weight   = 0.0;

    for (uint i = 0u; i < samples; i++) {
        vec3 halfVector = importanceSampleGGX(hammersley(i, samples), normal, roughness);

        /* The light direction the half-vector implies, with V = N. */
        vec3 light = normalize(2.0 * dot(normal, halfVector) * halfVector - normal);

        float nDotL = dot(normal, light);
        if (nDotL <= 0.0) continue;

        vec4 tap = texture(uSource, vec4(light, uProbePrefilter.z));

        /* PREMULTIPLIED — see the header. The cosine weight is the second half
         * of the split-sum's approximation and it is why this is a weighted
         * average rather than a plain one. */
        colour   += tap.rgb * tap.a * nDotL;
        coverage += tap.a * nDotL;
        weight   += nDotL;
    }

    /* NORMALISED BY COVERAGE, NOT BY WEIGHT, for the colour. Dividing by the
     * total weight instead would scale sunlit geometry down by however much open
     * sky shared its lobe, which is the darkening this whole arrangement exists
     * to avoid. */
    outRadiance = vec4(colour / max(coverage, 1e-4), coverage / max(weight, 1e-4));
}
