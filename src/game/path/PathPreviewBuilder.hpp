/* PathPreviewBuilder.hpp — turn a route into a drawable polyline.
 *
 * SINGLE RESPONSIBILITY: articulate a path. Drops, ladders and mantles get a
 * vertical riser at the tile boundary, a climb arcs over the cover it hops,
 * and a portal lifts into a teleport arc. Ramps need nothing — they are
 * planes, so a chord IS the surface.
 *
 * Infantry and vehicles articulate differently (a hull stays
 * on one surface, so its route is a straight anchor-centre chain), and that
 * difference is dispatched rather than branched on.
 */
#pragma once

#include "game/movement/search/PathPoint.hpp"
#include "game/movement/search/ReachField.hpp"
#include "game/query/Terrain.hpp"
#include "game/world/World.hpp"

#include <vector>

namespace game {


class Unit;

class PathPreviewBuilder {
public:
    explicit PathPreviewBuilder(const World& world) : world_(world), terrain_(world) {}

    /* `route` is a cell chain from PathReconstructor, start first. */
    void build(const Unit& unit, const ReachField& reach,
               const std::vector<int>& route, std::vector<PathPoint>& out);

private:
    /* One per PathStyle. Articulated walks cell centres and turns with the
     * terrain; anchored runs through the middle of a multi-tile footprint. */
    void buildArticulated();
    void buildAnchored();

public:

private:
    const World& world_;
    Terrain      terrain_;

    /* set by build() before either shape runs */
    const ReachField*       reach_ = nullptr;
    const std::vector<int>* route_ = nullptr;
    std::vector<PathPoint>* out_   = nullptr;
    const Unit*             unit_  = nullptr;
};

}  // namespace game
