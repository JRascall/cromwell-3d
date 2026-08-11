/* WorldAnchor.hpp — putting a screen-space widget over a world-space point.
 *
 * SINGLE RESPONSIBILITY: project a world position to device pixels and say
 * whether, where, and at what size a widget anchored to it should be drawn.
 *
 * WHY NOT DRAW THE UI IN THE WORLD. The obvious way to label a unit is a
 * billboarded quad in the 3D scene. Do not: it is rasterised at whatever size
 * perspective gives it, through a texture filter, at an angle, behind MSAA —
 * which is to say it is resampled, and resampled UI is soft UI. Every
 * complaint about blurry world-space labels in every engine comes from exactly
 * this.
 *
 * Projecting the ANCHOR and drawing the widget in screen space keeps the
 * geometry exact and the glyphs rasterised at their real size, so a nameplate
 * over a soldier is as crisp as one in a menu. The cost is that it does not
 * lean or occlude with the world, which for a nameplate is not a cost.
 *
 * WHAT THIS IS NOT FOR: decals, holograms, terminal screens, anything that
 * should genuinely live in the scene and be lit and occluded by it. That is a
 * material on a mesh, and it is a different job.
 *
 * THE SIZE LADDER IS THE IMPORTANT PART. Text scaled continuously with distance
 * asks the font set for a new atlas at every distinct rounded size — a label
 * drifting from 8 to 40 px would rasterise thirty-odd atlases, each a texture
 * upload, and a camera pull-back would hitch its way through all of them. So
 * distance scaling QUANTISES to a small ladder of steps: the label steps
 * between a handful of sizes as it recedes, each one crisp, instead of sliding
 * smoothly through a hundred blurry ones. Position stays continuous, so the
 * motion is still smooth; only the size is stepped, and at these ratios nobody
 * sees it happen.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"

#include "raylib.h"

namespace cromwell::ui {

/* How a world-anchored widget should be drawn this frame.
 *
 * ONE-SHOT DATA CARRIER (see the note in UiColor.hpp): produced by one call,
 * read at one site, dead within the frame. */
struct WorldAnchor {
    /* False when the point is behind the camera or beyond the fade distance —
     * the caller draws nothing. Checked rather than trusted, because a
     * projection of a point behind the eye produces a plausible-looking screen
     * position on the WRONG side of the screen. */
    bool visible = false;

    /* Device pixels, ready to hand to a widget. */
    Vec2 screenPosition;

    /* Metres from the camera, for the caller's own fading or sorting. */
    float distance = 0.0f;

    /* Multiply the widget's spec by this AS WELL AS the display scale — it is
     * the perspective term, quantised to the ladder. 1.0 at the reference
     * distance. */
    float scale = 1.0f;
};

/* How a world anchor should shrink with distance.
 *
 * ONE-SHOT DATA CARRIER. Defaults give a nameplate that is full size out to
 * eight metres and never drops below half. */
struct WorldAnchorSettings {
    /* Distance at which `scale` is 1. Closer than this the widget does NOT
     * grow — a nameplate that swelled as you approached would swamp the screen,
     * and every game that ships one clamps it. */
    float referenceDistance = 8.0f;

    /* Scale bounds after the perspective term. */
    float minScale = 0.5f;
    float maxScale = 1.0f;

    /* Beyond this the anchor reports invisible. 0 disables the cutoff. */
    float maxDistance = 0.0f;

    /* Size steps the scale is snapped to — see the header on why this is not
     * continuous. Larger means fewer font atlases and a more visible step. */
    float scaleStep = 0.25f;
};

/* Projects `worldPosition` for `camera`, in device pixels.
 *
 * `pixelOffset` is applied after projection, for lifting a nameplate above the
 * point it is anchored to — in pixels rather than world units so the gap does
 * not collapse as the target recedes, which is what makes it a UI offset rather
 * than a second world position. Pass it already multiplied by the display
 * scale. */
WorldAnchor anchorToWorld(Vector3 worldPosition, const Camera3D& camera,
                          Vec2 pixelOffset = {},
                          const WorldAnchorSettings& settings = {});

}  // namespace cromwell::ui
