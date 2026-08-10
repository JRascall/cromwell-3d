/* The component behaviour that needs the world.
 *
 * Kept out of the headers so that a component stays a DESCRIPTION — a struct of
 * fields anyone can read and fill in — rather than something that drags the
 * world model into every file that wants to know a unit's footprint.
 */
#include "game/components/BodyComponent.hpp"
#include "game/components/MobilityComponent.hpp"

#include "game/lattice/Lattice.hpp"
#include "game/movement/graph/InfantryMoveGraph.hpp"
#include "game/movement/graph/VehicleMoveGraph.hpp"
#include "game/query/Terrain.hpp"
#include "game/world/World.hpp"

namespace game {

using namespace cromwell;

float highestFloorUnder(const World& world, const Cell& anchor, const Footprint& footprint)
{
    /* A hull bridging a kerb rests on the kerb, not in it. */
    float highest = -1e30f;
    for (const Offset& o : footprint.offsets()) {
        const Tile* tile = world.tryAt(anchor.x + o.dx, anchor.y + o.dy, anchor.z);
        const float offset = tile ? tile->floorOffset : 0.0f;
        if (offset > highest) highest = offset;
    }
    return Lattice::cellBaseHeight(anchor.z) + (highest <= -1e29f ? 0.0f : highest);
}

float BodyComponent::baseHeightAt(const World& world, const Cell& anchor) const
{
    if (baseHeightMode_ == BaseHeightMode::TerrainCentre)
        return Terrain(world).centerHeight(anchor);
    return highestFloorUnder(world, anchor, footprint_);
}

std::unique_ptr<MoveGraph> MobilityComponent::createGraph(const World& world) const
{
    switch (graph_) {
        case MoveGraphKind::Vehicle:  return std::make_unique<VehicleMoveGraph>(world);
        case MoveGraphKind::Infantry: break;
    }
    return std::make_unique<InfantryMoveGraph>(world);
}

}  // namespace game
