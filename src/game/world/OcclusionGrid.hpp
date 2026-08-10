/* OcclusionGrid.hpp — the handful of bits a ray needs, one word per cell.
 *
 * SINGLE RESPONSIBILITY: summarise each cell's blocking geometry into a word
 * that answers a ray's questions without touching the Tile.
 *
 * WHY. RayCaster walks the lattice a cell at a time, and at each one it needs
 * about five yes/no facts: is there a wall on the face I am crossing, a window
 * in it, a floor, a roof, solid mass. To read those it pulls in the whole Tile
 * — 68 bytes of ramp geometry, art tags, portals and material — and for the
 * wall it pulls in TWO, because a face is authored on both sides and merged at
 * read time. Roughly 136 bytes moved to consume five bits.
 *
 * Worse than the volume is the SHAPE. The tile array is 352KB on the demo map,
 * far past what stays in fast cache, and one step up or down the lattice lands
 * 38KB away — a guaranteed wait, every time, and rays step vertically all day.
 *
 * This is the same data at 2 bytes per cell: ~10KB for the demo map, small
 * enough to sit in cache whole, with a vertical step 1.1KB away instead of
 * 38KB. The two-sided edges are merged ONCE here rather than per ray step.
 *
 * COVER IS STORED RAW, NOT PRE-RESOLVED. It would be a shade faster to bake
 * "opaque to sight" and "opaque to sunlight" as separate bits and let the ray
 * pick a mask. That buys one AND over one compare, and it costs four bits per
 * ray rule — so the third rule (smoke, a weapon that shoots through low cover,
 * thermal sight) would need a new bit plane and an edit here. Storing the cover
 * grade keeps the rule where it belongs: a predicate at the call site, changed
 * in one line. See RayRules.
 *
 * THE ESCAPE HATCH IS THE WHOLE DESIGN. Ramps, offset floor slabs and solid
 * mass need real arithmetic against real heights, and no bit field is going to
 * hold that. Those cells set kNeedsTile, and the ray falls through to exactly
 * the code it always ran. The fast path can only ever SKIP work that provably
 * does nothing — it never decides anything on its own — which is what makes
 * this an optimisation rather than a second implementation to keep in step.
 *
 * DERIVED DATA, AND THEREFORE A LIABILITY. It is rebuilt from the World that
 * owns it, on the same event that changes the geometry. See World::occlusion().
 */
#pragma once

#include "game/lattice/Direction.hpp"
#include "game/lattice/Lattice.hpp"

#include <cstdint>
#include <vector>

namespace game {


class World;

/* One word per cell. Bit layout, low to high:
 *
 *   0-7    cover grade per direction, 2 bits each, indexed by toIndex(Dir).
 *          Same values as the Cover enum: 0 none, 1 half, 2 full.
 *   8-11   window on that face
 *   12     floor slab sitting EXACTLY on the cell boundary
 *   13     canopy (roof plane at the top of this cell)
 *   14     solid mass fills the cell
 *   15     needs the Tile: a ramp, an offset floor, or solid mass */
namespace occ {

inline constexpr int kCoverBits   = 2;
inline constexpr int kWindowShift = 8;

inline constexpr std::uint16_t kSlab      = std::uint16_t(1u) << 12;
inline constexpr std::uint16_t kCanopy    = std::uint16_t(1u) << 13;
inline constexpr std::uint16_t kBlocked   = std::uint16_t(1u) << 14;
inline constexpr std::uint16_t kNeedsTile = std::uint16_t(1u) << 15;

/* 0 none, 1 half, 2 full — Cover's own numbering, so a caller compares against
 * static_cast<int>(Cover::Full) rather than a second set of names. */
inline int coverOf(std::uint16_t word, int dirIndex)
{
    return (word >> (dirIndex * kCoverBits)) & 0x3;
}

inline bool hasWindow(std::uint16_t word, int dirIndex)
{
    return (word & (std::uint16_t(1u) << (kWindowShift + dirIndex))) != 0;
}

}  // namespace occ

class OcclusionGrid {
public:
    /* Recomputes every word from the world's tiles. */
    void rebuild(const World& world);

    bool empty() const { return words_.empty(); }

    /* Unchecked — callers that already hold a valid index. */
    std::uint16_t at(int cellIndex) const
    {
        return words_[static_cast<std::size_t>(cellIndex)];
    }

    /* Zero outside the grid, which reads as "nothing here blocks" — the same
     * answer World::tryAt's nullptr produced. */
    std::uint16_t atOrEmpty(int x, int y, int z) const
    {
        if (!lattice_.isValid(x, y, z)) return 0;
        return at(lattice_.index(x, y, z));
    }

    /* How far one step moves through the array. The ray keeps a running index
     * rather than recomputing (z * height + y) * width + x per access. */
    int strideY() const { return lattice_.width(); }
    int strideZ() const { return lattice_.width() * lattice_.height(); }

    const Lattice& lattice() const { return lattice_; }

private:
    Lattice                    lattice_;
    std::vector<std::uint16_t> words_;
};

}  // namespace game
