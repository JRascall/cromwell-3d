#include "game/units/roster/DemoRosterFactory.hpp"

#include "game/lattice/Lattice.hpp"
#include "game/units/UnitFactory.hpp"
#include "game/units/roster/UnitRoster.hpp"

namespace game {


void DemoRosterFactory::build(UnitRoster& roster)
{
    roster.clear();
    roster.add(makeSoldier(Cell{ 11, 10, 0 }, Team::Player));
    roster.add(makeVehicle(Cell{ 16,  4, 0 }, Team::Player));
    roster.add(makeSoldier(Cell{  2, 16, 0 }, Team::Enemy));
    /* the second enemy stands on the rooftop — storey 2, cell 6 */
    roster.add(makeSoldier(Cell{ 6, 18, Lattice::storeyBaseZ(2) }, Team::Enemy));
}

}  // namespace game
