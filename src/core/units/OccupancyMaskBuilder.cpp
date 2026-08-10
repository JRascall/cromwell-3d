#include "core/units/OccupancyMaskBuilder.hpp"

#include "core/units/UnitRoster.hpp"

namespace xcom {

void OccupancyMaskBuilder::build(const UnitRoster& roster,
                                 const Unit* mover,
                                 const Lattice& lattice,
                                 BlockedMask& out)
{
    out.reset(lattice.cellCount());

    for (const std::unique_ptr<Unit>& unit : roster) {
        if (unit->isDead() || unit.get() == mover) continue;
        if (mover && unit->team() == mover->team()) continue;   /* friendlies pass through */

        for (const Cell& cell : unit->footprint().cellsAt(unit->position()))
            if (lattice.isValid(cell)) out.block(lattice.index(cell));
    }
}

}  // namespace xcom
