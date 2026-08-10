/* CoverModel.hpp — cover as the UI should show it.
 *
 * SINGLE RESPONSIBILITY: combine the three sources of cover into the one
 * grade a player should see:
 *   - authored edges,
 *   - dynamic full cover from an adjacent big unit's hull (XCOM: the tank is
 *     mobile high cover),
 *   - cover derived from raw geometry (LedgeCover).
 * A window reads as waist-high wall.
 *
 * DISPLAY ONLY — movement and LOS consult occupancy and the raycast directly,
 * so this class can never make them behave differently than they look.
 */
#pragma once

#include "game/lattice/Cover.hpp"
#include "game/lattice/Direction.hpp"
#include "game/query/cover/LedgeCover.hpp"
#include "game/world/World.hpp"

namespace game {


class UnitRoster;

class CoverModel {
public:
    CoverModel(const World& world, const UnitRoster& roster)
        : world_(world), roster_(roster), ledges_(world) {}

    Cover displayCover(const Cell& cell, Dir d) const;

private:
    const World&      world_;
    const UnitRoster& roster_;
    LedgeCover        ledges_;
};

}  // namespace game
