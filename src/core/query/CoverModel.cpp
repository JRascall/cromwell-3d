#include "core/query/CoverModel.hpp"

#include "core/units/UnitRoster.hpp"

namespace xcom {

Cover CoverModel::displayCover(const Cell& cell, Dir d) const
{
    const Edge edge = world_.effectiveEdge(cell, d);

    /* a window is a full wall you can shoot through — it reads as waist high */
    if (edge.cover == Cover::Full) return edge.window ? Cover::Half : Cover::Full;

    /* a big unit's hull beside you is high cover */
    const Cell neighbour{ cell.x + dx(d), cell.y + dy(d), cell.z };
    if (const Unit* occupant = roster_.occupantAt(neighbour))
        if (occupant->grantsHullCover()) return Cover::Full;

    const Cover ledge = ledges_.at(cell, d);
    if (ledge == Cover::Full) return Cover::Full;
    if (ledge == Cover::Half || edge.cover == Cover::Half) return Cover::Half;
    return edge.cover;
}

}  // namespace xcom
