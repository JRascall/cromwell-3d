/* World.hpp — tile storage.
 *
 * SINGLE RESPONSIBILITY: own the tiles and hand them out. Writing them is
 * MapAuthor's job; interpreting them is core/query's.
 *
 * effectiveEdge() lives here because the two-sided edge representation is a
 * storage detail — every face is written to both adjacent tiles, and this is
 * the accessor that hides that from everyone else.
 */
#pragma once

#include "core/lattice/Lattice.hpp"
#include "core/world/Tile.hpp"

#include <vector>

namespace xcom {

class World {
public:
    explicit World(const Lattice& lattice = Lattice());

    const Lattice& lattice() const { return lattice_; }
    int cellCount() const { return lattice_.cellCount(); }

    /* Resets every tile to its default-constructed state. */
    void clear();

    /* Unchecked flat access — callers that already hold a valid index. */
    Tile&       at(int index)       { return tiles_[static_cast<std::size_t>(index)]; }
    const Tile& at(int index) const { return tiles_[static_cast<std::size_t>(index)]; }

    Tile&       at(const Cell& c)       { return at(lattice_.index(c)); }
    const Tile& at(const Cell& c) const { return at(lattice_.index(c)); }

    /* nullptr outside the grid, so callers test one pointer instead of bounds. */
    Tile*       tryAt(int x, int y, int z);
    const Tile* tryAt(int x, int y, int z) const;
    const Tile* tryAt(const Cell& c) const { return tryAt(c.x, c.y, c.z); }

    /* The face as physics sees it, merged from both sides. Out-of-grid
     * neighbours contribute a default (empty) edge. */
    Edge effectiveEdge(int x, int y, int z, Dir d) const;
    Edge effectiveEdge(const Cell& c, Dir d) const { return effectiveEdge(c.x, c.y, c.z, d); }

private:
    Lattice           lattice_;
    std::vector<Tile> tiles_;
};

}  // namespace xcom
