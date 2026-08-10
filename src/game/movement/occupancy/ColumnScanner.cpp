#include "game/movement/occupancy/ColumnScanner.hpp"

namespace game {


void ColumnScanner::flatSurfaces(int x, int y, std::vector<FlatSurface>& out) const
{
    out.clear();
    const Lattice& lattice = world_.lattice();
    if (!lattice.inBounds(x, y)) return;

    for (int z = 0; z < lattice.depth(); z++) {
        const Tile& tile = world_.at(lattice.index(x, y, z));
        if (tile.hasFloor && !tile.blocked && !tile.isRamp())
            out.push_back({ z, Lattice::cellBaseHeight(z) + tile.floorOffset });
    }
}

void ColumnScanner::ramps(int x, int y, std::vector<RampSurface>& out) const
{
    out.clear();
    const Lattice& lattice = world_.lattice();
    if (!lattice.inBounds(x, y)) return;

    for (int z = 0; z < lattice.depth(); z++) {
        const Tile& tile = world_.at(lattice.index(x, y, z));
        if (!tile.isRamp() || tile.blocked) continue;
        out.push_back({ z,
                        tile.rampDir,
                        tile.rampBaseHeight,
                        tile.rampTopHeight(),
                        tile.rampBaseHeight + tile.rampRise * 0.5f });
    }
}

}  // namespace game
