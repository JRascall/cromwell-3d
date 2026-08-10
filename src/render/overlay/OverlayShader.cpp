#include "render/overlay/OverlayShader.hpp"

#include "render/gpu/ShaderLibrary.hpp"

namespace xcom {

OverlayShader::~OverlayShader()
{
    if (shader_.id) UnloadShader(shader_);
}

bool OverlayShader::load()
{
    /* raylib's default vertex stage already provides everything the batch
     * feeds it — position, UV and vertex colour through one mvp. */
    shader_ = ShaderLibrary::load(nullptr, "unlit_linear.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "OVERLAY: no linear shader - overlays will read hot");
        return false;
    }
    return true;
}

}  // namespace xcom
