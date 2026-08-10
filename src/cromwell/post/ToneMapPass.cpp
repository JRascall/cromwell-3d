#include "cromwell/post/ToneMapPass.hpp"

#include "cromwell/gpu/ShaderLibrary.hpp"

namespace cromwell {

ToneMapPass::~ToneMapPass()
{
    if (shader_.id) UnloadShader(shader_);
}

bool ToneMapPass::load()
{
    shader_ = ShaderLibrary::load(nullptr, "tonemap.fs.glsl");
    if (shader_.id == 0) return false;

    locExposure_ = GetShaderLocation(shader_, "uExposure");
    return true;
}

void ToneMapPass::draw(const HdrTarget& scene,
                       float destinationWidth, float destinationHeight) const
{
    if (shader_.id == 0 || !scene.valid()) return;

    SetShaderValue(shader_, locExposure_, &exposure_, SHADER_UNIFORM_FLOAT);

    /* Negative source height: raylib's FBOs are bottom-up. */
    const Rectangle source{ 0.0f, 0.0f, scene.width(), -scene.height() };
    const Rectangle destination{ 0.0f, 0.0f, destinationWidth, destinationHeight };

    BeginShaderMode(shader_);
    DrawTexturePro(scene.texture(), source, destination, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
}

}  // namespace cromwell
