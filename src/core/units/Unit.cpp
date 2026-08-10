#include "core/units/Unit.hpp"

namespace xcom {

bool Unit::occupies(const Cell& cell) const
{
    if (cell.z != position_.z) return false;
    for (const Offset& o : footprint().offsets())
        if (position_.x + o.dx == cell.x && position_.y + o.dy == cell.y) return true;
    return false;
}

}  // namespace xcom
