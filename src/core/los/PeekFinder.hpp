/* PeekFinder.hpp — step-out peeking, the way XCOM does it.
 *
 * SINGLE RESPONSIBILITY: find the tiles a unit hugging cover can lean out to.
 *
 * Hugging full cover, visibility is also evaluated from the tiles beside the
 * cover. The peek tile needs CLEAR AIRSPACE but NOT a walkable floor — leaning
 * out over a railing end is fine. Crucially the cover must END there: if the
 * same wall continues across the peek tile's facing there is no corner to lean
 * around, unless that continuation is a WINDOW (slide over and shoot through
 * the glass).
 */
#pragma once

#include "core/world/World.hpp"

#include <vector>

namespace xcom {

class PeekFinder {
public:
    explicit PeekFinder(const World& world) : world_(world) {}

    std::vector<Cell> peekPositions(const Cell& from) const;

private:
    const World& world_;
};

}  // namespace xcom
