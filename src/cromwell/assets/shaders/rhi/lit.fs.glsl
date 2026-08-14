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
#include "rhi/scene_block.glsl"
#include "rhi/material_block.glsl"

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

#include "rhi/sky.glsl"
#include "rhi/shadow.glsl"

/* AFTER rhi/sky.glsl, which it calls, and after the sampler declarations above
 * — it adds one of its own at slot 4. */
#include "rhi/probes.glsl"

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

    vec3 albedo        = vColour.rgb;
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
    vec3 ambient = ambientDiffuse * uExposureAndAmbient.y * ao + ambientSpecular * ao;

    outRadiance = vec4(sun.diffuse + sun.specular + ambient, 1.0);
}
