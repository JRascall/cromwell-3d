/* Footprint.hpp — the tiles a body occupies, relative to its anchor.
 *
 * SINGLE RESPONSIBILITY: enumerate occupancy offsets. A soldier is the
 * degenerate case (1x1); the vehicle occupies 2x2 anchored at its cell.
 */
#pragma once

#include "game/lattice/Cell.hpp"

#include <initializer_list>
#include <vector>

namespace game {


struct Offset {
    int dx = 0;
    int dy = 0;
};

class Footprint {
public:
    Footprint() : offsets_{ { 0, 0 } } {}
    Footprint(std::initializer_list<Offset> offsets) : offsets_(offsets) {}

    static Footprint single() { return Footprint{ { 0, 0 } }; }
    static Footprint square2x2() { return Footprint{ { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }; }

    const std::vector<Offset>& offsets() const { return offsets_; }
    int tileCount() const { return static_cast<int>(offsets_.size()); }
    bool isMultiTile() const { return offsets_.size() > 1; }

    /* The absolute cells this footprint covers when anchored at `anchor`. */
    std::vector<Cell> cellsAt(const Cell& anchor) const
    {
        std::vector<Cell> cells;
        cells.reserve(offsets_.size());
        for (const Offset& o : offsets_)
            cells.push_back({ anchor.x + o.dx, anchor.y + o.dy, anchor.z });
        return cells;
    }

private:
    std::vector<Offset> offsets_;
};

}  // namespace game
