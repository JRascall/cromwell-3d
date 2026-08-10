/* LedgeCover.hpp — cover derived from raw geometry.
 *
 * SINGLE RESPONSIBILITY: grade the height step to a neighbouring cell as
 * cover. No authored edge is needed — the cover IS the geometry, so a map
 * editor cannot make the two disagree.
 */
#pragma once

#include "core/lattice/Cover.hpp"
#include "core/lattice/Direction.hpp"
#include "core/query/BlockedMass.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

namespace xcom {

class LedgeCover {
public:
    explicit LedgeCover(const World& world)
        : world_(world), terrain_(world), mass_(world) {}

    Cover at(int x, int y, int z, Dir d) const;
    Cover at(const Cell& c, Dir d) const { return at(c.x, c.y, c.z, d); }

private:
    const World& world_;
    Terrain      terrain_;
    BlockedMass  mass_;
};

}  // namespace xcom
