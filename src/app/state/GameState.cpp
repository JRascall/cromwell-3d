#include "app/state/GameState.hpp"

#include "core/los/VisibilityComputer.hpp"
#include "core/query/BlockedMass.hpp"
#include "core/query/Terrain.hpp"
#include "core/units/DemoRosterFactory.hpp"
#include "core/units/OccupancyMaskBuilder.hpp"
#include "core/units/Vehicle.hpp"
#include "core/world/DemoMapFactory.hpp"

namespace xcom {

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
        return Vehicle::footprintBaseHeight(world_, cell, unit.footprint());

    if (const std::optional<float> top = BlockedMass(world_).topHeight(cell)) return *top;
    return Terrain(world_).centerHeight(cell);
}

}  // namespace xcom
