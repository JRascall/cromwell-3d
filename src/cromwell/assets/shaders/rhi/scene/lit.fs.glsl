#version 450 core
/* lit.fs.glsl — the opaque surface, in linear radiance.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ==================== HOW LITTLE IS LEFT IN THIS FILE =====================
 *
 * Almost everything it does is now an include, and that is the point rather
 * than an accident of tidying. The frame's uniforms, the material's, the sky
 * and the PCSS filter are all shared with rhi/transparent.fs.glsl — so an
 * opaque surface and a translucent one are lit by the same sun, through the
 * same shadow map, against the same sky, by the same BRDF. The only things
 * that differ between the two files are the two inputs that make a material
 * see-through, which is exactly the difference there should be.
 *
 * common/brdf.glsl is shared further still — with the raylib renderer, which
 * includes the same file. A BRDF that drifted between two renderers would look
 * plausible in both and make every comparison meaningless.
 *
 * WHAT THIS IS NOT YET: no albedo, normal or roughness textures. Reflection
 * probes have landed — the ambient specular below goes through
 * environmentSpecular, which returns exactly the analytic sky this file used to
 * compute inline whenever no probe claims the fragment. That was the point of
 * writing it that way: the probes were additive here rather than a rewrite.
 *
 * ========================= LINEAR OUT, NOT DISPLAY ========================
 *
 * Unbounded linear radiance into an RGBA16F target. The tone map resolves it.
 * Writing display colour here — or clamping — is what makes bright surfaces
 * flatten into paper and shadowed ones crush to black.
 */
#include "common/brdf.glsl"
#include "rhi/include/scene_block.glsl"
#include "rhi/include/material_block.glsl"

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColour;
layout(location = 3) in vec4 vShadowClip;

layout(location = 0) out vec4 outRadiance;

layout(binding = 1) uniform sampler2D uOcclusion;

/* THE SHADOW MAP, RAW. Slot 0 is deliberately empty — see rhi/shadow.glsl on
 * why this is not a comparison sampler. */
layout(binding = 2) uniform sampler2D uShadowDepth;

/* WHAT THE SUN BECOMES CROSSING ANYTHING TRANSLUCENT — half the depth map's
 * resolution, and RGBA because a fraction cannot carry a colour. See
 * rhi/transmission.fs.glsl. */
layout(binding = 3) uniform sampler2D uShadowTransmission;

#include "rhi/include/sky.glsl"
#include "rhi/include/shadow.glsl"

/* AFTER rhi/sky.glsl, which it calls, and after the sampler declarations above
 * — it adds one of its own at slot 4. */
#include "rhi/include/probes.glsl"

/* THE DECAL PLANES, at slots 5, 6 and 7.
 *
 * THE OPAQUE SHADER READS THESE AND THE TRANSPARENT ONE DELIBERATELY DOES NOT,
 * which looks like an omission and is the opposite. The DBuffer describes the
 * surface the PREPASS recorded at each pixel — an opaque one — and a pane of
 * glass in front of that surface is a different surface at the same pixel. A
 * transparent shader reading the planes would paint the wall's decal onto the
 * window as well, and then composite the wall behind it with the decal already
 * on it: the same mark, twice, one of them floating in mid air.
 *
 * A decal ON glass is a real thing to want and this is not how it would be
 * done; it needs the decal in the translucent surface's own material, because
 * there is no depth buffer describing a surface that was never written to
 * one. */
#include "rhi/include/dbuffer.glsl"

void main()
{
    vec3 normal = normalize(vNormal);

    /* Undersides seen through a cutaway rasterise back-facing, and shading them
     * with an inward normal makes an opened building black inside. */
    if (!gl_FrontFacing) normal = -normal;

    /* uSunDirection is where light TRAVELS, so the vector TOWARDS the sun is
     * its negation. Getting this backwards lights the shadowed side and is the
     * classic "the sun is inside the world" look. */
    vec3 toSun = normalize(-uSunDirection.xyz);
    vec3 V     = normalize(uCameraPosition.xyz - vWorldPosition);

    float nDotL = max(dot(normal, toSun), 0.0);
    float nDotV = clamp(dot(normal, V), 1e-4, 1.0);

    /* Screen-space, addressed by gl_FragCoord — the occlusion plane is the same
     * resolution as this pass by construction.
     *
     * CLAMPED TO THE PLANE'S SIZE, and that is not defensive. This same shader
     * runs inside a 128-pixel probe capture, where SSAO does not exist and a
     * 1x1 white texture stands in for it — screen coordinates mean nothing in
     * there, and an out-of-range texelFetch returns an undefined value rather
     * than zero. One min() makes the stand-in read as "nothing is occluded",
     * which is the honest answer for a pass with no depth prepass behind it. */
    ivec2 occlusionSize = textureSize(uOcclusion, 0);
    float ao = texelFetch(uOcclusion, min(ivec2(gl_FragCoord.xy), occlusionSize - 1), 0).r;

    /* A surface facing away from the sun is already dark from nDotL; asking the
     * shadow map about it as well spends twenty-four taps to multiply zero. */
    vec3 shadow = nDotL > 0.0 ? sunShadow(vWorldPosition, normal, nDotL) : vec3(1.0);

    /* f0 IS 4% FOR A DIELECTRIC AND THE ALBEDO FOR A METAL. That one mix is the
     * whole of the metal/rough model's specular story: a non-metal reflects a
     * weak white sheen, a metal reflects its own colour and has no diffuse. */
    float roughness = clamp(uMaterialFactors.x, 0.045, 1.0);
    float metalness = clamp(uMaterialFactors.y, 0.0, 1.0);

    vec3 albedo = vColour.rgb;

    /* ---- AND WHAT THE DECALS DID TO ALL OF IT ---------------------------
     *
     * READ ONCE, APPLIED TO FOUR INPUTS, AND THEN THE SURFACE LIGHTS NORMALLY.
     * That is the whole technique: a decal changes what the material IS, and
     * the material is lit once — with this fragment's shadow, this fragment's
     * probe and this fragment's occlusion already in hand. A decal drawn as its
     * own lit quad would have to recompute every one of them.
     *
     * BY gl_FragCoord OVER THE OCCLUSION PLANE'S SIZE, which is this pass's own
     * resolution — reused rather than asking for a second textureSize of the
     * same thing. The DBuffer is HALF that, so these are bilinear upsamples:
     * deliberate, and the reason the planes are premultiplied. See
     * rhi/dbuffer.glsl.
     *
     * INSIDE A PROBE CAPTURE THIS IS SWITCHED OFF, like SSAO and for the same
     * reason: screen space means nothing in a 128-pixel cube face, and the
     * capture binds a 1x1 stand-in whose single texel would otherwise ink the
     * entire face with whatever decal happened to be at the top left. */
    DecalLayer decals = readDecals(gl_FragCoord.xy / vec2(max(occlusionSize, ivec2(1))),
                                   uDecalParams.x);

    albedo    = applyDecalAlbedo(decals, albedo);
    normal    = applyDecalNormal(decals, normal);
    roughness = clamp(applyDecalScalar(decals.roughness, roughness, decals.transmit),
                      0.045, 1.0);
    metalness = clamp(applyDecalScalar(decals.metalness, metalness, decals.transmit),
                      0.0, 1.0);

    /* THE NORMAL MOVED, SO EVERY DOT PRODUCT TAKEN FROM IT IS STALE. nDotL and
     * nDotV were computed above from the geometric normal, and a decal with a
     * normal map that did not relight would be a bump you can see in the
     * diffuse and not in the highlight — which reads as the normal map being
     * too weak rather than as an ordering mistake.
     *
     * The SHADOW is not recomputed: it is a visibility term about where this
     * point is relative to the sun, and a bump in a decal does not move the
     * point. Recomputing it would cost twenty-four taps to get the same
     * answer. */
    nDotL = max(dot(normal, toSun), 0.0);
    nDotV = clamp(dot(normal, V), 1e-4, 1.0);
    vec3 f0            = mix(vec3(0.04), albedo, metalness);
    vec3 diffuseAlbedo = albedo * (1.0 - metalness);

    /* ---- direct ---------------------------------------------------------
     * A full Cook-Torrance response, not a Lambert term. Without the specular
     * lobe every surface is perfectly matte, nothing has a highlight and the
     * whole image sits flatter and brighter than it should. The 1/PI lives
     * inside evaluateDirectional. */
    vec3 incoming = uSunRadiance.rgb * nDotL * shadow;
    SurfaceResponse sun =
        evaluateDirectional(normal, V, toSun, incoming, diffuseAlbedo, f0, roughness);

    /* ---- ambient --------------------------------------------------------
     * NO /PI ON THE SKY, matching pbr.fs.glsl: skyIrradiance returns lobe
     * colours authored as the irradiance arriving, not a radiance to integrate.
     * Dividing one and not the other is deliberate and gets "tidied" into a bug.
     *
     * A rough surface reflects a wide cone, so the sample direction bends back
     * toward the normal as roughness rises rather than sampling a mirror
     * direction the material could not produce. environmentBRDF is Karis'
     * analytic fit to the split-sum integral, standing in for the lookup table
     * a real IBL would sample. */
    vec3 ambientDiffuse = diffuseAlbedo * skyIrradiance(normal);

    /* THE REFLECTION PROBES, WHERE ONE CLAIMS THIS FRAGMENT, and the same
     * analytic sky where none does — environmentSpecular is one call for both,
     * which is why adding probes changed one line here rather than the shape of
     * the file. The ambient intensity goes IN, applied to the sky half only:
     * the cubemap holds radiance the lit pass computed, and scaling that by
     * 0.42 as well would attenuate a physically captured reflection twice. See
     * rhi/probes.glsl. */
    vec3 reflection = normalize(mix(reflect(-V, normal), normal, roughness * roughness));
    vec3 ambientSpecular =
        environmentSpecular(reflection, vWorldPosition, normal, roughness,
                            uExposureAndAmbient.y)
        * environmentBRDF(f0, roughness, nDotV);

    /* OCCLUSION MULTIPLIES THE AMBIENT ONLY. The shadow map already answered
     * what the sun can reach, and darkening direct sunlight with a screen-space
     * term is what makes SSAO look like dirt. The intensity is on the diffuse
     * half alone because the specular half took it above. */
    /* ---- AND THE DEV PANEL'S PER-TERM SWITCHES --------------------------
     *
     * THE ONE PLACE EVERY TERM MEETS, which is why the gating is here rather
     * than at each term's computation. Removing a term is the whole question
     * this panel exists to answer — "is that artefact the reflections" — and an
     * answer is only worth having if the term is genuinely GONE rather than
     * computed and then multiplied by something small. Zeroing at the sum is
     * the version that cannot be partly true.
     *
     * A BRANCH PER TERM AND NOT A MULTIPLY BY A FLOAT. Uniform-driven, so every
     * fragment in the draw takes the same side and the hardware costs nothing
     * for it; and a multiply by zero would keep a NaN produced upstream, which
     * is precisely the case somebody would be using these switches to find. */
    vec3 direct = effectOn(kEffectDirectSun)
                ? sun.diffuse + sun.specular : vec3(0.0);

    vec3 ambient = vec3(0.0);
    if (effectOn(kEffectAmbientDiffuse))  ambient += ambientDiffuse * uExposureAndAmbient.y * ao;
    if (effectOn(kEffectAmbientSpecular)) ambient += ambientSpecular * ao;

    /* ---- and what the surface makes on its own account -------------------
     *
     * ADDED RAW, AFTER EVERYTHING, MULTIPLIED BY NOTHING — not the shadow, not
     * the occlusion plane, not the ambient intensity, not the albedo. Every one
     * of those describes light ARRIVING at a surface, and this is light
     * leaving it. A sign whose glow dimmed when a cloud crossed the sun would
     * be a reflection with extra steps.
     *
     * NOT SCALED BY THE ALBEDO EITHER, which is the mistake available here and
     * the one the decal path DOES make on purpose (`decalEmissive` multiplies
     * by albedo, because a decal's emission is a MASK over a colour it does not
     * own). A material's emission is authored as the colour it emits, so
     * multiplying by the surface colour would make a red strip light on a blue
     * wall come out black — read as the emissive not working. */
    vec3 emissive = effectOn(kEffectEmissive) ? uEmissive.rgb : vec3(0.0);

    /* A SELF-LIT DECAL, taking its hue from whatever albedo ended up here —
     * which is the decal's own wherever its mask is non-zero. One channel in
     * the surface plane instead of a fourth attachment; see rhi/dbuffer.glsl.
     * Gated by the same switch as a material's own emission, because from the
     * panel's point of view they are the same question. */
    if (effectOn(kEffectEmissive))
        emissive += decalEmissive(decals, albedo, uDecalParams.y);

    outRadiance = vec4(direct + ambient + emissive, 1.0);
}
