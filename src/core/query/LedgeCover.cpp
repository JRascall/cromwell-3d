#include "core/query/LedgeCover.hpp"

#include "core/lattice/Constants.hpp"

namespace xcom {

Cover LedgeCover::at(int x, int y, int z, Dir d) const
{
    const int nx = x + dx(d);
    const int ny = y + dy(d);
    if (!world_.lattice().inBounds(nx, ny)) return Cover::None;

    const std::optional<float> top = mass_.topHeight(nx, ny, z);
    if (!top) return Cover::None;

    const float step = *top - terrain_.centerHeight(x, y, z);
    if (step >= kLedgeFull) return Cover::Full;
    if (step >= kLedgeHalf) return Cover::Half;
    return Cover::None;
}

}  // namespace xcom
