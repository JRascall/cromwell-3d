/* SkyPass.hpp — the analytic sky, drawn behind everything.
 *
 * SINGLE RESPONSIBILITY: fill the scene target with sky before any geometry
 * goes into it.
 *
 * It runs as a flat rectangle in 2D, not as a skybox mesh. Depth testing is
 * off outside BeginMode3D, and in OpenGL a disabled depth test also disables
 * depth WRITES — so the sky lands under the whole frame without needing a far
 * plane, a cube, or a depth-clamp trick, and the geometry drawn after it
 * occludes it for free.
 *
 * The sun disc it draws and the ambient the surfaces are lit by come from the
 * same SunLight and the same two colour lobes, so the background can never
 * disagree with the lighting in front of it.
 */
#pragma once

#include "raylib.h"

#include "cromwell/lighting/SunLight.hpp"

namespace cromwell {

class SkyPass {
public:
    SkyPass() = default;
    ~SkyPass();

    SkyPass(const SkyPass&) = delete;
    SkyPass& operator=(const SkyPass&) = delete;

    bool load();
    bool valid() const { return shader_.id != 0; }

    /* Call inside the scene target, before BeginMode3D. `width`/`height` are
     * the TARGET's size, which is not the window's — the scene is
     * supersampled. */
    void draw(const SunLight& sun, const Camera3D& camera, int width, int height) const;

private:
    Shader shader_ = { 0 };

    int locResolution_ = -1;
    int locInverseViewProjection_ = -1;
    int locSunDirection_ = -1;
    int locSunColour_ = -1;
    int locZenithColour_ = -1;
    int locHorizonColour_ = -1;
    int locGroundColour_ = -1;
};

}  // namespace cromwell
