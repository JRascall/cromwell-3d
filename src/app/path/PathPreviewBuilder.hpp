/* PathPreviewBuilder.hpp — turn a route into a drawable polyline.
 *
 * SINGLE RESPONSIBILITY: articulate a path. Drops, ladders and mantles get a
 * vertical riser at the tile boundary, a climb arcs over the cover it hops,
 * and a portal lifts into a teleport arc. Ramps need nothing — they are
 * planes, so a chord IS the surface.
 *
 * A UnitVisitor: infantry and vehicles articulate differently (a hull stays
 * on one surface, so its route is a straight anchor-centre chain), and that
 * difference is dispatched rather than branched on.
 */
#pragma once

#include "core/movement/PathPoint.hpp"
#include "core/movement/ReachField.hpp"
#include "core/query/Terrain.hpp"
#include "core/units/UnitVisitor.hpp"
#include "core/world/World.hpp"

#include <vector>

namespace xcom {

class Unit;

class PathPreviewBuilder : public UnitVisitor {
public:
    explicit PathPreviewBuilder(const World& world) : world_(world), terrain_(world) {}

    /* `route` is a cell chain from PathReconstructor, start first. */
    void build(const Unit& unit, const ReachField& reach,
               const std::vector<int>& route, std::vector<PathPoint>& out);

    void visit(const Soldier& soldier) override;
    void visit(const Vehicle& vehicle) override;

private:
    const World& world_;
    Terrain      terrain_;

    /* set by build() before dispatching */
    const ReachField*       reach_ = nullptr;
    const std::vector<int>* route_ = nullptr;
    std::vector<PathPoint>* out_   = nullptr;
    const Unit*             unit_  = nullptr;
};

}  // namespace xcom
