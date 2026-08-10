/* LadderQuery.hpp — where does climbing this face land you?
 *
 * SINGLE RESPONSIBILITY: resolve a ladder edge into its landing cell. The
 * landing level is DERIVED, never authored: the map marks a climbable face and
 * this walks up the shaft to find the first mountable floor.
 */
#pragma once

#include "game/lattice/Direction.hpp"
#include "game/query/Terrain.hpp"
#include "game/world/World.hpp"

#include <optional>

namespace game {


struct LadderHit {
    Cell  cell;         /* where the climber ends up */
    float rise = 0.0f;  /* real height gained        */
};

class LadderQuery {
public:
    explicit LadderQuery(const World& world) : world_(world), terrain_(world) {}

    /* nullopt when there is no ladder on that face, or nothing mountable
     * above it — a ceiling or a canopy over the climber stops the ascent. */
    std::optional<LadderHit> targetFrom(int x, int y, int z, Dir d) const;

private:
    const World& world_;
    Terrain      terrain_;
};

}  // namespace game
