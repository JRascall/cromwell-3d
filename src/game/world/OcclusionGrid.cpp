#include "game/world/OcclusionGrid.hpp"

#include "game/world/World.hpp"

#include <cmath>

namespace game {

namespace {

/* The same epsilon RayCaster used when it read floorOffset directly. A slab
 * within this of the cell boundary IS the boundary; anything else is at a real
 * height and has to be straddle-tested against the ray. */
constexpr float kBoundaryEpsilon = 1e-6f;

}  // namespace

void OcclusionGrid::rebuild(const World& world)
{
    lattice_ = world.lattice();
    words_.assign(static_cast<std::size_t>(lattice_.cellCount()), 0);

    for (int z = 0; z < lattice_.depth(); z++)
    for (int y = 0; y < lattice_.height(); y++)
    for (int x = 0; x < lattice_.width(); x++) {
        const int   index = lattice_.index(x, y, z);
        const Tile& tile  = world.at(index);

        std::uint16_t word = 0;

        /* The two-sided merge happens HERE, once per rebuild, instead of twice
         * per ray step for the rest of the map's life. */
        for (Dir d : kAllDirs) {
            const Edge edge = world.effectiveEdge(x, y, z, d);
            const int  bit  = toIndex(d);

            word |= static_cast<std::uint16_t>(static_cast<int>(edge.cover)
                                               << (bit * occ::kCoverBits));
            if (edge.window)
                word |= static_cast<std::uint16_t>(1u << (occ::kWindowShift + bit));
        }

        const bool ramp         = tile.isRamp();
        const bool offsetFloor  = std::fabs(tile.floorOffset) >= kBoundaryEpsilon;
        const bool boundarySlab = tile.hasFloor && !ramp && !offsetFloor;

        if (boundarySlab) word |= occ::kSlab;
        if (tile.canopy)  word |= occ::kCanopy;

        /* Solid mass blocks by its PHYSICAL top height, which only BlockedMass
         * can work out, so a blocked cell always takes the slow path. */
        if (tile.blocked) word |= occ::kBlocked | occ::kNeedsTile;

        /* A ramp's surface is an inclined plane sampled at the ray's entry and
         * exit points; an offset slab blocks where the ray straddles its real
         * height. Neither is expressible as a bit. */
        if (ramp)                            word |= occ::kNeedsTile;
        if (tile.hasFloor && !ramp && offsetFloor) word |= occ::kNeedsTile;

        words_[static_cast<std::size_t>(index)] = word;
    }
}

}  // namespace game
