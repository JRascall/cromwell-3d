/* rhi/dbuffer.glsl — reading the decals back, on the surface they landed on.
 *
 * Converted from common/dbuffer.glsl. See assets/shaders/CONVENTIONS.md.
 *
 * The consumer half of the DBuffer. The decal pass wrote each plane
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
 * =============== WHAT THE CONVERSION CHANGED, AND ONE THING IT DID NOT =====
 *
 * The three planes became explicit bindings and `uDecalsEnabled` became a lane
 * of the scene block rather than a loose uniform — this dialect has no loose
 * uniforms, which is the reason common/environment.glsl and common/shadow.glsl
 * are NOT shared between the two renderers while brdf and colour are.
 *
 * WHAT DID NOT CHANGE IS THE RESOLUTION ARGUMENT. The planes are at the
 * PREPASS's resolution and this shader is supersampled, so these lookups are
 * bilinear upsamples. That is deliberate, and it is also why the fetches are
 * premultiplied: filtering premultiplied data is correct, filtering colour and
 * coverage separately is not, and a decal's edge is exactly where the
 * difference shows.
 */
#ifndef XCOM_RHI_DBUFFER
#define XCOM_RHI_DBUFFER

layout(binding = 5) uniform sampler2D uDBufferAlbedo;
layout(binding = 6) uniform sampler2D uDBufferNormal;
layout(binding = 7) uniform sampler2D uDBufferSurface;

struct DecalLayer {
    vec3  albedo;      /* LINEAR, premultiplied by coverage       */
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

/* `enabled` is the dev panel's layer switch, from the scene block.
 *
 * OFF MEANS OFF, AND IT HAS TO BE TESTED HERE AS WELL AS AT THE PASS. Skipping
 * the decal pass alone would leave the LAST frame's planes bound and still
 * sampled — so the switch would freeze the decals rather than remove them,
 * which is the exact trap RenderEffects.hpp records the probe switch falling
 * into. A pipeline's bindings are the same every frame; that is the general
 * reason and this is one instance of it. */
DecalLayer readDecals(vec2 screenUV, float enabled)
{
    if (enabled < 0.5) return noDecals();

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

/* IN LINEAR, AND THIS IS THE ONE PLACE THE DEVICE PATH DELIBERATELY DIVERGES
 * FROM THE RAYLIB ONE.
 *
 * That path stores the albedo plane sRGB-ENCODED in RGBA8 and blends the base
 * in its encoded form, because an 8-bit LINEAR albedo plane spends most of its
 * codes on brightnesses nothing uses and crushes the darks. The trade it
 * accepts in exchange is that decal-over-decal blending happens in the wrong
 * space.
 *
 * Here the albedo plane is RGBA16F instead, so there is no precision argument
 * left to make and everything stays linear end to end: the decal's map is an
 * sRGB texture the hardware decodes on fetch, the plane holds linear
 * premultiplied radiance, the blender composites linear over linear, and the
 * surface's own albedo is already linear. Overlapping decals are then correct
 * rather than merely acceptable.
 *
 * IT COSTS FOUR BYTES A PIXEL ON ONE PLANE, at the DBuffer's resolution rather
 * than the scene's — see ScenePipeline on why that is half the supersampled
 * target. The other two planes stay RGBA8: a normal and three 0..1 scalars have
 * no range to lose. */
vec3 applyDecalAlbedo(DecalLayer decals, vec3 baseLinear)
{
    return decals.albedo + baseLinear * decals.transmit;
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
 * the decal's own wherever the mask is non-zero — one channel instead of a
 * fourth attachment.
 *
 * `scale` is what a full-strength mask is worth in linear radiance. The mask is
 * one 8-bit channel and this shader outputs HDR, so the scale cannot live in
 * the buffer — the same reason a sun can be brighter than a wall. It arrives in
 * the scene block beside the enable. */
vec3 decalEmissive(DecalLayer decals, vec3 linearAlbedo, float scale)
{
    return linearAlbedo * decals.emissive * scale;
}

#endif
