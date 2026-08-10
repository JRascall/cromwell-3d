/* Vehicle.hpp — a 2x2 hull.
 *
 * SINGLE RESPONSIBILITY: answer Unit's virtuals the vehicle way. XCOM treats
 * a big unit as MOBILE HIGH COVER: its hull blocks sight and grants full cover
 * to infantry beside it. It moves in anchor space — no stairs, ladders, drops
 * or portals — and rests on the HIGHEST floor under its footprint (a hull
 * straddling road and kerb sits on the kerb).
 */
#pragma once

#include "core/units/Unit.hpp"

namespace xcom {

class Vehicle : public Unit {
public:
    Vehicle(Cell position, Team team) : Unit(position, team) {}

    const Footprint& footprint() const override;
    bool  blocksLineOfSight() const override { return true; }
    float hullHeight() const override;
    bool  canRestOnRamp() const override { return false; }
    float baseHeight(const World& world) const override;

    bool crushesHalfCover()  const override { return true; }
    bool showsCoverShields() const override { return false; }   /* armour, not cover */
    bool grantsHullCover()   const override { return true; }
    bool leavesWreckage()    const override { return true; }

    std::unique_ptr<MoveGraph> createMoveGraph(const World& world) const override;

    std::string displayName() const override { return "tank"; }
    std::string hudLabel() const override { return "TANK 2x2"; }
    std::string selectionDescription() const override
    {
        return "selected the TANK (2x2 - no stairs, no cover, crushes half cover)";
    }

    void accept(UnitVisitor& visitor) const override { visitor.visit(*this); }

    /* Where a multi-tile body's base sits: the HIGHEST floor under the
     * footprint. Exposed statically because hover plates and path previews
     * need it for candidate anchors the unit is not standing on yet. */
    static float footprintBaseHeight(const World& world, const Cell& anchor,
                                     const Footprint& footprint);
};

}  // namespace xcom
