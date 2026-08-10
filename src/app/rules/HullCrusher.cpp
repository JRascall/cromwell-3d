#include "app/rules/HullCrusher.hpp"

#include "core/world/MapAuthor.hpp"

namespace xcom {
namespace {

/* the four edges INSIDE a 2x2 footprint, relative to its anchor */
struct InternalEdge { int dx; int dy; Dir dir; };

constexpr InternalEdge kInternalEdges[4] = {
    { 0, 0, Dir::East },
    { 0, 1, Dir::East },
    { 0, 0, Dir::North },
    { 1, 0, Dir::North },
};

}  // namespace

int HullCrusher::crushAlong(const std::vector<int>& route)
{
    const Lattice& lattice = world_.lattice();
    MapAuthor author(world_);
    int crushed = 0;

    for (int index : route) {
        const Cell anchor = lattice.cellAt(index);

        for (const InternalEdge& internal : kInternalEdges) {
            const int x = anchor.x + internal.dx;
            const int y = anchor.y + internal.dy;

            const Edge edge = world_.effectiveEdge(x, y, anchor.z, internal.dir);
            if (edge.cover == Cover::Half && edge.destructible) {
                author.clearEdge(x, y, anchor.z, internal.dir);
                crushed++;
            }
        }
    }
    return crushed;
}

}  // namespace xcom
