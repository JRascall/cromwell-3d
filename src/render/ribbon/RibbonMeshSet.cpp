#include "render/ribbon/RibbonMeshSet.hpp"

#include "render/ribbon/RibbonConstants.hpp"

namespace xcom {

void RibbonMeshSet::clear()
{
    for (Strip& strip : strips_) UnloadMesh(strip.mesh);
    strips_.clear();
}

void RibbonMeshSet::append(const World& world, const LoopSet& loops, Color colour, Ring ring,
                           float width, float lift)
{
    LoopPolyliner polyliner(world);

    for (int index = 0; index < loops.loopCount(); index++) {
        polyliner.build(loops, index, kRibbonWallClearance, kRibbonChamfer, false, polyline_);
        if (polyline_.size() < 2) continue;

        Mesh mesh = stripBuilder_.build(polyline_, width * 0.5f, lift);
        if (mesh.vertexCount == 0) continue;

        strips_.push_back(Strip{ mesh, loops.loop(index).minZ, colour, ring });
    }
}

}  // namespace xcom
