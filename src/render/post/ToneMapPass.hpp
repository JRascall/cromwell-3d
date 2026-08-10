/* ToneMapPass.hpp — the linear scene, resolved onto the screen.
 *
 * SINGLE RESPONSIBILITY: blit the HDR scene target to the backbuffer through
 * the filmic curve.
 *
 * This is the boundary of the linear pipeline. Everything before it is
 * radiance; everything after it — the ribbon, its glow, the HUD — is display
 * colour drawn straight onto the backbuffer, which is why those passes did not
 * have to change to gain lighting.
 *
 * The blit also RESOLVES THE SUPERSAMPLING: the scene target is
 * kSupersampleFactor times the window on each axis, and scaling it down with
 * bilinear filtering lands each tap at the centre of a 2x2 source block, which
 * averages all four. See tonemap.fs.glsl.
 */
#pragma once

#include "raylib.h"

#include "render/gpu/HdrTarget.hpp"

namespace xcom {

class ToneMapPass {
public:
    /* Two-times linear, so four samples per output pixel. The scene is a few
     * thousand untextured triangles; the fill cost is nothing next to what the
     * hard box edges look like without it. */
    static constexpr int kSupersampleFactor = 2;

    /* Chosen against SunLight's radiance so a sunlit wall lands near mid grey.
     * Moving one moves the other.
     *
     * It is this high because the palette is dark: a wall authored at 0x8891a0
     * is only a quarter reflectance once decoded to linear, and a quarter of
     * the light is what it actually returns. The unlit renderer showed those
     * bytes directly; a lit one has to earn them back through the exposure. */
    static constexpr float kDefaultExposure = 4.5f;

    ToneMapPass() = default;
    ~ToneMapPass();

    ToneMapPass(const ToneMapPass&) = delete;
    ToneMapPass& operator=(const ToneMapPass&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0; }

    void  setExposure(float exposure) { exposure_ = exposure; }
    float exposure() const { return exposure_; }

    /* Call on the backbuffer, inside BeginDrawing and outside any 3D mode. */
    void draw(const HdrTarget& scene, float destinationWidth, float destinationHeight) const;

private:
    Shader shader_ = { 0 };
    int    locExposure_ = -1;
    float  exposure_ = kDefaultExposure;
};

}  // namespace xcom
