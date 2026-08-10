#include "render/post/PrepassShader.hpp"

#include "render/gpu/ShaderLibrary.hpp"

namespace xcom {

PrepassShader::~PrepassShader()
{
    if (shader_.id) UnloadShader(shader_);
}

bool PrepassShader::load()
{
    shader_ = ShaderLibrary::load("prepass.vs.glsl", "prepass.fs.glsl");
    if (shader_.id == 0) {
        TraceLog(LOG_WARNING, "PREPASS: no depth/normal shader - SSAO will be off");
        return false;
    }

    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    locRoughness_ = GetShaderLocation(shader_, "uPrepassRoughness");
    return true;
}

void PrepassShader::setRoughness(float roughness) const
{
    if (locRoughness_ < 0) return;
    SetShaderValue(shader_, locRoughness_, &roughness, SHADER_UNIFORM_FLOAT);
}

}  // namespace xcom
