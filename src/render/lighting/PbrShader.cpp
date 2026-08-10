#include "render/lighting/PbrShader.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

#include <cmath>
#include <vector>
#include "render/material/PbrMaterial.hpp"

namespace xcom {
namespace {

void setVector3(Shader shader, int location, Vector3 value)
{
    if (location < 0) return;
    const float packed[3] = { value.x, value.y, value.z };
    SetShaderValue(shader, location, packed, SHADER_UNIFORM_VEC3);
}

}  // namespace

PbrShader::~PbrShader()
{
    if (shader_.id) UnloadShader(shader_);
}

bool PbrShader::load()
{
    shader_ = ShaderLibrary::load("pbr.vs.glsl", "pbr.fs.glsl");
    if (shader_.id == 0) return false;

    locCameraPosition_      = GetShaderLocation(shader_, "uCameraPosition");
    locSunDirection_        = GetShaderLocation(shader_, "uSunDirection");
    locSunColour_           = GetShaderLocation(shader_, "uSunColour");
    locZenithColour_        = GetShaderLocation(shader_, "uZenithColour");
    locHorizonColour_       = GetShaderLocation(shader_, "uHorizonColour");
    locGroundColour_        = GetShaderLocation(shader_, "uGroundColour");
    locAmbientIntensity_    = GetShaderLocation(shader_, "uAmbientIntensity");
    locLightViewProjection_ = GetShaderLocation(shader_, "uLightViewProjection");
    locShadowTexel_         = GetShaderLocation(shader_, "uShadowTexel");
    locShadowWorldTexel_    = GetShaderLocation(shader_, "uShadowWorldTexel");
    locShadowDepthRange_    = GetShaderLocation(shader_, "uShadowDepthRange");
    locShadowSoftness_      = GetShaderLocation(shader_, "uShadowSoftness");
    locGlassTint_           = GetShaderLocation(shader_, "uGlassTint");
    locGlassClearPane_      = GetShaderLocation(shader_, "uGlassClearPane");
    locGlassParams_         = GetShaderLocation(shader_, "uGlassParams");
    locGlassEdge_           = GetShaderLocation(shader_, "uGlassEdge");
    locGlassRemap_          = GetShaderLocation(shader_, "uGlassRemap");
    locGlassGrime_          = GetShaderLocation(shader_, "uGlassGrime");
    locShadowStrength_      = GetShaderLocation(shader_, "uShadowStrength");
    locSceneSize_           = GetShaderLocation(shader_, "uSceneSize");
    locLightingSuppress_    = GetShaderLocation(shader_, "uLightingSuppress");
    if (locLightingSuppress_ < 0)
        TraceLog(LOG_WARNING, "PBR: no uLightingSuppress - the rendering "
                              "switches will do nothing");
    locMaterialFactors_     = GetShaderLocation(shader_, "uMaterialFactors");
    locMaterialOptions_     = GetShaderLocation(shader_, "uMaterialOptions");
    locMaterialTransmission_ = GetShaderLocation(shader_, "uTransmission");
    locLightmapParams_      = GetShaderLocation(shader_, "uLightmapParams");
    locLightmapGrid_        = GetShaderLocation(shader_, "uLightmapGrid");
    locLightmapIndexSize_   = GetShaderLocation(shader_, "uLightmapIndexSize");
    locUseLightmap_         = GetShaderLocation(shader_, "uUseLightmap");
    locFlatView_            = GetShaderLocation(shader_, "uFlatView");
    locProbeCount_          = GetShaderLocation(shader_, "uProbeCount");
    locProbeCapture_        = GetShaderLocation(shader_, "uProbeCapture");
    locProbeParallaxMin_    = GetShaderLocation(shader_, "uProbeParallaxMin");
    locProbeParallaxMax_    = GetShaderLocation(shader_, "uProbeParallaxMax");
    locProbeInfluenceMin_   = GetShaderLocation(shader_, "uProbeInfluenceMin");
    locProbeInfluenceMax_   = GetShaderLocation(shader_, "uProbeInfluenceMax");

    /* MATERIAL MAPS reach the shader through the map array: DrawMesh binds
     * maps[i] to unit i and sets locs[SHADER_LOC_MAP_DIFFUSE+i], so each
     * slot's sampler uniform is registered here by name. */
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapAlbedo]       = GetShaderLocation(shader_, "uAlbedoMap");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapPacked]       = GetShaderLocation(shader_, "uMraoMap");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapNormal]       = GetShaderLocation(shader_, "uNormalMap");
    shader_.locs[SHADER_LOC_MAP_DIFFUSE + kMapTranslucency] = GetShaderLocation(shader_, "uTranslucencyMap");

    /* FRAME BUFFERS are pointed at fixed units instead, once, here. A sampler
     * uniform is just an int naming a texture unit, and these never move — so
     * the per-draw work is a bind rather than a bind plus a uniform write, and
     * they stop occupying material slots they were never really part of.
     *
     * Forgetting one of these is invisible: the sampler stays at unit 0, reads
     * whatever the albedo happens to be, and the effect quietly does nothing.
     * That has already cost one full debugging session on the lightmap index
     * and another on the transmission plane. */
    bindSamplerUnit("uShadowMap",          kUnitShadow);
    bindSamplerUnit("uAmbientOcclusion",   kUnitOcclusion);
    bindSamplerUnit("uLightmap",           kUnitLightmap);
    bindSamplerUnit("uLightmapIndex",      kUnitLightIndex);
    bindSamplerUnit("uShadowTransmission", kUnitTransmission);
    bindSamplerUnit("uEnvironmentMap",     kUnitEnvironment);
    bindSamplerUnit("uDBufferAlbedo",      kUnitDecalAlbedo);
    bindSamplerUnit("uDBufferNormal",      kUnitDecalNormal);
    bindSamplerUnit("uDBufferSurface",     kUnitDecalSurface);

    locDecalsEnabled_       = GetShaderLocation(shader_, "uDecalsEnabled");
    locDecalEmissiveScale_  = GetShaderLocation(shader_, "uDecalEmissiveScale");

    /* The default has to be pushed rather than left at GLSL's implicit zero:
     * an unset uniform reads as 0 and 0 means "decals off", so a caller that
     * never touches the switch would silently get no decals at all. Same
     * polarity trap uLightingSuppress documents, solved the same way — by
     * making the safe value the one that is actually written. */
    setDecalsEnabled(true);
    setDecalEmissiveScale(kDefaultDecalEmissiveScale);

    return true;
}

void PbrShader::setDecalsEnabled(bool enabled) const
{
    const float value = enabled ? 1.0f : 0.0f;
    SetShaderValue(shader_, locDecalsEnabled_, &value, SHADER_UNIFORM_FLOAT);
}

void PbrShader::setDecalEmissiveScale(float radiance) const
{
    SetShaderValue(shader_, locDecalEmissiveScale_, &radiance, SHADER_UNIFORM_FLOAT);
}

void PbrShader::bindDecalBuffer(const DecalBuffer& decals) const
{
    const auto bind = [](int unit, Texture2D texture) {
        rlActiveTextureSlot(unit);
        rlEnableTexture(texture.id);
    };

    /* An invalid buffer hands back its 1x1 (0,0,0,255) stand-in, which decodes
     * to "no decal here" in all three planes — so this is safe to call whether
     * or not the decal system came up, and the lit shader never branches on it.
     * See DecalBuffer.hpp. */
    bind(kUnitDecalAlbedo,  decals.albedo());
    bind(kUnitDecalNormal,  decals.normal());
    bind(kUnitDecalSurface, decals.surface());

    rlActiveTextureSlot(0);
}

void PbrShader::setEnvironmentProbes(const ReflectionProbeSet& probes) const
{
    const int count = probes.valid() ? probes.probeCount() : 0;
    SetShaderValue(shader_, locProbeCount_, &count, SHADER_UNIFORM_INT);

    if (count <= 0) {
        /* No probes is not a failure — it is the analytic sky, and the shader
         * takes that branch on uProbeCount alone. Nothing else needs pushing:
         * a volume array nobody loops over cannot be read. */
        ReflectionProbeSet::unbindFrom(kUnitEnvironment);
        return;
    }

    /* ONE UPLOAD PER ARRAY, NOT ONE PER PROBE. SetShaderValueV writes a
     * contiguous run of a uniform array in a single glUniform4fv, so the
     * staging vectors here exist to make the data contiguous — pushing
     * uProbeCapture[i] one at a time would be five GL calls per room per
     * frame for data that changes when a wall comes down. */
    std::vector<float> capture(static_cast<std::size_t>(count) * 4);
    std::vector<float> parallaxMin(static_cast<std::size_t>(count) * 4);
    std::vector<float> parallaxMax(static_cast<std::size_t>(count) * 4);
    std::vector<float> influenceMin(static_cast<std::size_t>(count) * 4);
    std::vector<float> influenceMax(static_cast<std::size_t>(count) * 4);

    for (int i = 0; i < count; i++) {
        const ProbeVolume& probe = probes.probes()[static_cast<std::size_t>(i)];
        const std::size_t at = static_cast<std::size_t>(i) * 4;

        /* THE W CHANNELS ARE NOT PADDING. Transition rides with the capture
         * point and priority rides with the parallax minimum, because a vec4
         * array costs the same uniform space as a vec3 one — std140 pads a
         * vec3 to four floats regardless — so two extra scalars are free here
         * and would each cost their own array otherwise. */
        capture[at + 0] = probe.capture.x;
        capture[at + 1] = probe.capture.y;
        capture[at + 2] = probe.capture.z;
        capture[at + 3] = probe.transition;

        parallaxMin[at + 0] = probe.parallaxMin.x;
        parallaxMin[at + 1] = probe.parallaxMin.y;
        parallaxMin[at + 2] = probe.parallaxMin.z;
        parallaxMin[at + 3] = probe.priority;

        parallaxMax[at + 0] = probe.parallaxMax.x;
        parallaxMax[at + 1] = probe.parallaxMax.y;
        parallaxMax[at + 2] = probe.parallaxMax.z;
        parallaxMax[at + 3] = 0.0f;

        influenceMin[at + 0] = probe.influenceMin.x;
        influenceMin[at + 1] = probe.influenceMin.y;
        influenceMin[at + 2] = probe.influenceMin.z;
        influenceMin[at + 3] = 0.0f;

        influenceMax[at + 0] = probe.influenceMax.x;
        influenceMax[at + 1] = probe.influenceMax.y;
        influenceMax[at + 2] = probe.influenceMax.z;
        influenceMax[at + 3] = 0.0f;
    }

    SetShaderValueV(shader_, locProbeCapture_,      capture.data(),
                    SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(shader_, locProbeParallaxMin_,  parallaxMin.data(),
                    SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(shader_, locProbeParallaxMax_,  parallaxMax.data(),
                    SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(shader_, locProbeInfluenceMin_, influenceMin.data(),
                    SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(shader_, locProbeInfluenceMax_, influenceMax.data(),
                    SHADER_UNIFORM_VEC4, count);

    probes.bindTo(kUnitEnvironment);
}

void PbrShader::clearEnvironmentProbes() const
{
    /* Zero probes sends the shader to the analytic sky, and the unit is
     * unbound so no array is left dangling there. */
    const int none = 0;
    SetShaderValue(shader_, locProbeCount_, &none, SHADER_UNIFORM_INT);
    ReflectionProbeSet::unbindFrom(kUnitEnvironment);
}

void PbrShader::bindSamplerUnit(const char* name, int unit) const
{
    const int location = GetShaderLocation(shader_, name);
    if (location < 0) {
        TraceLog(LOG_WARNING, "PBR: no sampler named %s - it will read unit 0", name);
        return;
    }
    SetShaderValue(shader_, location, &unit, SHADER_UNIFORM_INT);
}

void PbrShader::bindFrameTextures(Texture2D shadowMap, Texture2D occlusion,
                                  Texture2D lightmap, Texture2D lightmapIndex,
                                  Texture2D transmission) const
{
    const auto bind = [](int unit, Texture2D texture) {
        rlActiveTextureSlot(unit);
        rlEnableTexture(texture.id);   /* id 0 unbinds, which reads as black */
    };

    bind(kUnitShadow,       shadowMap);
    bind(kUnitOcclusion,    occlusion);
    bind(kUnitLightmap,     lightmap);
    bind(kUnitLightIndex,   lightmapIndex);
    bind(kUnitTransmission, transmission);

    /* Leave the selector where the rest of the frame expects it. rlgl's batch
     * and DrawMesh both set the slot before binding, but neither restores it,
     * and a stray active unit is the kind of state bug that shows up three
     * passes later. */
    rlActiveTextureSlot(0);
}

void PbrShader::updateEnvironment(const SunLight& sun, Vector3 cameraPosition)
{
    setVector3(shader_, locCameraPosition_, cameraPosition);
    setVector3(shader_, locSunDirection_,   sun.travelDirection());
    setVector3(shader_, locSunColour_,      sun.radiance());
    setVector3(shader_, locZenithColour_,   sun.zenithColour());
    setVector3(shader_, locHorizonColour_,  sun.horizonColour());
    setVector3(shader_, locGroundColour_,   sun.groundColour());

    const float ambient = sun.ambientIntensity();
    SetShaderValue(shader_, locAmbientIntensity_, &ambient, SHADER_UNIFORM_FLOAT);

    /* Without a shadow map the sun still shades — it just stops occluding.
     * Losing shadows is a downgrade; losing the whole lit pass is a black
     * screen. */
    const float strength = 0.0f;
    SetShaderValue(shader_, locShadowStrength_, &strength, SHADER_UNIFORM_FLOAT);
}

void PbrShader::updateEnvironment(const SunLight& sun, const ShadowMap& shadows,
                                  Vector3 cameraPosition)
{
    updateEnvironment(sun, cameraPosition);
    if (!shadows.valid()) return;

    const float strength = 1.0f;
    SetShaderValue(shader_, locShadowStrength_, &strength, SHADER_UNIFORM_FLOAT);

    SetShaderValueMatrix(shader_, locLightViewProjection_, shadows.viewProjection());

    const Vector2 texel = shadows.texelSize();
    const float packed[2] = { texel.x, texel.y };
    SetShaderValue(shader_, locShadowTexel_, packed, SHADER_UNIFORM_VEC2);

    const float worldTexel = shadows.worldTexelSize();
    SetShaderValue(shader_, locShadowWorldTexel_, &worldTexel, SHADER_UNIFORM_FLOAT);

    const float depthRange = shadows.depthRange();
    SetShaderValue(shader_, locShadowDepthRange_, &depthRange, SHADER_UNIFORM_FLOAT);

    /* Tangent precomputed here rather than per fragment, and the search radius
     * capped: the blocker search is twelve samples spread over this many
     * texels, so letting it grow without bound makes the estimate sparse and
     * noisy rather than soft.
     *
     * 48 rather than 24 because a three-storey wall at a low sun genuinely
     * throws a penumbra that wide, and clamping there flattened it back to a
     * constant blur — the exact failure PCSS exists to remove. The shader
     * doubles its sample count past 8 texels to keep the wider disc covered.
     * A sun near the horizon can still exceed even this; see the note in
     * study/source2_rendering.md. */
    const float softness[2] = { std::tan(sun.angularRadius()), 48.0f };
    SetShaderValue(shader_, locShadowSoftness_, softness, SHADER_UNIFORM_VEC2);
}

void PbrShader::setGlassTransmission(Vector4 transmission) const
{
    /* One tint for all glass — the transmission plane is a single channel, so
     * per-pane colour would need it to be RGB. The dimming, though, IS per
     * texel: it is the channel's whole content. */
    const float tint[3] = { transmission.x, transmission.y, transmission.z };
    SetShaderValue(shader_, locGlassTint_, tint, SHADER_UNIFORM_VEC3);

    const float clearPane = transmission.w;
    SetShaderValue(shader_, locGlassClearPane_, &clearPane, SHADER_UNIFORM_FLOAT);
}

void PbrShader::setSceneSize(float width, float height)
{
    const float packed[2] = { width, height };
    SetShaderValue(shader_, locSceneSize_, packed, SHADER_UNIFORM_VEC2);
}

void PbrShader::setLightingSuppress(int mask) const
{
    if (locLightingSuppress_ < 0) return;
    SetShaderValue(shader_, locLightingSuppress_, &mask, SHADER_UNIFORM_INT);
}

void PbrShader::setMaterialFactors(Vector4 factors) const
{
    const float packed[4] = { factors.x, factors.y, factors.z, factors.w };
    SetShaderValue(shader_, locMaterialFactors_, packed, SHADER_UNIFORM_VEC4);
}

void PbrShader::setLightmapLayout(Vector4 params, Vector4 grid, Vector2 indexSize)
{
    const float packedParams[4] = { params.x, params.y, params.z, params.w };
    const float packedGrid[4]   = { grid.x, grid.y, grid.z, grid.w };
    const float packedIndex[2]  = { indexSize.x, indexSize.y };
    SetShaderValue(shader_, locLightmapParams_,    packedParams, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locLightmapGrid_,      packedGrid,   SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locLightmapIndexSize_, packedIndex,  SHADER_UNIFORM_VEC2);
}

void PbrShader::setLightmapEnabled(bool enabled) const
{
    const float value = enabled ? 1.0f : 0.0f;
    SetShaderValue(shader_, locUseLightmap_, &value, SHADER_UNIFORM_FLOAT);
}

void PbrShader::setShadowsEnabled(bool enabled) const
{
    const float value = enabled ? 1.0f : 0.0f;
    SetShaderValue(shader_, locShadowStrength_, &value, SHADER_UNIFORM_FLOAT);
}

void PbrShader::setDebugView(int mode) const
{
    const float value = static_cast<float>(mode);
    SetShaderValue(shader_, locFlatView_, &value, SHADER_UNIFORM_FLOAT);
}

void PbrShader::setGlass(Vector4 params, Vector4 edge, Vector2 remap, Vector4 grime) const
{
    const float packedParams[4] = { params.x, params.y, params.z, params.w };
    const float packedEdge[4]   = { edge.x, edge.y, edge.z, edge.w };
    const float packedRemap[2]  = { remap.x, remap.y };
    const float packedGrime[4]  = { grime.x, grime.y, grime.z, grime.w };
    SetShaderValue(shader_, locGlassParams_, packedParams, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locGlassEdge_,   packedEdge,   SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, locGlassRemap_,  packedRemap,  SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, locGlassGrime_,  packedGrime,  SHADER_UNIFORM_VEC4);
}

void PbrShader::setMaterialTransmission(Vector4 transmission) const
{
    const float packed[4] = { transmission.x, transmission.y, transmission.z,
                              transmission.w };
    SetShaderValue(shader_, locMaterialTransmission_, packed, SHADER_UNIFORM_VEC4);
}

void PbrShader::setMaterialOptions(Vector4 options) const
{
    const float packed[4] = { options.x, options.y, options.z, options.w };
    SetShaderValue(shader_, locMaterialOptions_, packed, SHADER_UNIFORM_VEC4);
}

}  // namespace xcom
