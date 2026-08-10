/* BandBuilder.hpp — turn a ReachField into a Band.
 *
 * SINGLE RESPONSIBILITY: apply a cost cap, and expand anchors to footprints.
 *
 * A vehicle's search runs in ANCHOR space, so the displayed band is the UNION
 * of each reachable anchor's footprint — otherwise the ribbon outlines anchor
 * positions and reads a tile short on two sides.
 */
#pragma once

#include "game/border/band/Band.hpp"
#include "game/lattice/Lattice.hpp"
#include "game/movement/search/ReachField.hpp"
#include "game/units/Footprint.hpp"

namespace game {


class BandBuilder {
public:
    explicit BandBuilder(const Lattice& lattice) : lattice_(lattice) {}

    /* Cells whose cost is at or under `costCap`, each expanded by
     * `footprint` (pass Footprint::single() for infantry). */
    void build(const ReachField& reach,
               float costCap,
               const Footprint& footprint,
               Band& out) const;

private:
    Lattice lattice_;
};

}  // namespace game
