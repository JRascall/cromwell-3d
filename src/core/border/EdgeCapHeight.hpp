/* EdgeCapHeight.hpp — how high the line sits where two tiles differ.
 *
 * SINGLE RESPONSIBILITY: compute the cap height for one boundary face.
 *
 * The ribbon straddles the tile boundary — it has no inset, because in XCOM's
 * model nothing lives on the boundary: a wall is a property of the FACE
 * (TileDataBlocksPathing*), units sit at tile centres, cover is inside the
 * tile. But ART has thickness, and our floor slabs are drawn a full tile wide,
 * so where the tile ACROSS the boundary is higher its slab buries the
 * overhanging half of the line.
 *
 * So the line is capped to the higher of the two surfaces it straddles: it
 * sits ON the kerb rather than under its lip. That is what "the ribbon sticks
 * to the kerb" looks like in game.
 *
 * Gated to a single step (kWalkStep) so a wall, a plinth or a storey above can
 * never drag the line up with it — only genuine micro-relief can.
 */
#pragma once

#include "core/lattice/Direction.hpp"
#include "core/query/Terrain.hpp"
#include "core/world/World.hpp"

namespace xcom {

class EdgeCapHeight {
public:
    explicit EdgeCapHeight(const World& world) : world_(world), terrain_(world) {}

    /* Ramps keep no cap — their height is sampled from the inclined plane. */
    static constexpr float kNoCap = -1.0e30f;

    float capFor(int cellIndex, Dir d, float ownHeight) const;

private:
    const World& world_;
    Terrain      terrain_;
};

}  // namespace xcom
