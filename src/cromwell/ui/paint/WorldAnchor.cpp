#include "cromwell/ui/paint/WorldAnchor.hpp"

#include "cromwell/camera/Viewport.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell::ui {

WorldAnchor anchorToWorld(Vec3 worldPosition, const Camera3D& camera,
                          Vec2 pixelOffset, const WorldAnchorSettings& settings)
{
    WorldAnchor anchor;

    /* THE PROJECTION AND THE BEHIND-THE-CAMERA TEST BOTH LIVE IN Viewport, and
     * they used to live here. Moving them was the point of that class: this file
     * had the only correct depth test in the codebase, and the next caller that
     * wanted to put something over a world point would have written its own and
     * got it wrong. See camera/Viewport.hpp.
     *
     * The window is the right rectangle here because the UI kit draws into the
     * window — a HUD anchored to a point in an inset viewport would build its
     * own Viewport and project through that instead. */
    const Viewport viewport = Viewport::ofWindow(camera);
    const ScreenPoint projected = viewport.project(worldPosition);

    if (!projected.inFront) {
        return anchor;
    }

    anchor.distance = projected.distance;
    if (settings.maxDistance > 0.0f && anchor.distance > settings.maxDistance) {
        return anchor;
    }

    /* Device pixels, the same ones the rest of the UI is in — see the
     * display-scale note in UiContext.hpp. */
    anchor.screenPosition = projected.position + pixelOffset;

    /* Inverse-depth falloff, clamped, then snapped to the ladder. DEPTH rather
     * than distance, so a target at the edge of a wide field of view does not
     * shrink relative to one dead ahead at the same range. */
    const float reference = std::max(settings.referenceDistance, 0.01f);
    const float raw = std::clamp(reference / std::max(projected.depth, 0.01f),
                                 settings.minScale, settings.maxScale);

    const float step = std::max(settings.scaleStep, 0.01f);
    anchor.scale = std::clamp(std::round(raw / step) * step,
                              settings.minScale, settings.maxScale);

    anchor.visible = true;
    return anchor;
}

}  // namespace cromwell::ui
