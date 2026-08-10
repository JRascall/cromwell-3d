#include "core/los/PeekFinder.hpp"

#include <algorithm>

namespace xcom {

std::vector<Cell> PeekFinder::peekPositions(const Cell& from) const
{
    std::vector<Cell> peeks;

    for (Dir d : kAllDirs) {
        const Edge own = world_.effectiveEdge(from, d);
        /* windows: fire over the parapet rather than leaning around it */
        if (own.cover != Cover::Full || own.window) continue;

        for (Dir sideways : perpendicular(d)) {
            const int nx = from.x + dx(sideways);
            const int ny = from.y + dy(sideways);
            if (!world_.lattice().inBounds(nx, ny)) continue;

            const Tile* tile = world_.tryAt(nx, ny, from.z);
            if (!tile || tile->blocked || tile->isRamp()) continue;   /* airspace not clear */

            /* a wall between us and the peek tile */
            if (world_.effectiveEdge(from, sideways).cover == Cover::Full) continue;

            /* no corner to lean around if the cover simply continues */
            const Edge continuation = world_.effectiveEdge(nx, ny, from.z, d);
            if (continuation.cover == Cover::Full && !continuation.window) continue;

            const Cell candidate{ nx, ny, from.z };
            const bool alreadyFound = std::any_of(
                peeks.begin(), peeks.end(),
                [&](const Cell& c) { return c.x == candidate.x && c.y == candidate.y; });
            if (!alreadyFound) peeks.push_back(candidate);
        }
    }
    return peeks;
}

}  // namespace xcom
