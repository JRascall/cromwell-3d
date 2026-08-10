/* RibbonShader.hpp — the MovementBorder shader and its uniforms.
 *
 * SINGLE RESPONSIBILITY: own the shader program, cache its uniform locations
 * and expose typed setters. It does not decide what to draw, own any target,
 * or know the glow chain exists.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

class RibbonShader {
public:
    RibbonShader() = default;
    ~RibbonShader();

    RibbonShader(const RibbonShader&) = delete;
    RibbonShader& operator=(const RibbonShader&) = delete;

    /* Loads the program and pushes the constants that never change across a
     * run. False when the shader files could not be found. */
    bool load();

    bool valid() const { return shader_.id != 0; }
    const Material& material() const { return material_; }

    /* ---- per-frame uniforms ------------------------------------------- */
    void setViewport(float width, float height) const;
    void setCamera(const Camera3D& camera) const;
    void setTime(float seconds) const;
    void setHideHeight(float height) const;
    void setEmissive(float emissive) const;
    void setPanSpeed(float unitsPerSecond) const;
    void setDepthTexture(Texture2D depth) const;

    /* ---- per-mesh uniforms -------------------------------------------- */
    void setColour(Color colour) const;
    /* 1 draws the profile's standing (solid) channel, 0 the scrolling one. */
    void setRelevance(float relevance) const;

private:
    Shader shader_ = { 0 };

    /* Mutable because setDepthTexture is const like every other setter here,
     * and for the same reason: this class holds draw state, and whether that
     * state lives in a uniform or in a material's texture slot is raylib's
     * business, not the caller's. See RibbonShader::setDepthTexture for why
     * the depth texture has to travel in the material at all. */
    mutable Material material_ = { 0 };

    int locResolution_   = 0;
    int locNear_         = 0;
    int locFar_          = 0;
    int locCameraPos_    = 0;
    int locWpoPush_      = 0;
    int locTime_         = 0;
    int locColour_       = 0;
    int locEmissive_     = 0;
    int locRelevance_    = 0;
    int locHideHeight_   = 0;
    int locHideFade_     = 0;
    int locDepthRate_    = 0;
    int locDepthFloor_   = 0;
    int locPanSpeed_     = 0;
};

}  // namespace cromwell
