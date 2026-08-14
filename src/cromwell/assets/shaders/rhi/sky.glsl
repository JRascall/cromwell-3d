/* rhi/sky.glsl — the two-lobe sky, as an irradiance.
 *
 * The same function common/environment.glsl gives the raylib path, reading the
 * scene block's colours instead of loose uniforms. That file cannot be shared
 * with this dialect because it declares `uniform vec3` globals, which
 * CONVENTIONS.md forbids and SPIR-V has no concept of; this is the four lines
 * of it that matter, against a std140 block.
 *
 * ONE SKY, TWO CONSUMERS — and now three. What the sky pass DRAWS behind the
 * geometry, what an opaque surface takes as ambient, and what a transparent one
 * reflects are all this function, so a backdrop can never disagree with the
 * light in front of it.
 *
 * Requires rhi/scene_block.glsl.
 */
#ifndef XCOM_RHI_SKY
#define XCOM_RHI_SKY

/* Sampling by the NORMAL is the usual hemisphere-ambient shortcut: a cosine
 * lobe's worth of sky, without integrating one. Sampling by the REFLECTION
 * vector instead is what turns it into an environment reflection, which is why
 * one function serves both. */
vec3 skyIrradiance(vec3 direction)
{
    float up = direction.y;
    vec3 sky = mix(uSkyHorizon.rgb, uSkyZenith.rgb, clamp(up, 0.0, 1.0));

    /* NOT BLACK BELOW. A surface facing straight down still sees light bounced
     * off the ground, and zeroing it is the single most common reason
     * untextured geometry reads as plastic — every underside becomes a hole. */
    return mix(uSkyGround.rgb, sky, smoothstep(-0.3, 0.2, up));
}

#endif
