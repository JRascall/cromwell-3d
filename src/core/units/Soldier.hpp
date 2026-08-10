/* Soldier.hpp — a 1x1 infantry body.
 *
 * SINGLE RESPONSIBILITY: answer Unit's virtuals the infantry way. Transparent
 * to sight, walks the full infantry graph, rests anywhere but a staircase.
 */
#pragma once

#include "core/units/Unit.hpp"

namespace xcom {

class Soldier : public Unit {
public:
    Soldier(Cell position, Team team) : Unit(position, team) {}

    const Footprint& footprint() const override;
    bool  blocksLineOfSight() const override { return false; }
    float hullHeight() const override { return 0.0f; }
    bool  canRestOnRamp() const override { return false; }
    float baseHeight(const World& world) const override;

    bool crushesHalfCover()  const override { return false; }
    bool showsCoverShields() const override { return true; }
    bool grantsHullCover()   const override { return false; }
    bool leavesWreckage()    const override { return false; }

    std::unique_ptr<MoveGraph> createMoveGraph(const World& world) const override;

    std::string displayName() const override { return "soldier"; }
    std::string hudLabel() const override { return "soldier"; }
    std::string selectionDescription() const override { return "selected the soldier"; }

    void accept(UnitVisitor& visitor) const override { visitor.visit(*this); }
};

}  // namespace xcom
