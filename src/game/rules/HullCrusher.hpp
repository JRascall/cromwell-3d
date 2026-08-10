/* HullCrusher.hpp — a 2x2 hull flattens what it drives over.
 *
 * SINGLE RESPONSIBILITY: clear destructible half cover along a vehicle's SWEPT
 * path — the internal edges of every anchor footprint it passed through.
 *
 * Full cover is never crushed; VehicleMoveGraph already refuses to route
 * through it, so anything the hull meets is by construction crushable.
 */
#pragma once

#include "game/world/World.hpp"

#include <vector>

namespace game {


class HullCrusher {
public:
    explicit HullCrusher(World& world) : world_(world) {}

    /* `route` is a cell chain of ANCHOR positions. Returns how many edges
     * were crushed. */
    int crushAlong(const std::vector<int>& route);

private:
    World& world_;
};

}  // namespace game
