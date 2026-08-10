#include "core/units/Vehicle.hpp"

#include "core/lattice/Constants.hpp"
#include "core/movement/VehicleMoveGraph.hpp"
#include "core/world/World.hpp"

namespace xcom {

const Footprint& Vehicle::footprint() const
{
    static const Footprint kSquare = Footprint::square2x2();
    return kSquare;
}

float Vehicle::hullHeight() const { return kVehicleLosHeight; }

float Vehicle::footprintBaseHeight(const World& world, const Cell& anchor,
                                   const Footprint& footprint)
{
    float highest = -1e30f;
    for (const Offset& o : footprint.offsets()) {
        const Tile* tile = world.tryAt(anchor.x + o.dx, anchor.y + o.dy, anchor.z);
        const float offset = tile ? tile->floorOffset : 0.0f;
        if (offset > highest) highest = offset;
    }
    return Lattice::cellBaseHeight(anchor.z) + (highest <= -1e29f ? 0.0f : highest);
}

float Vehicle::baseHeight(const World& world) const
{
    return footprintBaseHeight(world, position(), footprint());
}

std::unique_ptr<MoveGraph> Vehicle::createMoveGraph(const World& world) const
{
    return std::make_unique<VehicleMoveGraph>(world);
}

}  // namespace xcom
