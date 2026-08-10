/* BlockedMass.hpp — how tall is the solid geometry in this cell?
 *
 * SINGLE RESPONSIBILITY: report the top surface of a blocked cell.
 *
 * A blocked cell fills its 64uu cell, UNLESS a walkable floor sits directly
 * above (a plinth), in which case the mass stops at that floor's height. That
 * distinction is what lets a plinth be chest-high cover you can shoot over
 * while a container is a full-cell obstruction.
 */
#pragma once

#include "core/world/World.hpp"

#include <optional>

namespace xcom {

class BlockedMass {
public:
    explicit BlockedMass(const World& world) : world_(world) {}

    /* The mass's top height, or nullopt when the cell is not blocked. */
    std::optional<float> topHeight(int x, int y, int z) const;
    std::optional<float> topHeight(const Cell& c) const { return topHeight(c.x, c.y, c.z); }

private:
    const World& world_;
};

}  // namespace xcom
