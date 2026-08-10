#include "core/query/BlockedMass.hpp"

namespace xcom {

std::optional<float> BlockedMass::topHeight(int x, int y, int z) const
{
    const Tile* tile = world_.tryAt(x, y, z);
    if (!tile || !tile->blocked) return std::nullopt;

    const Tile* above = world_.tryAt(x, y, z + 1);
    if (above && above->hasFloor && !above->isRamp())
        return Lattice::cellBaseHeight(z + 1) + above->floorOffset;
    return Lattice::cellBaseHeight(z + 1);
}

}  // namespace xcom
