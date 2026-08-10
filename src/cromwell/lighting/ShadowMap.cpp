#include "cromwell/lighting/ShadowMap.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "cromwell/gpu/ShaderLibrary.hpp"

namespace cromwell {

ShadowMap::~ShadowMap()
{
    if (shader_.id) UnloadShader(shader_);
    if (glassShader_.id) UnloadShader(glassShader_);
}

bool ShadowMap::load()
{
    shader_ = ShaderLibrary::load("depth_only.vs.glsl", "depth_only.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "SHADOW: no depth shader - the sun will not cast");
        return false;
    }

    material_ = LoadMaterialDefault();
    material_.shader = shader_;

    /* Its own vertex stage rather than the depth-only one, because the grime
     * on a pane decides how much light gets past it, and grime is a texture:
     * this pass needs UVs. */
    glassShader_ = ShaderLibrary::load("glass_shadow.vs.glsl", "glass_shadow.fs.glsl");
    if (glassShader_.id != 0) {
        glassMaterial_ = LoadMaterialDefault();
        glassMaterial_.shader = glassShader_;

        /* Slot 0 rather than SetShaderValueTexture: DrawMesh binds maps[i] and
         * ignores anything registered with rlgl's batch. See PbrShader.hpp. */
        glassShader_.locs[SHADER_LOC_MAP_DIFFUSE] =
            GetShaderLocation(glassShader_, "uTranslucencyMap");
        locGlassTransmit_ = GetShaderLocation(glassShader_, "uGlassTransmit");
    } else {
        TraceLog(LOG_WARNING, "SHADOW: no glass shader - windows will not tint");
        glassMaterial_ = material_;
    }

    /* R8: this pass writes depth and nothing else, and the colour attachment
     * only exists because OpenGL will not call a colour-less framebuffer
     * complete without a draw-buffer setting rlgl cannot express. */
    target_.resize(kResolution, kResolution, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
    if (!target_.valid()) {
        TraceLog(LOG_WARNING, "SHADOW: could not allocate the depth target");
        return false;
    }

    projection_ = SunLight::ShadowProjection{ MatrixIdentity(), 1.0f, 1.0f };
    loaded_ = true;
    return true;
}

void ShadowMap::setTransmitter(Texture2D translucency, Vector4 transmit)
{
    if (glassShader_.id == 0) return;

    if (translucency.id != 0) glassMaterial_.maps[MATERIAL_MAP_DIFFUSE].texture = translucency;

    if (locGlassTransmit_ >= 0) {
        const float packed[4] = { transmit.x, transmit.y, transmit.z, transmit.w };
        SetShaderValue(glassShader_, locGlassTransmit_, packed, SHADER_UNIFORM_VEC4);
    }
}

Vector2 ShadowMap::texelSize() const
{
    const float texel = 1.0f / static_cast<float>(kResolution);
    return Vector2{ texel, texel };
}

ShadowMap::Scope::Scope(ShadowMap& map, const SunLight::ShadowProjection& projection)
    : targetScope_(map.target_), map_(map)
{
    map_.projection_ = projection;

    /* WHITE, not blank: the colour plane holds transmission, and its default
     * has to mean "the sun's path here is clear". Clearing to black would say
     * every texel is behind glass. */
    ClearBackground(WHITE);

    /* BeginTextureMode leaves depth testing alone, and outside BeginMode3D it
     * is off — with it off the depth buffer is never written and the whole
     * pass silently produces nothing. */
    rlEnableDepthTest();

    rlSetMatrixProjection(MatrixIdentity());
    rlSetMatrixModelview(projection.viewProjection);
}

ShadowMap::Scope::~Scope()
{
    /* Flush anything still batched while the light matrices are current — the
     * caller's next draw belongs to the camera, not to the sun. */
    rlDrawRenderBatchActive();

    rlDisableDepthTest();
    rlSetMatrixProjection(MatrixIdentity());
    rlSetMatrixModelview(MatrixIdentity());
}

}  // namespace cromwell
