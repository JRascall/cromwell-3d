/* rhi/scene_block.glsl — the frame, as every scene shader sees it.
 *
 * ONE DECLARATION, INCLUDED, rather than the same block retyped in each stage.
 *
 * A uniform block is matched across stages BY NAME, so a vertex shader and a
 * fragment shader that declare `PassBlock` differently do not warn — they fail
 * to link, with a message about an interface mismatch some distance from the
 * edit that caused it. Two shaders that declare it differently is worse: they
 * both link, and one of them reads every member past the divergence at the
 * wrong offset.
 *
 * That had already happened twice here — adding the sky colours and then the
 * shadow scales each meant editing the same list in two files, and each was
 * caught by a link error rather than by review. This is the fix.
 *
 * THE C++ HALF IS LitBlockData in ScenePipeline.cpp, and the two are one
 * contract written twice. There is no reflection on the explicit backends to
 * check them, so the static_assert there and this file are what keep them
 * honest. Add to the END of both, together.
 */
#ifndef XCOM_RHI_SCENE_BLOCK
#define XCOM_RHI_SCENE_BLOCK

layout(std140, binding = 1) uniform PassBlock {
    mat4 uViewProjection;
    mat4 uSunViewProjection;

    vec4 uSunDirection;        /* xyz = the direction light TRAVELS */
    vec4 uCameraPosition;
    vec4 uSunRadiance;         /* linear, and far brighter than one */
    vec4 uSkyZenith;
    vec4 uSkyHorizon;
    vec4 uSkyGround;
    vec4 uExposureAndAmbient;  /* x = the resolve's exposure, y = ambient */

    /* x texel in UV, y texel in world units, z depth range in world units,
     * w tan(sun angular radius). Every one is a unit conversion a shader
     * cannot do for itself — see LitBlockData. */
    vec4 uShadowScales;

    /* x = HOW MANY REFLECTION PROBES ARE LIVE, and it is a per-PASS number
     * rather than a per-frame one — which is why it sits here, in the block the
     * pass uploads, and not in the probe block beside the volumes themselves.
     *
     * A PROBE CAPTURE SETS IT TO ZERO. The capture renders into a slice of the
     * very cubemap array the probes are sampled from, and a texture that is
     * simultaneously a colour attachment and a live sampler read is undefined
     * on every backend. It would also be wrong if it worked: a probe that
     * reflected the probes would compound its own error on every sweep, and
     * the second bounce would arrive a second later than the first.
     *
     * yzw spare. */
    vec4 uProbeParams;

    /* x = WHICH LIGHTING TERMS ARE SUPPRESSED — RenderEffects' bits, carried as
     * a float because every other member here is one and a mixed block is one
     * more way for the two halves of this contract to disagree quietly. Five
     * bits, so the conversion is exact.
     *
     * A SUPPRESSION MASK: a set bit REMOVES a term, and zero is the ordinary
     * image. Written that way round because a uniform that never arrives reads
     * as zero, and the alternative polarity would make any such failure black
     * the scene out — see SceneFrame::effectSuppress. yzw spare. */
    vec4 uEffectMask;

    /* x = 1 WHEN THE DECALS ARE ON, 0 when the dev panel's layer switch is off.
     * Read by the surface shaders as well as gating the pass, because a pipeline
     * binds the same slots every frame — skipping the pass alone would leave the
     * last frame's planes bound and still sampled, so "off" would FREEZE the
     * decals rather than remove them. rhi/dbuffer.glsl carries the argument.
     *
     * y = WHAT A FULL-STRENGTH EMISSIVE MASK IS WORTH in linear radiance. The
     * mask is one 8-bit channel and the surface shaders output HDR, so the
     * scale cannot live in the buffer — the same reason the sun is brighter
     * than a wall. zw spare. */
    vec4 uDecalParams;
};

/* WHETHER A TERM SURVIVES. `effectBit` is one of RenderEffects::Bit's values;
 * the shader tests, it does not know what the bit is called.
 *
 * A FUNCTION RATHER THAN THE TEST WRITTEN OUT, because it appears in both
 * surface shaders and the two must agree — a lit wall and the pane of glass in
 * front of it disagreeing about whether the sun is switched off is exactly the
 * kind of half-applied debug switch that gets read as a transparency bug. */
bool effectOn(int effectBit)
{
    return (int(uEffectMask.x + 0.5) & effectBit) == 0;
}

/* THE BIT VALUES, mirroring RenderEffects::Bit. One list in two languages, like
 * the block above it — add to both, together. */
const int kEffectDirectSun       = 1;
const int kEffectAmbientDiffuse  = 2;
const int kEffectAmbientSpecular = 4;
const int kEffectBakedOcclusion  = 8;
const int kEffectTransmission    = 16;
const int kEffectEmissive        = 32;

#endif
