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
                       float destinationWidth, float destinationHeight, bool graded) const
{
    if (!scene.valid()) return;

    /* Negative source height: raylib's FBOs are bottom-up. */
    const Rectangle source{ 0.0f, 0.0f, scene.width(), -scene.height() };
    const Rectangle destination{ 0.0f, 0.0f, destinationWidth, destinationHeight };

    if (graded && shader_.id != 0) {
        SetShaderValue(shader_, locExposure_, &exposure_, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(shader_);
        DrawTexturePro(scene.texture(), source, destination, Vector2{ 0.0f, 0.0f }, 0.0f,
                       WHITE);
        EndShaderMode();
        return;
    }

    /* RAW — the switch's off position and the missing-shader fallback share
     * one path: the identical blit, no curve. See the header. */
    DrawTexturePro(scene.texture(), source, destination, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
}

}  // namespace cromwell
