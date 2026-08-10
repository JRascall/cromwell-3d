#include "game/rules/RestPlacement.hpp"

namespace game {


bool RestPlacement::canRest(const Unit& unit, const MoveGraph& graph,
                            const BlockedMask& blocked, const Cell& cell) const
{
    if (!world_.lattice().isValid(cell)) return false;
    if (!graph.isTraversable(cell, &blocked)) return false;

    if (!unit.canRestOnRamp() && world_.at(cell).isRamp()) return false;

    /* Multi-tile hulls stop here: anchor validity already demanded flat
     * standable floor across the whole footprint and no full cover through it,
     * and enemy occupancy is in the mask. */
    if (unit.footprint().isMultiTile()) return true;

    return roster_.occupantAt(cell, &unit) == nullptr;
}

}  // namespace game
