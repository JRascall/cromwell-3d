#include "core/units/Soldier.hpp"

#include "core/movement/InfantryMoveGraph.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

namespace xcom {

const Footprint& Soldier::footprint() const
{
    static const Footprint kSingle = Footprint::single();
    return kSingle;
}

float Soldier::baseHeight(const World& world) const
{
    return Terrain(world).centerHeight(position());
}

std::unique_ptr<MoveGraph> Soldier::createMoveGraph(const World& world) const
{
    return std::make_unique<InfantryMoveGraph>(world);
}

}  // namespace xcom
