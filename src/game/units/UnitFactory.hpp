/* UnitFactory.hpp — the body types this game fields, as values.
 *
 * SINGLE RESPONSIBILITY: assemble a Unit's components into a known kind.
 *
 * This file is what used to be Soldier.hpp/.cpp and Vehicle.hpp/.cpp — two
 * classes, four files, twenty-eight overrides. A body type is now a function
 * that fills in five structs, and the whole difference between infantry and a
 * tank is readable on one screen instead of spread across a hierarchy.
 */
#pragma once

#include "game/units/kinds/Unit.hpp"

#include <memory>

namespace game {

std::unique_ptr<Unit> makeSoldier(Cell position, Team team);
std::unique_ptr<Unit> makeVehicle(Cell position, Team team);

}  // namespace game
