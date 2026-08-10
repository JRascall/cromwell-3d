/* Lattice.hpp — the grid's extents and the arithmetic over them.
 *
 * SINGLE RESPONSIBILITY: convert between coordinates, flat indices and world
 * heights. It holds no tiles and answers no gameplay question.
 *
 * Extents are runtime values rather than macros, so a map is free to be any
 * size; every buffer sized against a map asks the lattice for cellCount().
 */
#pragma once

#include "game/lattice/Cell.hpp"
#include "game/lattice/Constants.hpp"

#include <cmath>

namespace game {


class Lattice {
public:
    Lattice(int width = kDefaultGridWidth,
            int height = kDefaultGridHeight,
            int storeys = kDefaultStoreyCount);

    int width()   const { return width_; }
    int height()  const { return height_; }
    int storeys() const { return storeys_; }
    int depth()   const { return depth_; }        /* z cells = storeys * 3 */
    int cellCount() const { return cellCount_; }

    /* Row-major with z slowest, matching the original xcIdx. */
    int index(int x, int y, int z) const { return (z * height_ + y) * width_ + x; }
    int index(const Cell& c) const { return index(c.x, c.y, c.z); }

    Cell cellAt(int index) const
    {
        Cell c;
        c.x = index % width_;
        c.y = (index / width_) % height_;
        c.z = index / (width_ * height_);
        return c;
    }

    bool inBounds(int x, int y) const
    {
        return x >= 0 && x < width_ && y >= 0 && y < height_;
    }
    bool isValid(int x, int y, int z) const
    {
        return inBounds(x, y) && z >= 0 && z < depth_;
    }
    bool isValid(const Cell& c) const { return isValid(c.x, c.y, c.z); }

    /* ---- height arithmetic: independent of extents, hence static -------- */
    static float cellBaseHeight(int z) { return static_cast<float>(z) * kCellHeight; }
    static int   storeyOfZ(int z) { return z / kCellsPerStorey; }
    static int   storeyBaseZ(int storey) { return storey * kCellsPerStorey; }
    static float storeyBaseHeight(int storey) { return static_cast<float>(storey) * kStoreyHeight; }

    /* Which z cell owns an absolute world height, and the leftover offset.
     * Heights below the grid floor (a sunken road) clamp to cell 0 and keep a
     * negative offset rather than falling off the bottom of the lattice. */
    int cellOfHeight(float height, float* offsetOut = nullptr) const;

private:
    int width_;
    int height_;
    int storeys_;
    int depth_;
    int cellCount_;
};

}  // namespace game
