/* Edge.hpp — one tile face.
 *
 * SINGLE RESPONSIBILITY: hold a face's properties, and combine the two
 * records that describe the same physical face from either side.
 *
 * An edge is stored on BOTH adjacent tiles; combine() merges them so gameplay
 * code never cares which side authored it.
 */
#pragma once

#include "game/lattice/Cover.hpp"

namespace game {


struct Edge {
    Cover cover        = Cover::None;
    bool  destructible = false;
    bool  window       = false;   /* full cover but SEE-THROUGH in the band */
    bool  ladder       = false;   /* climbable face; landing is derived     */

    /* The face as physics sees it: strongest cover wins, every boolean is a
     * union. Either side authoring a property makes it true of the face. */
    static Edge combine(const Edge& a, const Edge& b)
    {
        Edge out;
        out.cover        = strongest(a.cover, b.cover);
        out.destructible = a.destructible || b.destructible;
        out.window       = a.window || b.window;
        out.ladder       = a.ladder || b.ladder;
        return out;
    }

    bool blocksMovement() const { return cover == Cover::Full; }
};

}  // namespace game
