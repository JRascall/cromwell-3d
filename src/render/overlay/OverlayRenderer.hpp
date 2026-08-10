/* OverlayRenderer.hpp — the diagnostic overlays drawn over the world.
 *
 * SINGLE RESPONSIBILITY: draw the debug/feedback layers — visibility plates,
 * cover shields, the hover plate and the path preview line. It holds no state
 * of its own; every call takes what it should draw.
 */
#pragma once

#include "raylib.h"

#include "core/los/VisibilityField.hpp"
#include "core/movement/PathPoint.hpp"
#include "core/query/CoverModel.hpp"
#include "core/query/Standability.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

#include <vector>

namespace xcom {

class UnitRoster;

class OverlayRenderer {
public:
    OverlayRenderer(const World& world, const UnitRoster& roster)
        : world_(world), terrain_(world), standability_(world), cover_(world, roster) {}

    /* Green = seen directly, teal = only by leaning out, dull red = not seen. */
    void drawVisibility(const VisibilityField& visibility, int maxStorey) const;

    /* The little armour plates showing which faces of a tile grant cover. */
    void drawCoverShields(const Cell& cell) const;

    /* The plate under the cursor. `size` widens for a 2x2 hull. */
    void drawHoverPlate(const Cell& cell, float height, float size, Color colour) const;

    void drawPathPreview(const std::vector<PathPoint>& path) const;

private:
    const World& world_;
    Terrain      terrain_;
    Standability standability_;
    CoverModel   cover_;
};

}  // namespace xcom
