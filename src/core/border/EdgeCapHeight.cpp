#include "core/border/EdgeCapHeight.hpp"

#include "core/lattice/Constants.hpp"

namespace xcom {

float EdgeCapHeight::capFor(int cellIndex, Dir d, float ownHeight) const
{
    const Lattice& lattice = world_.lattice();
    const Cell cell = lattice.cellAt(cellIndex);

    const int nx = cell.x + dx(d);
    const int ny = cell.y + dy(d);
    if (!lattice.inBounds(nx, ny)) return ownHeight;

    float best = ownHeight;
    for (int z = 0; z < lattice.depth(); z++) {
        const Tile* tile = world_.tryAt(nx, ny, z);
        if (!tile || tile->blocked || (!tile->hasFloor && !tile->isRamp())) continue;

        const float height = terrain_.centerHeight(nx, ny, z);
        const float step   = height - ownHeight;
        if (step > 0.0f && step <= kWalkStep && height > best) best = height;
    }
    return best;
}

}  // namespace xcom
