#include "game/query/Standability.hpp"

namespace game {


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

}  // namespace game
