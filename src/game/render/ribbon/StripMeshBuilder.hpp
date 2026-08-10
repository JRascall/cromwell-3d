/* StripMeshBuilder.hpp — a polyline into a flat decal strip.
 *
 * SINGLE RESPONSIBILITY: geometry. Miter joins, vertical-riser handling and
 * the UV run — no shader, no target, no draw call.
 *
 * The width direction is resolved PER SEGMENT: using the chord (prev -> next)
 * at each vertex smears a corner's rotation along the whole adjacent segment,
 * which visibly bows long edges and twists the tight hairpins around wall tips.
 */
#pragma once

#include "raylib.h"

#include "game/border/loop/LoopPolyliner.hpp"

#include <vector>

namespace game {


class StripMeshBuilder {
public:
    /* Returns a Mesh with vertexCount 0 for a degenerate polyline. */
    Mesh build(const std::vector<BorderPoint>& points, float halfWidth, float lift);

private:
    /* per-vertex scratch, reused across loops */
    std::vector<float> directions_;   /* 2 per point */
    std::vector<float> pushes_;       /* 2 per point */
    std::vector<float> runLengths_;   /* n + 1       */
};

}  // namespace game
