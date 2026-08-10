/* RibbonRenderer.hpp — submit the ribbon strips.
 *
 * SINGLE RESPONSIBILITY: set the render state and issue the draw calls. It
 * borrows the shader and the meshes; it owns neither.
 */
#pragma once

#include "raylib.h"

#include "cromwell/ribbon/RibbonShader.hpp"
#include "cromwell/ribbon/Ring.hpp"
#include "cromwell/ribbon/StripSet.hpp"

namespace cromwell {

/* Everything a ribbon pass needs to know about the frame. */
struct RibbonPassSettings {
    Camera3D camera{};
    RingMask visibleRings;
    Ring     solidRing = Ring::Move;
    float    hideHeight = 0.0f;   /* world height above which the border dissolves */
    int      maxStorey = 0;       /* culls whole loops starting above the iso floor */
};

class RibbonRenderer {
public:
    RibbonRenderer(const RibbonShader& shader, const StripSet& meshes)
        : shader_(shader), meshes_(meshes) {}

    /* `viewWidth/Height` is the size of the TARGET being drawn into, which the
     * fragment stage needs to turn gl_FragCoord into the screen-space lookup
     * for the depth texture — it is half that in the glow pass. */
    void submit(const RibbonPassSettings& settings,
                float emissive,
                float viewWidth, float viewHeight,
                Texture2D sceneDepth) const;

private:
    const RibbonShader& shader_;
    const StripSet&     meshes_;
};

}  // namespace cromwell
