#include "game/state/GameState.hpp"

#include "game/los/VisibilityComputer.hpp"
#include "game/query/BlockedMass.hpp"
#include "game/query/Terrain.hpp"
#include "game/units/roster/DemoRosterFactory.hpp"
#include "game/units/roster/OccupancyMaskBuilder.hpp"
#include "game/components/BodyComponent.hpp"
#include "game/world/authoring/DemoMapFactory.hpp"

namespace game {


GameState::GameState()
    : pathfinder_(world_.lattice()), bandBuilder_(world_.lattice())
{
    reset();
}

void GameState::reset()
{
    DemoMapFactory::build(world_);
    DemoRosterFactory::build(roster_);
    selectedIndex_ = 0;
    selectIndex(0);
}

void GameState::selectIndex(int index)
{
    if (index < 0 || index >= roster_.size()) index = 0;
    selectedIndex_ = index;
    /* the graph a body moves on is a property of the body */
    moveGraph_ = selectedUnit().createMoveGraph(world_);
}

void GameState::selectUnit(const Unit* unit)
{
    const int index = roster_.indexOf(unit);
    if (index >= 0) selectIndex(index);
}

void GameState::recompute()
{
    Unit& unit = selectedUnit();

    OccupancyMaskBuilder::build(roster_, &unit, world_.lattice(), blockedMask_);
    pathfinder_.search(*moveGraph_, unit.position(), sprintBudget(), &blockedMask_, reach_);

    if (losMode_) {
        VisibilityComputer(world_, roster_, &unit).compute(unit.position(), visibility_);
    } else {
        visibility_.reset(world_.lattice().cellCount());
    }
}

void GameState::buildBand(float costCap, Band& out) const
{
    bandBuilder_.build(reach_, costCap, selectedUnit().footprint(), out);
}

float GameState::hoverPlateHeight(const Cell& cell) const
{
    const Unit& unit = selectedUnit();

    if (unit.footprint().isMultiTile())
        return highestFloorUnder(world_, cell, unit.footprint());

    if (const std::optional<float> top = BlockedMass(world_).topHeight(cell)) return *top;
    return Terrain(world_).centerHeight(cell);
}

}  // namespace game
