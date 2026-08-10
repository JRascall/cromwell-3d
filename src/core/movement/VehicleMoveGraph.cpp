#include "core/movement/VehicleMoveGraph.hpp"

#include "core/lattice/Constants.hpp"

namespace xcom {
namespace {

/* the 2x2 hull, anchored at (0,0) */
constexpr int kHullFootprint[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
constexpr int kDiagonalOffsets[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

}  // namespace

bool VehicleMoveGraph::isTraversable(const Cell& anchor, const BlockedMask* blocked) const
{
    const Lattice& lattice = world_.lattice();

    for (const auto& offset : kHullFootprint) {
        const int x = anchor.x + offset[0];
        const int y = anchor.y + offset[1];
        if (!lattice.inBounds(x, y)) return false;
        if (!standability_.isFlatStandable(x, y, anchor.z)) return false;
        if (blocked && blocked->isBlocked(lattice.index(x, y, anchor.z))) return false;
    }

    /* no FULL cover through the footprint's internal edges */
    const int z = anchor.z;
    if (world_.effectiveEdge(anchor.x,     anchor.y,     z, Dir::East).cover  == Cover::Full) return false;
    if (world_.effectiveEdge(anchor.x,     anchor.y + 1, z, Dir::East).cover  == Cover::Full) return false;
    if (world_.effectiveEdge(anchor.x,     anchor.y,     z, Dir::North).cover == Cover::Full) return false;
    if (world_.effectiveEdge(anchor.x + 1, anchor.y,     z, Dir::North).cover == Cover::Full) return false;
    return true;
}

bool VehicleMoveGraph::stepEdgesClear(const Cell& anchor, Dir d) const
{
    const int ax = anchor.x, ay = anchor.y, z = anchor.z;
    int edges[2][2];

    switch (d) {
        case Dir::East:  edges[0][0] = ax + 1; edges[0][1] = ay;
                         edges[1][0] = ax + 1; edges[1][1] = ay + 1; break;
        case Dir::West:  edges[0][0] = ax;     edges[0][1] = ay;
                         edges[1][0] = ax;     edges[1][1] = ay + 1; break;
        case Dir::North: edges[0][0] = ax;     edges[0][1] = ay + 1;
                         edges[1][0] = ax + 1; edges[1][1] = ay + 1; break;
        default:         edges[0][0] = ax;     edges[0][1] = ay;         /* South */
                         edges[1][0] = ax + 1; edges[1][1] = ay;     break;
    }

    for (const auto& e : edges)
        if (world_.effectiveEdge(e[0], e[1], z, d).cover == Cover::Full) return false;
    return true;
}

void VehicleMoveGraph::neighbors(const Cell& from,
                                 const BlockedMask* blocked,
                                 std::vector<Move>& out) const
{
    out.clear();

    for (Dir d : kAllDirs) {
        const Cell target{ from.x + dx(d), from.y + dy(d), from.z };
        if (!isTraversable(target, blocked)) continue;
        if (!stepEdgesClear(from, d)) continue;
        out.push_back({ target, 1.0f, MoveKind::Walk, d });
    }

    for (const auto& offset : kDiagonalOffsets) {
        const int ox = offset[0], oy = offset[1];
        const Cell target{ from.x + ox, from.y + oy, from.z };
        const Cell viaX { from.x + ox, from.y,      from.z };
        const Cell viaY { from.x,      from.y + oy, from.z };

        if (!isTraversable(target, blocked)) continue;
        if (!isTraversable(viaX, blocked)) continue;
        if (!isTraversable(viaY, blocked)) continue;

        const Dir dirX = ox > 0 ? Dir::East : Dir::West;
        const Dir dirY = oy > 0 ? Dir::North : Dir::South;

        if (!stepEdgesClear(from,  dirX)) continue;
        if (!stepEdgesClear(from,  dirY)) continue;
        if (!stepEdgesClear(viaX,  dirY)) continue;
        if (!stepEdgesClear(viaY,  dirX)) continue;

        out.push_back({ target, kDiagonalCost, MoveKind::Diagonal, std::nullopt });
    }
}

}  // namespace xcom
