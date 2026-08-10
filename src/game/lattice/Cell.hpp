/* Cell.hpp — an integer lattice coordinate.
 *
 * SINGLE RESPONSIBILITY: carry an (x, y, z) triple as one value, so functions
 * stop returning three out-parameters the way xcUnIdx had to.
 */
#pragma once

namespace game {


struct Cell {
    int x = 0;
    int y = 0;
    int z = 0;

    friend constexpr bool operator==(const Cell& a, const Cell& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(const Cell& a, const Cell& b) { return !(a == b); }
};

}  // namespace game
