/* dbuffer.glsl — reading the decals back, on the surface they landed on.
 *
 * The consumer half of DecalBuffer. The decal pass wrote each plane
 * premultiplied by coverage with the REMAINING transmittance in alpha, so
 * every decode here is the same one line:
 *
 *     result = decalContribution + baseValue * transmittance
 *
 * which is an ordinary over-blend with the multiply already done. Coverage
 * itself is never reconstructed and never needed.
 *
 * INCLUDED BY THE SURFACE SHADER, not by a pass of its own, because that is the
 * whole point of the technique: the decal changes what the material IS, and
 * then the material lights once, with its receiver's shadow, its receiver's
 * probe and its receiver's occlusion already in hand. A decal drawn as its own
 * lit quad would have to recompute all three.
 *
 * THE PLANES ARE AT THE PREPASS'S RESOLUTION AND THIS SHADER IS SUPERSAMPLED,
 * so these lookups are bilinear upsamples. That is deliberate — see
 * DecalBuffer.hpp — and it is also why the fetches are premultiplied: filtering
 * premultiplied data is correct, filtering colour and coverage separately is
 * not, and a decal's edge is exactly where the difference shows.
 */

uniform sampler2D uDBufferAlbedo;
uniform sampler2D uDBufferNormal;
uniform sampler2D uDBufferSurface;

/* 0 or 1, for the dev panel's layer switch. OFF MEANS OFF: skipping the decal
 * PASS alone would leave the last frame's planes bound and still sampled, so
 * the switch has to be here as well as there. */
uniform float uDecalsEnabled;

/* WHAT A FULL-STRENGTH EMISSIVE MASK IS WORTH, in the linear radiance this
 * shader outputs. The mask is one 8-bit channel and the output is HDR, so the
 * scale cannot live in the buffer; it is the same reason a sun can be brighter
 * than a wall. Tuned against SunLight's radiance so a fully self-lit decal
 * reads as glowing rather than merely pale after the tonemap. */
uniform float uDecalEmissiveScale;

struct DecalLayer {
    vec3  albedo;      /* sRGB-encoded, premultiplied by coverage */
    vec3  normal;      /* n * 0.5 + 0.5, premultiplied            */
    float metalness;   /* premultiplied                           */
    float roughness;   /* premultiplied                           */
    float emissive;    /* premultiplied mask                      */
    float transmit;    /* 1 - total coverage                      */
};

/* The identity: everything premultiplied is zero and the base passes through
 * untouched. Handed back wherever decals are switched off or do not apply. */
DecalLayer noDecals()
{
    return DecalLayer(vec3(0.0), vec3(0.0), 0.0, 0.0, 0.0, 1.0);
}

DecalLayer readDecals(vec2 screenUV)
{
    if (uDecalsEnabled < 0.5) return noDecals();

    vec4 albedo = texture(uDBufferAlbedo, screenUV);

    /* THE EARLY OUT IS THE WHOLE COST STORY. Most of the screen has no decal on
     * it, and on those pixels this is one fetch rather than three. The test is
     * on transmittance rather than on a separate mask because alpha is already
     * there and already exact: 1.0 means nothing was ever blended here. */
    if (albedo.a >= 0.999) return noDecals();

    vec4 normal  = texture(uDBufferNormal,  screenUV);
    vec4 surface = texture(uDBufferSurface, screenUV);

    return DecalLayer(albedo.rgb, normal.rgb,
                      surface.r, surface.g, surface.b,
                      albedo.a);
}

/* IN sRGB, ON PURPOSE. The plane holds encoded values, so the base has to be
 * encoded too for the blend to be the one the decal pass assumed — which makes
 * the round trip exact for a single decal, and leaves only decal-over-decal
 * blending in the wrong space. That is the same trade every DBuffer makes, and
 * it buys back the low-end precision an 8-bit linear albedo plane would lose. */
vec3 applyDecalAlbedo(DecalLayer decals, vec3 baseSrgb)
{
    return decals.albedo + baseSrgb * decals.transmit;
}

/* The encoded forms blend, then decode once. Interpolating between two unit
 * vectors shortens the result, hence the normalize — which the base normal
 * needed anyway. */
vec3 applyDecalNormal(DecalLayer decals, vec3 baseNormal)
{
    vec3 encoded = decals.normal + (baseNormal * 0.5 + 0.5) * decals.transmit;
    return normalize(encoded * 2.0 - 1.0);
}

float applyDecalScalar(float decalPremultiplied, float base, float transmit)
{
    return decalPremultiplied + base * transmit;
}

/* The glow takes its hue from whatever albedo ended up on the surface, which is
 * the decal's own wherever the mask is non-zero — see DecalBuffer.hpp for why
 * one channel beats a fourth attachment. */
vec3 decalEmissive(DecalLayer decals, vec3 linearAlbedo)
{
    return linearAlbedo * decals.emissive * uDecalEmissiveScale;
}
