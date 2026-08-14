/* rhi/shadow.glsl — how much of the sun reaches a point.
 *
 * Percentage-closer soft shadows, ported from common/shadow.glsl minus the two
 * things the device path does not have: the baked lightmap, and the shadow
 * map's transmission plane for glass.
 *
 * SHARED BY THE OPAQUE AND TRANSPARENT SHADERS. A transparent surface is lit by
 * the same sun through the same map — and its BACK face is too, which is what
 * the transmission term asks about. Two copies of a PCSS filter is two things
 * to keep in step for no reason.
 *
 * Requires rhi/scene_block.glsl for the matrices and scales, common/colour.glsl
 * for hash12 (which common/brdf.glsl already pulls in), and a sampler named
 * uShadowDepth bound to the shadow map. A second, uShadowTransmission,
 * holds what the sun becomes crossing anything translucent — see
 * rhi/transmission.fs.glsl.
 *
 * ======================= WHY THE MAP IS READ RAW ==========================
 *
 * Not through a sampler2DShadow. A comparison sampler is the right idea and it
 * gave the wrong answer: GL does not require linear filtering of a 32-bit float
 * depth format, and where the driver falls back to nearest every tap becomes
 * binary, so twelve taps return one of thirteen values. That prints as blocky
 * plateaus along a shadow edge dissolving into speckle where the disc is wide,
 * which reads as a filtering bug anywhere but the sampler. shadowTap does the
 * compare-then-interpolate by hand and is correct on every depth format.
 */
#ifndef XCOM_RHI_SHADOW
#define XCOM_RHI_SHADOW

/* A ROTATED POISSON DISC, not a grid. An ordered pattern turns undersampling
 * into structure — moire, and edges that step in time with the kernel — where
 * an irregular one turns it into noise, which the supersample averages away.
 * The same twelve points common/shadow.glsl uses. */
const vec2 kShadowPoisson[12] = vec2[12](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
    vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
    vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
    vec2( 0.896,  0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

/* How far the filter may ever reach, in texels — PbrShader passes the raylib
 * path the same 48. The cap and the inner ring below move together: widening
 * one without the other trades a hard edge for a grainy one. */
const float kShadowMaxPenumbra = 48.0;
const float kShadowInnerRingAbove = 8.0;

/* ONE SAMPLE, BILINEARLY FILTERED — COMPARE FIRST, INTERPOLATE AFTER. A fetch
 * followed by a compare is binary, so a kernel of N taps returns one of N+1
 * values and a shadow edge at a shallow angle to the texel grid scallops.
 * Comparing four neighbours and interpolating the RESULTS gives a ramp. */
float shadowTap(vec2 uv, float compare, float texel)
{
    vec2 coord = uv / texel - 0.5;
    vec2 base  = floor(coord);
    vec2 f     = coord - base;
    vec2 uv00  = (base + 0.5) * texel;

    float d00 = texture(uShadowDepth, uv00).r;
    float d10 = texture(uShadowDepth, uv00 + vec2(texel, 0.0)).r;
    float d01 = texture(uShadowDepth, uv00 + vec2(0.0, texel)).r;
    float d11 = texture(uShadowDepth, uv00 + vec2(texel)).r;

    vec4 lit = step(vec4(compare), vec4(d00, d10, d01, d11));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

/* WHY PCSS AND NOT A FIXED KERNEL. A fixed filter blurs every shadow equally,
 * so a shaft through a door frame is as soft at the frame as ten tiles away.
 * Real shadows sharpen at contact and widen with distance, because the sun has
 * angular size — and that contact hardening is most of what makes a shadow read
 * as attached to the thing casting it.
 *
 * THE TEXTBOOK FORMULA DOES NOT APPLY: (receiver - blocker) / blocker comes
 * from similar triangles under a PERSPECTIVE light. The sun is directional and
 * its map orthographic, so depth is linear and there is no apex. For a light of
 * angular radius t the penumbra is simply 2 * distance * tan(t). */
/* RETURNS A COLOUR, not a fraction. Light that reached this point through a
 * translucent surface is both dimmer AND differently coloured, and a scalar
 * cannot say so — which is why a window would otherwise cast a grey patch
 * rather than a tinted one. Open air returns white and costs a multiply. */
vec3 sunShadow(vec3 worldPosition, vec3 normal, float nDotL)
{
    float texel      = uShadowScales.x;
    float worldTexel = uShadowScales.y;
    float depthRange = uShadowScales.z;
    float tanAngular = uShadowScales.w;

    /* NORMAL OFFSET, not a depth bias alone. Offsetting the LOOKUP along the
     * normal moves it to where the map's own texel footprint already agrees
     * with the surface, killing acne on grazing faces without the peter-panning
     * a depth bias large enough to do the same would cause. */
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    vec3  offsetPosition = worldPosition + normal * worldTexel * (1.5 + 3.0 * slope);

    vec4 lightSpace = uSunViewProjection * vec4(offsetPosition, 1.0);
    vec3 projected = lightSpace.xyz / lightSpace.w;

    /* xy to texture space. z is ALREADY 0..1 — the engine's Mat4 produces that
     * range and the GL backend sets glClipControl to match, so the usual
     * `* 0.5 + 0.5` on z would halve every depth and shadow the whole world. */
    projected.xy = projected.xy * 0.5 + 0.5;

    /* Outside the sun's box is LIT — and with a focused projection that happens
     * constantly, at the edge of what the camera can see. */
    if (projected.z > 1.0) return vec3(1.0);
    if (any(lessThan(projected.xy, vec2(0.0)))) return vec3(1.0);
    if (any(greaterThan(projected.xy, vec2(1.0)))) return vec3(1.0);

    /* BIAS IN WORLD UNITS, converted here. Written as a bare normalised
     * constant it would mean something different at every zoom, because the
     * projection refits to the camera every frame. */
    const float kWorldBias      = 0.004;
    const float kWorldSlopeBias = 0.020;

    float compare = projected.z - (kWorldBias + kWorldSlopeBias * slope)
                                / max(depthRange, 0.001);

    float angle = hash12(gl_FragCoord.xy) * 6.2831853;
    mat2 rotation = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));

    /* 1. blocker search — raw reads, no filtering: an averaged depth across a
     * silhouette describes a surface that is not there. */
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 12; i++) {
        vec2 offset = rotation * kShadowPoisson[i] * kShadowMaxPenumbra * texel;
        float depth = texture(uShadowDepth, projected.xy + offset).r;
        if (depth < compare) {
            blockerSum += depth;
            blockerCount += 1.0;
        }
    }

    /* WHAT SURVIVED THE JOURNEY, filtered over the SAME DISC as the depth taps.
     * A single unjittered tap here was the last hard edge in the raylib version
     * of this filter: a pane stands on its sill, the sill takes the sun right up
     * to the glass, and the foot of the transmission footprint lands in full
     * light on a ledge the camera can see along. Point-sampled that is a step a
     * whole texel high running at a shallow angle to the grid — a serrated
     * fringe along every sill. */
    vec3 transmitted = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        vec2 offset = rotation * kShadowPoisson[i] * kShadowMaxPenumbra * texel;
        transmitted += texture(uShadowTransmission, projected.xy + offset).rgb;
    }
    transmitted /= 12.0;

    /* Nothing opaque in the way — but something translucent may still have
     * tinted the light, so this returns the transmittance rather than white. */
    if (blockerCount < 0.5) return transmitted;

    /* 2. penumbra from the sun's angular size. Floored at one texel: at true
     * contact a zero-width filter is a point sample, which aliases as hard as
     * no filtering at all. */
    float averageBlocker = blockerSum / blockerCount;
    float distanceWorld  = max((projected.z - averageBlocker) * depthRange, 0.0);
    float penumbra = clamp(2.0 * distanceWorld * tanAngular / max(worldTexel, 1e-6),
                           1.0, kShadowMaxPenumbra);

    /* 3. filter at that radius. Sample count follows radius — density falls as
     * the square of it, so a wide disc gets a second ring rotated 60 degrees
     * off the first so the two do not line up into spokes. */
    float lit = 0.0;
    float taken = 12.0;

    for (int i = 0; i < 12; i++) {
        vec2 offset = rotation * kShadowPoisson[i] * penumbra * texel;
        lit += shadowTap(projected.xy + offset, compare, texel);
    }

    if (penumbra > kShadowInnerRingAbove) {
        float innerAngle = angle + 1.0472;
        mat2 innerRotation = mat2(cos(innerAngle), -sin(innerAngle),
                                  sin(innerAngle),  cos(innerAngle));
        for (int i = 0; i < 12; i++) {
            vec2 offset = innerRotation * kShadowPoisson[i] * penumbra * 0.55 * texel;
            lit += shadowTap(projected.xy + offset, compare, texel);
        }
        taken = 24.0;
    }

    return vec3(lit / taken) * transmitted;
}

#endif
