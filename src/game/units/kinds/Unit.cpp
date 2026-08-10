#include "game/units/kinds/Unit.hpp"

#include "game/movement/graph/MoveGraph.hpp"

namespace game {

std::unique_ptr<MoveGraph> Unit::createMoveGraph(const World& world) const
{
    return mobility().createGraph(world);
}


bool Unit::occupies(const Cell& cell) const
{
    if (cell.z != position_.z) return false;
    for (const Offset& o : footprint().offsets())
        if (position_.x + o.dx == cell.x && position_.y + o.dy == cell.y) return true;
    return false;
}

}  // namespace game
