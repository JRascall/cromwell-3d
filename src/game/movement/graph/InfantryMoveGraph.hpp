/* InfantryMoveGraph.hpp — legal steps for a 1x1 body.
 *
 * SINGLE RESPONSIBILITY: enumerate infantry moves. Walks, climbs over half
 * cover, mantles, one-way drops down open shafts, ramps, ladders, portals and
 * corner-safe diagonals.
 *
 * Scratch vectors are INSTANCE members, not file statics: two graphs are
 * independent, which is what the old module-scope buffers could not promise.
 */
#pragma once

#include "game/movement/occupancy/ColumnScanner.hpp"
#include "game/movement/graph/MoveGraph.hpp"
#include "game/query/LadderQuery.hpp"
#include "game/query/Standability.hpp"
#include "game/query/Terrain.hpp"
#include "game/world/World.hpp"

namespace game {


class InfantryMoveGraph : public MoveGraph {
public:
    explicit InfantryMoveGraph(const World& world);

    void neighbors(const Cell& from,
                   const BlockedMask* blocked,
                   std::vector<Move>& out) const override;

    bool isTraversable(const Cell& cell, const BlockedMask* blocked) const override;

    /* Surface-following moves only — used for border-loop topology. */
    void continuousNeighbors(const Cell& from, std::vector<Move>& out) const;

private:
    void addRampMoves(const Cell& from, const Tile& tile, std::vector<Move>& out) const;
    void addPortalMoves(const Cell& from, const Tile& tile, std::vector<Move>& out) const;
    void addLateralMoves(const Cell& from, float myHeight, std::vector<Move>& out) const;
    void addDiagonalMoves(const Cell& from, float myHeight, std::vector<Move>& out) const;

    const World& world_;
    Terrain      terrain_;
    Standability standability_;
    LadderQuery  ladders_;
    ColumnScanner columns_;

    /* reused scratch — see the header note */
    mutable std::vector<FlatSurface> surfaces_;
    mutable std::vector<RampSurface> ramps_;
    mutable std::vector<FlatSurface> shoulderA_;
    mutable std::vector<FlatSurface> shoulderB_;
    mutable std::vector<Move>        filterScratch_;
};

}  // namespace game
