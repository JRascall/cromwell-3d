/* OccupancyMaskBuilder.hpp — turn a roster into a movement mask.
 *
 * SINGLE RESPONSIBILITY: decide which cells a given mover may not ENTER.
 *
 * Enemy-occupied cells are walls; friendlies are pass-through, because XCOM
 * lets a path cross a squadmate but never end on one. That end-of-move rule
 * is a separate question and lives with the mover, not here.
 */
#pragma once

#include "game/lattice/Lattice.hpp"
#include "game/movement/occupancy/BlockedMask.hpp"

namespace game {


class Unit;
class UnitRoster;

class OccupancyMaskBuilder {
public:
    /* `mover` may be nullptr, in which case every living unit blocks. */
    static void build(const UnitRoster& roster,
                      const Unit* mover,
                      const Lattice& lattice,
                      BlockedMask& out);
};

}  // namespace game
