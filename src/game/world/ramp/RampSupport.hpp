/* RampSupport.hpp — do these staircases still have anything holding them up?
 *
 * SINGLE RESPONSIBILITY: run the structural pass that collapses unsupported
 * ramps, and nothing else.
 *
 * An elevated ramp stays up only while something holds it: a floor near its
 * low end behind it, a same-direction flight whose top meets our base, or the
 * landing floor ahead at our top. The sweep runs to a FIXPOINT, so destroying
 * the bottom of a chain cascades all the way up it.
 */
#pragma once

#include "game/world/World.hpp"

namespace game {


class RampSupport {
public:
    explicit RampSupport(World& world) : world_(world) {}

    /* Returns the number of ramps collapsed. */
    int collapseUnsupported();

private:
    bool isSupported(int x, int y, const Tile& ramp) const;

    World& world_;
};

}  // namespace game
