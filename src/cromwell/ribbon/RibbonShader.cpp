#include "cromwell/ribbon/RibbonShader.hpp"

#include "rlgl.h"

#include "cromwell/gpu/ShaderLibrary.hpp"
#include "cromwell/ribbon/RibbonConstants.hpp"

namespace cromwell {

RibbonShader::~RibbonShader()
{
    if (shader_.id) UnloadShader(shader_);
}

bool RibbonShader::load()
{
    shader_ = ShaderLibrary::load("ribbon.vs.glsl", "ribbon.fs.glsl");
    if (shader_.id == 0) return false;

    locResolution_   = GetShaderLocation(shader_, "uResolution");
    locNear_         = GetShaderLocation(shader_, "uNear");
    locFar_          = GetShaderLocation(shader_, "uFar");
    locCameraPos_    = GetShaderLocation(shader_, "uCamPos");
    locWpoPush_      = GetShaderLocation(shader_, "uWpoPush");
    locTime_         = GetShaderLocation(shader_, "uTime");
    locColour_       = GetShaderLocation(shader_, "uColor");
    locEmissive_     = GetShaderLocation(shader_, "uEmissive");
    locRelevance_    = GetShaderLocation(shader_, "uBorderRelevance");
    locHideHeight_   = GetShaderLocation(shader_, "uHideHeight");
    locHideFade_     = GetShaderLocation(shader_, "uHideFade");
    locDepthRate_    = GetShaderLocation(shader_, "uDepthRate");
    locDepthFloor_   = GetShaderLocation(shader_, "uDepthFloor");
    locPanSpeed_     = GetShaderLocation(shader_, "uPanSpeed");

    /* the vertex stage offsets in world space, so it needs view and projection
     * apart rather than the combined mvp raylib would otherwise be enough */
    shader_.locs[SHADER_LOC_MATRIX_VIEW]       = GetShaderLocation(shader_, "matView");
    shader_.locs[SHADER_LOC_MATRIX_PROJECTION] = GetShaderLocation(shader_, "matProjection");

    /* depthTex is the material's diffuse SLOT, not a SetShaderValueTexture
     * sampler — see setDepthTexture. Nothing else in this shader samples, so
     * slot 0 is free and the default white texture it held is not missed. */
    shader_.locs[SHADER_LOC_MAP_DIFFUSE] = GetShaderLocation(shader_, "depthTex");

    material_ = LoadMaterialDefault();
    material_.shader = shader_;

    /* the constants that never change across a run */
    const float push       = kRibbonWpoPush;
    const float hideFade   = kRibbonHideFade;
    const float depthRate  = kRibbonDepthRate;
    const float depthFloor = kRibbonDepthFloor;
    const float panSpeed   = kRibbonPanSpeed;
    SetShaderValue(shader_, locWpoPush_,    &push,       SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locHideFade_,   &hideFade,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locDepthRate_,  &depthRate,  SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locDepthFloor_, &depthFloor, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locPanSpeed_,   &panSpeed,   SHADER_UNIFORM_FLOAT);
    return true;
}

void RibbonShader::setViewport(float width, float height) const
{
    const float resolution[2] = { width, height };
    const float nearPlane = rlGetCullDistanceNear();
    const float farPlane  = rlGetCullDistanceFar();
    SetShaderValue(shader_, locResolution_, resolution, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, locNear_,       &nearPlane, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, locFar_,        &farPlane,  SHADER_UNIFORM_FLOAT);
}

void RibbonShader::setCamera(const Camera3D& camera) const
{
    const float position[3] = { camera.position.x, camera.position.y, camera.position.z };
    SetShaderValue(shader_, locCameraPos_, position, SHADER_UNIFORM_VEC3);
}

void RibbonShader::setTime(float seconds) const
{
    SetShaderValue(shader_, locTime_, &seconds, SHADER_UNIFORM_FLOAT);
}

void RibbonShader::setHideHeight(float height) const
{
    SetShaderValue(shader_, locHideHeight_, &height, SHADER_UNIFORM_FLOAT);
}

void RibbonShader::setEmissive(float emissive) const
{
    SetShaderValue(shader_, locEmissive_, &emissive, SHADER_UNIFORM_FLOAT);
}

/* THIS CANNOT USE SetShaderValueTexture. That call hands the texture to rlgl's
 * internal BATCH, which binds it when the batch is next flushed and clears the
 * registration afterwards — but DrawMesh does not go through the batch at all,
 * so the sampler is left pointing at an unbound unit.
 *
 * It used to survive on an accident of ordering: the ribbon shared one
 * BeginMode3D with the batched overlay draws, and their flush bound the
 * texture. Once the lit pipeline moved the ribbon into its own pass after the
 * tonemap, that flush was gone, depthTex sampled zero, and every fragment took
 * the discard in depthFade — the ribbon vanished completely, with no warning
 * from anything.
 *
 * A material map slot is bound by DrawMesh itself, so it cannot come apart. */
void RibbonShader::setPanSpeed(float unitsPerSecond) const
{
    SetShaderValue(shader_, locPanSpeed_, &unitsPerSecond, SHADER_UNIFORM_FLOAT);
}

void RibbonShader::setDepthTexture(Texture2D depth) const
{
    material_.maps[MATERIAL_MAP_DIFFUSE].texture = depth;
}

void RibbonShader::setColour(Color colour) const
{
    const float rgba[4] = { colour.r / 255.0f, colour.g / 255.0f,
                            colour.b / 255.0f, colour.a / 255.0f };
    SetShaderValue(shader_, locColour_, rgba, SHADER_UNIFORM_VEC4);
}

void RibbonShader::setRelevance(float relevance) const
{
    SetShaderValue(shader_, locRelevance_, &relevance, SHADER_UNIFORM_FLOAT);
}

}  // namespace cromwell
