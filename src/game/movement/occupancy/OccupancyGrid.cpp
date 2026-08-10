#include "game/movement/occupancy/OccupancyGrid.hpp"

namespace game {


void OccupancyGrid::bind(const Lattice& lattice)
{
    slots_.assign(static_cast<std::size_t>(lattice.cellCount()), kEmpty);
}

void OccupancyGrid::clear()
{
    slots_.assign(slots_.size(), kEmpty);
}

}  // namespace game
