/* Tile.hpp — one 96 x 96 x 64uu cell of the world.
 *
 * SINGLE RESPONSIBILITY: hold a cell's authored state. Every derived question
 * — how high is the surface, can you stand here, does this grant cover — is
 * answered by the query classes in core/query, never here.
 */
#pragma once

#include "game/lattice/Cover.hpp"
#include "game/lattice/Direction.hpp"
#include "game/world/Edge.hpp"

#include <array>

namespace game {


struct Tile {
    bool hasFloor            = false;
    bool floorDestructible   = false;
    bool blocked             = false;   /* cell fully occupied by solid geometry     */
    bool blockedDestructible = false;   /* containers/rock stay false, for contrast  */
    bool canopy              = false;   /* roof plane at the TOP of this cell        */

    /* GENUINE sub-cell walk height (a plinth top). Movement, LOS, cover and
     * the border ribbon all honour it. */
    float floorOffset = 0.0f;

    /* ---- decoration, invisible to the simulation ------------------------
     * artDrop is how far the RENDERED surface dips below the tile plane, and
     * it may only ever go DOWN. That is the whole trick behind the border
     * sitting on top of everything: the tile plane is the CEILING of the art,
     * not its average, so a ribbon lifted 4uu above the plane clears the art
     * by construction and never has to trace it.
     *
     * XCOM appears to work the same way — WORLD_BaseHeight is a single global
     * "std. curb height" offset, not a per-tile field, i.e. the base plane is
     * raised BY the kerb height so kerbs are cut down into it. A kerb there is
     * art inside one tile, not a height difference between two.
     *
     * Anything that needs to rise must instead be modelled by dropping its
     * surroundings. Nothing in the sim reads these two fields. */
    float artDrop = 0.0f;
    Art   artTag  = Art::Plain;

    /* Ramp: an inclined PLANE spanning this tile, rampRise > 0 when present.
     * Floor runs from rampBaseHeight (downhill edge, absolute world height) up
     * to rampBaseHeight + rampRise in direction rampDir. */
    Dir   rampDir        = Dir::North;   /* uphill */
    float rampBaseHeight = 0.0f;
    float rampRise       = 0.0f;

    unsigned char portal = 0;            /* 0 = none; equal ids are linked */

    std::array<Edge, kDirCount> edges{};

    bool isRamp() const { return rampRise > 0.0f; }
    float rampTopHeight() const { return rampBaseHeight + rampRise; }

    const Edge& edge(Dir d) const { return edges[toIndex(d)]; }
    Edge&       edge(Dir d)       { return edges[toIndex(d)]; }
};

}  // namespace game
