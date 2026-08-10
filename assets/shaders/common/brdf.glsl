/* common/brdf.glsl - the surface response, shared by every lit family.
 *
 * Cook-Torrance specular with a GGX distribution and a Smith height-correlated
 * visibility term, over a metal/rough parameterisation - the same model Source
 * 2 shades with. Glass, water and opaque surfaces all want THIS, unchanged;
 * what differs between them is how they arrive at albedo, roughness and
 * opacity, not how those turn into light.
 *
 * PURE FUNCTIONS ONLY. No uniforms, no samplers, no varyings - so this file
 * can be included by a fragment stage that has never heard of a shadow map.
 */
#ifndef XCOM_COMMON_BRDF
#define XCOM_COMMON_BRDF

#include "common/colour.glsl"

float distributionGGX(float nDotH, float alpha)
{
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-7);
}

/* Smith height-correlated, already folded together with the 1/(4 NoL NoV)
 * denominator - hence "visibility" rather than "geometry". */
float visibilitySmith(float nDotV, float nDotL, float alpha)
{
    float a2 = alpha * alpha;
    float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

vec3 fresnelSchlick(vec3 f0, float cosine)
{
    return f0 + (vec3(1.0) - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

/* Karis' analytic fit to the split-sum environment BRDF. Stands in for the
 * lookup table a real IBL would sample. */
vec3 environmentBRDF(vec3 f0, float roughness, float nDotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.040);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

/* One directional light against one surface, returning diffuse and specular
 * SEPARATELY rather than summed.
 *
 * They have to stay apart because a transparent surface scales them
 * differently: opacity multiplies the diffuse and must not touch the
 * specular, or a nearly clear pane keeps only a few percent of its sun glint
 * and stops reading as glass. See the premultiplied output in pbr.fs.glsl. */
struct SurfaceResponse {
    vec3 diffuse;
    vec3 specular;
};

SurfaceResponse evaluateDirectional(vec3 N, vec3 V, vec3 L, vec3 incoming,
                                    vec3 diffuseAlbedo, vec3 f0, float roughness)
{
    SurfaceResponse response;
    response.diffuse  = vec3(0.0);
    response.specular = vec3(0.0);

    float nDotL = clamp(dot(N, L), 0.0, 1.0);
    if (nDotL <= 0.0) return response;

    float alpha = roughness * roughness;
    float nDotV = clamp(dot(N, V), 1e-4, 1.0);

    vec3  H     = normalize(L + V);
    float nDotH = clamp(dot(N, H), 0.0, 1.0);
    float vDotH = clamp(dot(V, H), 0.0, 1.0);

    vec3 F = fresnelSchlick(f0, vDotH);

    response.specular = F * (distributionGGX(nDotH, alpha) *
                             visibilitySmith(nDotV, nDotL, alpha)) * incoming;
    response.diffuse  = diffuseAlbedo * (vec3(1.0) - F) / kPi * incoming;
    return response;
}

#endif
