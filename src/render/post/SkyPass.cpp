#include "render/post/SkyPass.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

namespace xcom {
namespace {

void setVector3(Shader shader, int location, Vector3 value)
{
    if (location < 0) return;
    const float packed[3] = { value.x, value.y, value.z };
    SetShaderValue(shader, location, packed, SHADER_UNIFORM_VEC3);
}

}  // namespace

SkyPass::~SkyPass()
{
    if (shader_.id) UnloadShader(shader_);
}

bool SkyPass::load()
{
    /* No vertex stage of its own: raylib's default one already delivers what a
     * fullscreen rectangle needs, and the fragment shader works off
     * gl_FragCoord rather than an interpolated UV. */
    shader_ = ShaderLibrary::load(nullptr, "sky.fs.glsl");
    if (shader_.id == 0) return false;

    locResolution_            = GetShaderLocation(shader_, "uResolution");
    locInverseViewProjection_ = GetShaderLocation(shader_, "uInverseViewProjection");
    locSunDirection_          = GetShaderLocation(shader_, "uSunDirection");
    locSunColour_             = GetShaderLocation(shader_, "uSunColour");
    locZenithColour_          = GetShaderLocation(shader_, "uZenithColour");
    locHorizonColour_         = GetShaderLocation(shader_, "uHorizonColour");
    locGroundColour_          = GetShaderLocation(shader_, "uGroundColour");
    return true;
}

void SkyPass::draw(const SunLight& sun, const Camera3D& camera, int width, int height) const
{
    if (shader_.id == 0) return;

    /* Rebuilt here rather than read back from rlgl, because this pass runs
     * OUTSIDE BeginMode3D — at this point rlgl still holds the target's 2D
     * ortho, which is not the camera the ray should be unprojected through. */
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Matrix view = GetCameraMatrix(camera);
    const Matrix projection = MatrixPerspective(camera.fovy * DEG2RAD, aspect,
                                                rlGetCullDistanceNear(),
                                                rlGetCullDistanceFar());
    const Matrix inverse = MatrixInvert(MatrixMultiply(view, projection));

    const float resolution[2] = { static_cast<float>(width), static_cast<float>(height) };
    SetShaderValue(shader_, locResolution_, resolution, SHADER_UNIFORM_VEC2);
    SetShaderValueMatrix(shader_, locInverseViewProjection_, inverse);

    setVector3(shader_, locSunDirection_,  sun.travelDirection());
    setVector3(shader_, locSunColour_,     sun.radiance());
    setVector3(shader_, locZenithColour_,  sun.zenithColour());
    setVector3(shader_, locHorizonColour_, sun.horizonColour());
    setVector3(shader_, locGroundColour_,  sun.groundColour());

    BeginShaderMode(shader_);
    DrawRectangle(0, 0, width, height, WHITE);
    EndShaderMode();
}

}  // namespace xcom
