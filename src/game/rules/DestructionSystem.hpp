/* DestructionSystem.hpp — the grenade.
 *
 * SINGLE RESPONSIBILITY: edit the DATA. Everything else — the baked mesh, the
 * reachability field, the border ribbons — re-derives from the world
 * afterwards, which is why this class knows nothing about any of them.
 *
 * A killed vehicle stamps its wreck INTO the data: the hull becomes half cover,
 * so cover, LOS and pathing all pick it up with no special case.
 */
#pragma once

#include "game/units/roster/UnitRoster.hpp"
#include "game/world/World.hpp"

namespace game {


struct BlastReport {
    int dataEdits   = 0;
    int unitsKilled = 0;
};

class DestructionSystem {
public:
    static constexpr float kBlastRadius = 2.2f;

    DestructionSystem(World& world, UnitRoster& roster)
        : world_(world), roster_(roster) {}

    BlastReport detonate(const Cell& centre);

private:
    int destroyTerrain(const Cell& centre);
    int killUnits(const Cell& centre);
    void stampWreck(const Unit& vehicle);

    World&      world_;
    UnitRoster& roster_;
};

}  // namespace game
