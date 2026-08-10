#include "core/query/Standability.hpp"

namespace xcom {

bool Standability::isStandable(int x, int y, int z) const
{
    const Tile* tile = world_.tryAt(x, y, z);
    return tile && !tile->blocked && (tile->hasFloor || tile->isRamp());
}

bool Standability::isFlatStandable(int x, int y, int z) const
{
    const Tile* tile = world_.tryAt(x, y, z);
    return tile && !tile->blocked && tile->hasFloor && !tile->isRamp();
}

}  // namespace xcom
