/* Standability.hpp — can a body occupy this cell?
 *
 * SINGLE RESPONSIBILITY: decide whether a cell offers footing. Nothing about
 * height, cover or occupancy — just floor-or-ramp and not solid.
 */
#pragma once

#include "core/world/World.hpp"

namespace xcom {

class Standability {
public:
    explicit Standability(const World& world) : world_(world) {}

    /* Any footing at all: a floor slab or a ramp surface. */
    bool isStandable(int x, int y, int z) const;
    bool isStandable(const Cell& c) const { return isStandable(c.x, c.y, c.z); }

    /* Level footing only — excludes ramps. Vehicles and rest positions need
     * this; a body may pass over a flight but not stop on one. */
    bool isFlatStandable(int x, int y, int z) const;
    bool isFlatStandable(const Cell& c) const { return isFlatStandable(c.x, c.y, c.z); }

private:
    const World& world_;
};

}  // namespace xcom
