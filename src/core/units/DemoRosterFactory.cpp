#include "core/units/DemoRosterFactory.hpp"

#include "core/lattice/Lattice.hpp"
#include "core/units/Soldier.hpp"
#include "core/units/UnitRoster.hpp"
#include "core/units/Vehicle.hpp"

namespace xcom {

void DemoRosterFactory::build(UnitRoster& roster)
{
    roster.clear();
    roster.add(std::make_unique<Soldier>(Cell{ 11, 10, 0 }, Team::Player));
    roster.add(std::make_unique<Vehicle>(Cell{ 16,  4, 0 }, Team::Player));
    roster.add(std::make_unique<Soldier>(Cell{  2, 16, 0 }, Team::Enemy));
    /* the second enemy stands on the rooftop — storey 2, cell 6 */
    roster.add(std::make_unique<Soldier>(Cell{ 6, 18, Lattice::storeyBaseZ(2) }, Team::Enemy));
}

}  // namespace xcom
