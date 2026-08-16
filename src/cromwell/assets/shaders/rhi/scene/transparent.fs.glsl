#version 450 core
/* transparent.fs.glsl — the translucent material.
 *
 * See assets/shaders/CONVENTIONS.md for the dialect.
 *
 * ================ THE SAME MATERIAL, WITH TWO MORE INPUTS =================
 *
 * This is not a glass shader. It is rhi/lit.fs.glsl's surface response — the
 * same Cook-Torrance BRDF from the same shared common/brdf.glsl, the same sun,
 * shadow and sky ambient — plus the two inputs that make a material see-through:
 *
 *   OPACITY       how much of what is behind survives, varying with view angle
 *   TRANSMISSION  light arriving through the surface from the far side
 *
 * That is the shape a translucent material has in any modern authoring tool,
 * and it is why glass, water, a leaf and a paper lantern are one shader here
 * rather than four. Nothing below knows it is drawing a window.
 *
 * WHAT MAKES A PANE READ AS A PANE, and it is not the tint: it is the WORLD
 * REFLECTED IN IT. A sun glint exists at one angle; the environment's
 * reflection exists at every angle, and it is what lets the eye place a sheet
 * of glass in space at all. Everything below about not scaling specular by
 * coverage is in service of that one fact.
 *
 * AND IT IS THE ROOM, NOT THE SKY, ON AN INTERIOR PANE. The ambient specular
 * below goes through the reflection probes, which is what turns a window from a
 * uniformly sky-tinted sheet into something that shows the room it is set in.
 * Glass and the ladder are the two surfaces smooth enough for that to be
 * visible at all — everything else on the board is roughness 0.8, where the
 * term has already blended back to the analytic sky. See rhi/probes.glsl.
 *
 * ========================= NO REFRACTION, ON PURPOSE ======================
 *
 * A flat pane has two parallel interfaces whose bends cancel, leaving a
 * displacement of about a pixel that is uniform across the surface — and a
 * uniform displacement has no reference to be seen against. Curved or thick
 * glass would need it; windows do not. See PbrMaterial, where the same
 * reasoning is written out. If water arrives and wants it, it wants a normal
 * map and a screen-space offset, and it wants them HERE rather than in a
 * shader of its own.
 */
#include "common/brdf.glsl"

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColour;
layout(location = 3) in vec4 vShadowClip;

layout(location = 0) out vec4 outRadiance;

#include "rhi/include/scene_block.glsl"
#include "rhi/include/material_block.glsl"

layout(binding = 1) uniform sampler2D uOcclusion;
layout(binding = 2) uniform sampler2D uShadowDepth;

/* WHAT THE SUN BECOMES CROSSING ANYTHING TRANSLUCENT — half the depth map's
 * resolution, and RGBA because a fraction cannot carry a colour. See
 * rhi/transmission.fs.glsl. */
layout(binding = 3) uniform sampler2D uShadowTransmission;

#include "rhi/include/shadow.glsl"
#include "rhi/include/sky.glsl"

/* After rhi/sky.glsl, which it calls. It adds a samplerCubeArray at slot 4. */
#include "rhi/include/probes.glsl"

void main()
{
    vec3 normal = normalize(vNormal);
    if (!gl_FrontFacing) normal = -normal;

    vec3 V = normalize(uCameraPosition.xyz - vWorldPosition);
    vec3 toSun = normalize(-uSunDirection.xyz);

    float nDotL = max(dot(normal, toSun), 0.0);
    float nDotV = clamp(dot(normal, V), 1e-4, 1.0);

    float roughness = clamp(uMaterialFactors.x, 0.045, 1.0);
    float metalness = clamp(uMaterialFactors.y, 0.0, 1.0);

    vec3 albedo        = vColour.rgb;
    vec3 f0            = mix(vec3(0.04), albedo, metalness);
    vec3 diffuseAlbedo = albedo * (1.0 - metalness);

    /* CLAMPED, because this shader also runs inside a 128-pixel probe capture
     * where a 1x1 white stands in for the occlusion plane — see the longer note
     * in rhi/lit.fs.glsl. */
    ivec2 occlusionSize = textureSize(uOcclusion, 0);
    float ao = texelFetch(uOcclusion, min(ivec2(gl_FragCoord.xy), occlusionSize - 1), 0).r;

    vec3 shadow = nDotL > 0.0 ? sunShadow(vWorldPosition, normal, nDotL) : vec3(1.0);

    /* ---- the surface response, exactly as an opaque one --------------- */
    vec3 incoming = uSunRadiance.rgb * nDotL * shadow;
    SurfaceResponse sun =
        evaluateDirectional(normal, V, toSun, incoming, diffuseAlbedo, f0, roughness);

    vec3 ambientDiffuse = diffuseAlbedo * skyIrradiance(normal);

    /* THE PROBE, AND THIS IS THE SURFACE IT WAS BUILT FOR. Glass at roughness
     * 0.05 sits well below the 0.12..0.55 ramp where the term fades back to the
     * analytic sky, so a window shows its room's cubemap essentially at full
     * strength — which is the difference between a pane you can place in space
     * and a blue-grey rectangle. */
    vec3 reflection = normalize(mix(reflect(-V, normal), normal, roughness * roughness));
    vec3 ambientSpecular =
        environmentSpecular(reflection, vWorldPosition, normal, roughness,
                            uExposureAndAmbient.y)
        * environmentBRDF(f0, roughness, nDotV);

    vec3 ambientDiffuseLight  = ambientDiffuse * uExposureAndAmbient.y * ao;
    vec3 ambientSpecularLight = ambientSpecular * ao;

    /* ---- opacity ------------------------------------------------------
     *
     * THE FRESNEL RAMP. A dielectric reflects ~4% head-on and approaches 100%
     * edge-on, and since what is not reflected is transmitted, that curve IS
     * the opacity curve — a pane is nearly clear looking through it and turns
     * to a bright sheet at a grazing angle. Lifted from pbr.fs.glsl so the two
     * renderers ramp identically.
     *
     * uTint.w offsets N.V before the curve, which is how a material says "treat
     * me as thicker than I am"; uOpacity.w blends between the flat opacity and
     * the ramped one, so a material can opt out of the angle dependence
     * entirely without a branch. */
    float viewDotNormalInv = clamp(1.0 - (dot(V, normal) - uTint.w), 0.0001, 1.0);
    float fresnel = clamp(pow(viewDotNormalInv, uOpacity.y), 0.0, 1.0) * uOpacity.z;

    float plain    = uOpacity.x;
    float coverage = mix(plain, max(plain, fresnel), uOpacity.w);

    /* AND THE SURFACE'S OWN ALPHA ON TOP OF THE MATERIAL'S.
     *
     * vColour is the mesh's vertex colour times the object tint, alpha
     * included, and until now only its rgb was read. Glass is unaffected: the
     * world's vertices and every object tint carry alpha 1, so this multiplies
     * by one on every surface that existed before overlays did.
     *
     * WHAT IT IS FOR is geometry whose opacity varies WITHIN one mesh, which
     * no material parameter can express - a visibility field draws a
     * directly-seen cell more strongly than a peek-only one, and both are the
     * same material in the same draw. The alternative was one renderable per
     * grade, which is three meshes and three draws to say what one byte per
     * vertex already says. */
    coverage *= vColour.a;

    /* The tint the surface picks up at grazing angles, by the same curve. */
    vec3 edgeTint = mix(vec3(1.0), uTint.rgb, fresnel);

    /* ---- transmission -------------------------------------------------
     *
     * Light arriving through the surface from the FAR side, lit by the back
     * face's N.L. No 1/PI and no Fresnel: this is not a BRDF lobe, it is the
     * fraction of light the material passes, and the amount is authored rather
     * than derived. Zero for anything that has not asked for it, so an opaque
     * material drawn through this shader pays one multiply. */
    vec3 transmissive = vec3(0.0);
    if (uTransmission.w > 0.0) {
        float backNDotL = max(dot(-normal, toSun), 0.0);
        if (backNDotL > 0.0) {
            vec3 backShadow = sunShadow(vWorldPosition, -normal, backNDotL);
            transmissive = uSunRadiance.rgb * backShadow * uTransmission.rgb
                         * uTransmission.w * backNDotL;
        }
    }

    /* ---- premultiplied output ------------------------------------------
     *
     * ONLY THE DIFFUSE IS SCALED BY COVERAGE, and this is the whole reason the
     * BRDF returns its two halves separately.
     *
     * Ordinary alpha blending scales the entire fragment by opacity, specular
     * included — so a 6%-opaque pane keeps 6% of its sky reflection, and the
     * one cue that most says "this is glass and not a hole in the wall" is the
     * first thing thrown away. Keeping the specular and the sky at full
     * strength while the diffuse is scaled is what Alyx's membrane mode does,
     * and it is why the pass blends ONE / ONE_MINUS_SRC_ALPHA rather than
     * SRC_ALPHA / ONE_MINUS_SRC_ALPHA: the diffuse has already been scaled
     * here, so the blender must not scale it again.
     *
     * Transmission joins the SPECULAR side for the same reason — it is light
     * the surface itself sends toward the eye, not a modulation of what is
     * behind it, so scaling it by coverage would stop a nearly clear pane
     * glowing no matter how strongly it is lit from behind. */
    /* ---- THE DEV PANEL'S PER-TERM SWITCHES, THE SAME FOUR THE LIT PASS HAS
     *
     * AND THEY HAVE TO BE THE SAME, which is why `effectOn` is a function in
     * the shared block rather than a test written out in each file. Glass and
     * the wall behind it disagreeing about whether the sun is switched off is
     * not read as a debug switch being half-applied — it is read as a
     * transparency bug, and it is looked for in the blend state.
     *
     * `transmission` IS THIS SHADER'S ALONE, because it is the only surface
     * that has any: light arriving through a pane from behind it. Removing it
     * is what tells a bright window apart from a bright reflection IN a window,
     * which are the two explanations for the same pixel. */
    vec3 diffuse  = effectOn(kEffectDirectSun) ? sun.diffuse : vec3(0.0);
    vec3 specular = effectOn(kEffectDirectSun) ? sun.specular : vec3(0.0);

    if (effectOn(kEffectAmbientDiffuse))  diffuse  += ambientDiffuseLight;
    if (effectOn(kEffectAmbientSpecular)) specular += ambientSpecularLight;
    if (effectOn(kEffectTransmission))    specular += transmissive;

    /* EMISSION JOINS THE SPECULAR SIDE, which is to say the side that is NOT
     * scaled by coverage — the same place transmission goes and for the same
     * reason. Both are light the surface itself sends toward the eye rather
     * than a modulation of what is behind it, so scaling them by an 8% opacity
     * would stop a nearly clear pane glowing however brightly it was authored.
     * A backlit sign in a shop window is the case. */
    if (effectOn(kEffectEmissive)) specular += uEmissive.rgb;

    vec3 diffuseLight  = diffuse * edgeTint;
    vec3 specularLight = specular;

    outRadiance = vec4(diffuseLight * coverage + specularLight, coverage);
}
