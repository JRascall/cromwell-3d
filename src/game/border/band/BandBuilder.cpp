#include "game/border/band/BandBuilder.hpp"

namespace game {


void BandBuilder::build(const ReachField& reach,
                        float costCap,
                        const Footprint& footprint,
                        Band& out) const
{
    out.reset(lattice_.cellCount());

    for (int i = 0; i < lattice_.cellCount(); i++) {
        if (reach.cost(i) > costCap) continue;

        if (!footprint.isMultiTile()) {
            out.mark(i);
            continue;
        }
        for (const Cell& cell : footprint.cellsAt(lattice_.cellAt(i)))
            if (lattice_.isValid(cell)) out.mark(lattice_.index(cell));
    }
}

}  // namespace game
