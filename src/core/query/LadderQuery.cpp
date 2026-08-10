#include "core/query/LadderQuery.hpp"

namespace xcom {

std::optional<LadderHit> LadderQuery::targetFrom(int x, int y, int z, Dir d) const
{
    if (!world_.effectiveEdge(x, y, z, d).ladder) return std::nullopt;

    const int   nx     = x + dx(d);
    const int   ny     = y + dy(d);
    const float fromH  = terrain_.centerHeight(x, y, z);
    const int   depth  = world_.lattice().depth();

    for (int z2 = z + 1; z2 < depth; z2++) {
        const Tile* above = world_.tryAt(x, y, z2);
        const Tile* below = world_.tryAt(x, y, z2 - 1);
        if (above && above->hasFloor) return std::nullopt;   /* ceiling over the climber */
        if (below && below->canopy)   return std::nullopt;   /* roof plane over him      */

        const Tile* landing = world_.tryAt(nx, ny, z2);
        if (!landing || landing->blocked) return std::nullopt;

        if (landing->hasFloor && !landing->isRamp() &&
            world_.effectiveEdge(x, y, z2, d).cover != Cover::Full) {
            LadderHit hit;
            hit.cell = { nx, ny, z2 };
            hit.rise = terrain_.centerHeight(nx, ny, z2) - fromH;
            return hit;
        }
        /* floored-but-walled cells: the climb continues past them */
    }
    return std::nullopt;
}

}  // namespace xcom
