#include "render/gpu/CustomDepthStencil.hpp"

#include "rlgl.h"

#include "render/gpu/ShaderLibrary.hpp"

namespace xcom {

CustomDepthStencil::~CustomDepthStencil()
{
    if (shader_.id) UnloadShader(shader_);
}

bool CustomDepthStencil::load()
{
    /* The depth-only vertex stage, shared with the shadow map and the
     * prepass: this pass wants position and nothing else, and paying for a
     * normal it never reads would be the same mistake three times. */
    shader_ = ShaderLibrary::load("depth_only.vs.glsl", "custom_stencil.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "STENCIL: no shader - custom depth will be unavailable");
        return false;
    }

    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    locStencil_ = GetShaderLocation(shader_, "uStencilValue");
    return true;
}

void CustomDepthStencil::resize(int width, int height)
{
    target_.resize(width, height);

    /* POINT, AND IT IS NOT OPTIONAL. This channel holds an object id quantised
     * to 8 bits. Bilinear filtering across the boundary between object 3 and
     * object 7 returns a 5 — an id belonging to nothing, along every silhouette
     * edge, which is precisely where a consumer of this buffer does its work. */
    if (target_.valid()) {
        SetTextureFilter(target_.colourTexture(), TEXTURE_FILTER_POINT);
        SetTextureFilter(target_.depthTexture(), TEXTURE_FILTER_POINT);
    }
}

void CustomDepthStencil::setStencil(int value) const
{
    if (locStencil_ < 0) return;

    const int clamped = (value < 0) ? 0 : (value > kMaxValue ? kMaxValue : value);
    const float encoded = static_cast<float>(clamped) / static_cast<float>(kMaxValue);
    SetShaderValue(shader_, locStencil_, &encoded, SHADER_UNIFORM_FLOAT);
}

/* Blending OFF for the whole pass. An id is a NUMBER, and blending it against
 * whatever was underneath produces a different number — one that identifies a
 * different object, or none. The pass replaces; it never composites. */
CustomDepthStencil::Scope::Scope(const CustomDepthStencil& buffer)
    : inner_(buffer.target_)
{
    rlDisableColorBlend();
}

CustomDepthStencil::Scope::~Scope()
{
    rlEnableColorBlend();
}

}  // namespace xcom
