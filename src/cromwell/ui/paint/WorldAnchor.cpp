#include "cromwell/ui/paint/WorldAnchor.hpp"

#include "raymath.h"

#include <algorithm>
#include <cmath>

namespace cromwell::ui {

WorldAnchor anchorToWorld(Vector3 worldPosition, const Camera3D& camera,
                          Vec2 pixelOffset, const WorldAnchorSettings& settings)
{
    WorldAnchor anchor;

    const Vector3 toPoint = Vector3Subtract(worldPosition, camera.position);
    const Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));

    /* Depth along the VIEW DIRECTION, not the straight-line distance: the
     * behind-the-camera test needs the component that the projection actually
     * divides by. A point beside the camera is in front of it by straight-line
     * distance and behind it by depth, and projecting one produces a screen
     * position that looks entirely reasonable and is mirrored onto the wrong
     * side of the view. */
    const float depth = Vector3DotProduct(toPoint, forward);
    if (depth <= 0.01f) {
        return anchor;
    }

    anchor.distance = Vector3Length(toPoint);
    if (settings.maxDistance > 0.0f && anchor.distance > settings.maxDistance) {
        return anchor;
    }

    /* GetWorldToScreen works in the same device pixels the rest of the UI does,
     * because raylib's screen size IS the framebuffer size in this
     * configuration — see the display-scale note in UiContext.hpp. */
    const Vector2 projected = GetWorldToScreen(worldPosition, camera);
    anchor.screenPosition = { projected.x + pixelOffset.x, projected.y + pixelOffset.y };

    /* Inverse-depth falloff, clamped, then snapped to the ladder. Depth rather
     * than distance again, so a target at the edge of a wide field of view does
     * not shrink relative to one dead ahead at the same range. */
    const float reference = std::max(settings.referenceDistance, 0.01f);
    const float raw = std::clamp(reference / std::max(depth, 0.01f),
                                 settings.minScale, settings.maxScale);

    const float step = std::max(settings.scaleStep, 0.01f);
    anchor.scale = std::clamp(std::round(raw / step) * step,
                              settings.minScale, settings.maxScale);

    anchor.visible = true;
    return anchor;
}

}  // namespace cromwell::ui
