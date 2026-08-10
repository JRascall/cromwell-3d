/* PrepassShader.hpp — the shader the scene prepass is drawn with.
 *
 * SINGLE RESPONSIBILITY: own the depth-and-normal program and the material
 * that carries it.
 *
 * Distinct from ShadowMap's caster shader, which is position-only. That pass
 * genuinely wants nothing but depth and would be paying for a normal it never
 * reads; this one feeds SSAO, which cannot work without one. Two ten-line
 * shaders is the honest split.
 */
#pragma once

#include "raylib.h"

namespace xcom {

class PrepassShader {
public:
    PrepassShader() = default;
    ~PrepassShader();

    PrepassShader(const PrepassShader&) = delete;
    PrepassShader& operator=(const PrepassShader&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0; }

    /* Const reference, not a value: Material::maps is a heap pointer, so a
     * by-value copy shares the map array. See PbrShader::material. */
    const Material& material() const { return material_; }

    /* Written into the G-buffer's alpha, for the screen-space passes that
     * cannot ask a material directly. Push it before every draw whose
     * roughness differs, exactly as the lit pass pushes its factors — an
     * unset value is the previous material's. */
    void setRoughness(float roughness) const;

private:
    Shader   shader_ = { 0 };
    Material material_ = { 0 };
    int      locRoughness_ = -1;
};

}  // namespace xcom
