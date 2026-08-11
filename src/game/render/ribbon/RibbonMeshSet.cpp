#include "game/render/ribbon/RibbonMeshSet.hpp"

#include "cromwell/ribbon/RibbonConstants.hpp"
#include "game/lattice/Lattice.hpp"

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

void RibbonMeshSet::append(const World& world, const LoopSet& loops, Color colour, Ring ring,
                           float width, float lift)
{
    LoopPolyliner polyliner(world);

    for (int index = 0; index < loops.loopCount(); index++) {
        polyliner.build(loops, index, kRibbonWallClearance, kRibbonChamfer, false, polyline_);
        if (polyline_.size() < 2) continue;

        Mesh mesh = stripBuilder_.build(polyline_, width * 0.5f, lift,
                                        loops.loop(index).closed);
        if (mesh.vertexCount == 0) continue;

        /* The lattice conversion happens HERE, once, rather than in the
         * renderer: cromwell culls a strip by comparing its storey against the
         * cutaway, and knowing that a storey is kCellsPerStorey cells is the
         * one thing that would drag a world model into the engine. */
        add(mesh, Lattice::storeyOfZ(loops.loop(index).minZ), colour, ring);
    }
}

}  // namespace game
