/* VehicleMoveGraph.hpp — legal steps for a 2x2 hull.
 *
 * SINGLE RESPONSIBILITY: enumerate vehicle moves in ANCHOR space.
 *
 * An anchor is valid when all four footprint tiles are flat standable floor
 * and no FULL cover runs through the footprint (half cover is crushed on
 * arrival). No stairs, ladders, drops or portals: vehicles stay on a storey.
 */
#pragma once

#include "game/movement/graph/MoveGraph.hpp"
#include "game/query/Standability.hpp"
#include "game/world/World.hpp"

namespace game {


class VehicleMoveGraph : public MoveGraph {
public:
    explicit VehicleMoveGraph(const World& world)
        : world_(world), standability_(world) {}

    void neighbors(const Cell& from,
                   const BlockedMask* blocked,
                   std::vector<Move>& out) const override;

    /* All four footprint tiles legal, and no full cover through the hull. */
    bool isTraversable(const Cell& anchor, const BlockedMask* blocked) const override;

private:
    /* Would the hull have to pass through full cover stepping `d`? */
    bool stepEdgesClear(const Cell& anchor, Dir d) const;

    const World& world_;
    Standability standability_;
};

}  // namespace game
