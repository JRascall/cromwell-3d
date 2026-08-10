#include "core/world/RampSupport.hpp"

#include "core/lattice/Constants.hpp"

#include <cmath>

namespace xcom {

bool RampSupport::isSupported(int x, int y, const Tile& ramp) const
{
    const Dir   uphill   = ramp.rampDir;
    const Dir   downhill = opposite(uphill);
    const float baseH    = ramp.rampBaseHeight;
    const float topH     = ramp.rampTopHeight();

    const int behindX = x + dx(downhill);
    const int behindY = y + dy(downhill);
    const int aheadX  = x + dx(uphill);
    const int aheadY  = y + dy(uphill);

    for (int z = 0; z < world_.lattice().depth(); z++) {
        /* something to step off ONTO behind our low end */
        if (const Tile* behind = world_.tryAt(behindX, behindY, z)) {
            if (!behind->blocked) {
                if (behind->hasFloor && !behind->isRamp() &&
                    std::fabs(Lattice::cellBaseHeight(z) + behind->floorOffset - baseH) <= kWalkStep)
                    return true;
                /* a same-direction flight whose top meets our base */
                if (behind->isRamp() && behind->rampDir == uphill &&
                    std::fabs(behind->rampTopHeight() - baseH) < 1e-5f)
                    return true;
            }
        }
        /* or the landing floor ahead at our top */
        if (const Tile* landing = world_.tryAt(aheadX, aheadY, z)) {
            if (!landing->blocked && landing->hasFloor && !landing->isRamp() &&
                std::fabs(Lattice::cellBaseHeight(z) + landing->floorOffset - topH) <= kWalkStep)
                return true;
        }
    }
    return false;
}

int RampSupport::collapseUnsupported()
{
    const Lattice& lattice = world_.lattice();
    int total = 0;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int z = 0; z < lattice.depth(); z++)
        for (int y = 0; y < lattice.height(); y++)
        for (int x = 0; x < lattice.width(); x++) {
            Tile& tile = world_.at(lattice.index(x, y, z));
            if (!tile.isRamp()) continue;
            if (tile.rampBaseHeight <= 1e-6f) continue;      /* rests on the ground */
            if (isSupported(x, y, tile)) continue;

            tile.rampRise = 0.0f;
            changed = true;
            total++;
        }
    }
    return total;
}

}  // namespace xcom
