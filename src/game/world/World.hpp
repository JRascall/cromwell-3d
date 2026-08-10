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

#include "game/lattice/Lattice.hpp"
#include "game/world/OcclusionGrid.hpp"
#include "game/world/Tile.hpp"

#include <vector>

namespace game {


class World {
public:
    explicit World(const Lattice& lattice = Lattice());

    const Lattice& lattice() const { return lattice_; }
    int cellCount() const { return lattice_.cellCount(); }

    /* Resets every tile to its default-constructed state. */
    void clear();

    /* Unchecked flat access — callers that already hold a valid index.
     *
     * THE NON-CONST ONES INVALIDATE THE OCCLUSION GRID, because handing out a
     * mutable Tile& is handing out permission to change the geometry, and
     * there is no later moment at which that can be noticed. Conservative on
     * purpose: a caller that takes the reference and reads it still costs a
     * rebuild, which is the right way round for a derived cache. Ask for a
     * const World when you only mean to read. */
    Tile&       at(int index)       { occlusionDirty_ = true;
                                      return tiles_[static_cast<std::size_t>(index)]; }
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

    /* ---- the ray caster's view of the same tiles ------------------------
     * Rebuilt on demand after any mutation. See OcclusionGrid.hpp for what it
     * holds and why it exists.
     *
     * OWNED HERE, NOT BY THE CASTER, so it cannot go stale: the only way to
     * change the geometry is through this class, and the only way to read the
     * summary is through this call. There is no third party to keep in step.
     *
     * NOT THREAD SAFE ON FIRST CALL. It is const and rebuilds lazily, so two
     * threads racing to be first would both rebuild into the same vector. Warm
     * it on the calling thread before spawning workers — SunBaker::bakeSlots
     * does exactly that, and it is the only place in the tree that fans a bake
     * across cores. */
    const OcclusionGrid& occlusion() const;

private:
    Lattice           lattice_;
    std::vector<Tile> tiles_;

    mutable OcclusionGrid occlusion_;
    mutable bool          occlusionDirty_ = true;
};

}  // namespace game
