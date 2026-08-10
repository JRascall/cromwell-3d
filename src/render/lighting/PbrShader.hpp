/* PbrShader.hpp — the lit surface program and its uniforms.
 *
 * SINGLE RESPONSIBILITY: own the shader, know where its uniforms live, and
 * push the lighting environment and the current material's scalars into them.
 *
 * IT OWNS NO MATERIAL. Materials belong to MaterialLibrary, one per
 * SurfaceKind, each holding its own texture set and all bound to this one
 * program. There is a single shader for the whole world — textured or not,
 * dielectric or metal — because every map has a 1x1 fallback and every scalar
 * multiplies its map. No permutations, no branches, no #ifdefs.
 *
 * TEXTURES REACH THE SHADER THROUGH MATERIAL MAP SLOTS, never through
 * SetShaderValueTexture. That call registers a sampler with rlgl's batch, and
 * the batch is what binds it — but DrawMesh does not go through the batch, it
 * binds material.maps[i] and nothing else. Pointing each SHADER_LOC_MAP_* at
 * our own uniform name and parking the texture in the matching slot is what
 * makes the binding actually happen. See PbrMaterial.hpp for the slot map.
 */
#pragma once

#include "raylib.h"

#include "render/decal/DecalBuffer.hpp"
#include "render/lighting/ReflectionProbeSet.hpp"
#include "render/lighting/ShadowMap.hpp"
#include "render/lighting/SunLight.hpp"

namespace xcom {

class PbrShader {
public:
    /* WHAT A FULLY SELF-LIT DECAL IS WORTH, in the linear radiance the shader
     * outputs. The emissive mask is one 8-bit channel and the output is HDR, so
     * the brightness cannot live in the buffer — this is the multiplier that
     * turns a saturated mask into a decal that reads as GLOWING after the
     * tonemap rather than merely pale. Sits a little above the radiance of a
     * sunlit white wall, which is what makes it survive the exposure. */
    static constexpr float kDefaultDecalEmissiveScale = 6.0f;

    PbrShader() = default;
    ~PbrShader();

    PbrShader(const PbrShader&) = delete;
    PbrShader& operator=(const PbrShader&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0; }

    /* The program every material in the library is bound to. */
    Shader shader() const { return shader_; }

    /* Everything that moves: the sun, the shadow matrix, the eye. Call once
     * per frame before drawing anything lit. */
    void updateEnvironment(const SunLight& sun, const ShadowMap& shadows,
                           Vector3 cameraPosition);

    /* The same, for a pass that has no shadow map at all — the model preview's
     * studio, where there is no ground for a shadow to land on and a 4096
     * square depth target to render one into would be absurd. Shadow strength
     * goes to zero, so the sun still shades and simply stops occluding.
     *
     * Its own entry point rather than a null ShadowMap& because the caller
     * genuinely has no shadow map to name, and a dead one held as a member
     * purely to have something to pass is a worse lie than an overload. */
    void updateEnvironment(const SunLight& sun, Vector3 cameraPosition);

    /* The frame's shared buffers, bound to fixed high texture units — NOT
     * through any material's map array. They are identical for every draw in
     * the frame, so they are bound once here rather than copied into a dozen
     * materials, and that keeps the material slots for actual materials. See
     * PbrMaterial.hpp's unit block.
     *
     * Call once per frame, after the shadow map and SSAO have been rendered
     * and before any lit geometry. */
    void bindFrameTextures(Texture2D shadowMap, Texture2D occlusion,
                           Texture2D lightmap, Texture2D lightmapIndex,
                           Texture2D transmission) const;

    /* The three DBuffer planes, to their own fixed units. Its own entry point
     * rather than three more arguments to bindFrameTextures because the decal
     * pass runs later than the shadow map and the occlusion buffer do, and
     * binding a target that is still being written to is the kind of thing that
     * works until a driver decides otherwise.
     *
     * SAFE WITH AN INVALID BUFFER. DecalBuffer hands back a 1x1 stand-in that
     * decodes to "no decal", so the lit shader has no branch for the system
     * being absent. */
    void bindDecalBuffer(const DecalBuffer& decals) const;

    /* Turns decals off without touching the buffer, which is what a dev-view
     * layer switch wants: skipping the decal PASS alone would leave the last
     * frame's planes bound and still sampled, freezing the decals rather than
     * removing them. Same failure the reflection-probe switch had. */
    void setDecalsEnabled(bool enabled) const;

    /* See kDefaultDecalEmissiveScale. Pushed at load; a setter exists so the
     * dev panel can find the value rather than the value being guessed once. */
    void setDecalEmissiveScale(float radiance) const;

    /* Every room's probe, and the volumes the shader picks between them with.
     * Binds the cubemap ARRAY to its own texture unit as a side effect, so
     * call it once a frame alongside bindFrameTextures — and AFTER any
     * capture, since capturing rebinds the framebuffer. */
    void setEnvironmentProbes(const ReflectionProbeSet& probes) const;

    /* Says there are NO probes, so the shader reflects the analytic sky alone.
     * What a preview wants: a probe is parallax-corrected against a room's
     * bounding box, and an object floating in a UI panel is in no room at all
     * — reflecting the world's cubemap there would paint a wall onto a rifle. */
    void clearEnvironmentProbes() const;

    /* The size of the target being shaded, which is NOT the window: the scene
     * is supersampled, and the occlusion buffer is not. The shader needs it to
     * turn its pixel position into a lookup into that buffer. */
    void setSceneSize(float width, float height);

    /* Which lighting terms are SUPPRESSED — see RenderEffects.hpp for the bit
     * values and for why the polarity is "off" rather than "on". Frame state,
     * not material state: push it before anything lit is drawn.
     *
     * Terms are removed rather than faded. A term scaled to near-zero still
     * hides what is underneath it, which is exactly what a switch is for
     * finding out. */
    void setLightingSuppress(int mask) const;

    /* (roughness, metalness, normalStrength, uvScale) — one uniform rather
     * than four calls, pushed immediately before the draw it applies to.
     * Const because it changes GPU state, not this object. */
    void setMaterialFactors(Vector4 factors) const;

    /* (channelPacking, reserved...) — which channels of the packed map hold
     * metalness and occlusion. Authored art here is MRAO; anything imported
     * from a glTF is ORM. See PbrMaterial.hpp. */
    void setMaterialOptions(Vector4 options) const;

    /* (transmissionColour.rgb, transmissionAmount) — light reaching the eye
     * through the surface from behind. Must be pushed wherever the factors
     * are, or the last material's value leaks onto the next draw. */
    void setMaterialTransmission(Vector4 transmission) const;

    /* Only read when the material is Blend. See PbrMaterial's glass block.
     * `grime` is (colour.rgb, roughness) — the layer the translucency map is
     * the opacity of. */
    void setGlass(Vector4 params, Vector4 edge, Vector2 remap, Vector4 grime) const;

    /* (transmissionTint.rgb, cleanPaneTransmittance) — how the shadow map's
     * colour plane is decoded. Frame-invariant, so it is set once; it has to
     * agree with what ShadowMap's transmitter writes or the tint lands at the
     * wrong strength. */
    void setGlassTransmission(Vector4 transmission) const;

    /* The baked lightmap's layout, so the shader can turn a world position and
     * a surface normal into an atlas lookup — see SunLightmapLayout for why
     * that beats a second UV channel.
     *   params = (texelsPerTile, pagesPerRow, atlasWidth, atlasHeight)
     *   grid   = (gridWidth, gridHeight, gridDepth, cellHeight) */
    void setLightmapLayout(Vector4 params, Vector4 grid, Vector2 indexSize);

    /* 1 for the baked lattice, 0 for units and props — they move, or carry no
     * lightmap UVs, so they stay on the shadow map. This is Source 2's split:
     * mesh entities are never lightmapped. */
    void setLightmapEnabled(bool enabled) const;

    /* Turns the shadow term off without touching the map itself, which is what
     * a dev-view layer switch wants: skipping the shadow PASS alone would
     * leave the last frame's depth bound and shade the world with a shadow
     * that no longer moves. Push it after updateEnvironment, which sets the
     * same uniform from whether the map loaded at all. */
    void setShadowsEnabled(bool enabled) const;

    /* Diagnostic views, each answering a question the lit image cannot:
     *   0  off
     *   1  geometry only — flat facing ratio, glass solid. "Is there a surface
     *      here at all", which is otherwise indistinguishable from a hole in
     *      the lighting.
     *   2  the reflection probes — an ORDINARY frame, with a chrome ball
     *      drawn at each capture point by ProbeSpheres. Nothing for this
     *      shader to do; the balls need a real scene to sit in, which is why
     *      the flat-view test for 1 is ranged rather than open-ended.
     *   4  roughness — the value the surface is ACTUALLY shaded with, after
     *      the mrao map, the material factor and the grime layer have all had
     *      their say. What MaterialLibrary lists is what a material was
     *      authored as, and the two disagreeing is a bug this is the only way
     *      to see. Banded green across the probe fade range.
     *   3  probe assignment — a flat colour per room, so which probe owns a
     *      surface is visible directly. View 2 cannot answer that: a
     *      reflection sourced from the WRONG room still looks like a
     *      reflection, which is precisely how the leak went unnoticed. Here a
     *      wall reads as two colours if its faces belong to two rooms and one
     *      colour if it leaks. Magenta is "no probe claims this". */
    void setDebugView(int mode) const;

private:
    /* Points a sampler uniform at a fixed texture unit, once. */
    void bindSamplerUnit(const char* name, int unit) const;

    Shader shader_ = { 0 };

    int locCameraPosition_ = -1;
    int locSunDirection_ = -1;
    int locSunColour_ = -1;
    int locZenithColour_ = -1;
    int locHorizonColour_ = -1;
    int locGroundColour_ = -1;
    int locAmbientIntensity_ = -1;
    int locLightViewProjection_ = -1;
    int locShadowTexel_ = -1;
    int locShadowWorldTexel_ = -1;
    int locShadowDepthRange_ = -1;
    int locShadowSoftness_ = -1;
    int locGlassTint_ = -1;
    int locGlassClearPane_ = -1;
    int locGlassParams_ = -1;
    int locGlassEdge_ = -1;
    int locGlassRemap_ = -1;
    int locGlassGrime_ = -1;
    int locShadowStrength_ = -1;
    int locSceneSize_ = -1;
    int locLightingSuppress_ = -1;
    int locMaterialFactors_ = -1;
    int locMaterialOptions_ = -1;
    int locMaterialTransmission_ = -1;
    int locLightmapParams_ = -1;
    int locLightmapGrid_ = -1;
    int locLightmapIndexSize_ = -1;
    int locUseLightmap_ = -1;
    int locFlatView_ = -1;
    int locProbeCount_ = -1;
    int locProbeCapture_ = -1;
    int locProbeParallaxMin_ = -1;
    int locProbeParallaxMax_ = -1;
    int locProbeInfluenceMin_ = -1;
    int locProbeInfluenceMax_ = -1;
    int locDecalsEnabled_ = -1;
    int locDecalEmissiveScale_ = -1;
};

}  // namespace xcom
