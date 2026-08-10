/* RibbonMeshSet.hpp — this game's ribbon strips, built from border loops.
 *
 * SINGLE RESPONSIBILITY: turn the reach field's border loops into strip
 * meshes. Owning and drawing them is cromwell's job — the storage is
 * cromwell::StripSet, which this fills.
 *
 * WHY IT IS A StripSet RATHER THAN HAVING ONE. cromwell::RibbonRenderer draws
 * a StripSet, and every call site here passes this object to it. Inheriting
 * keeps those call sites unchanged and says the true thing: this IS a set of
 * strips, with one extra way of filling it that happens to need a World.
 */
#pragma once

#include "raylib.h"

#include "cromwell/ribbon/Ring.hpp"
#include "cromwell/ribbon/StripSet.hpp"
#include "game/border/loop/LoopSet.hpp"
#include "game/render/ribbon/StripMeshBuilder.hpp"
#include "game/world/World.hpp"

#include <vector>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

class RibbonMeshSet : public StripSet {
public:
    RibbonMeshSet() = default;

    /* Appends strips for every loop. Both rings get identical treatment —
     * the sprint ring used to be nudged 0.06 further in to stop the two
     * z-fighting, which accidentally became the only reason it cleared wall
     * art while the move ring vanished inside it. The wall clearance in
     * LoopPolyliner is the real fix. */
    void append(const World& world, const LoopSet& loops, Color colour, Ring ring,
                float width, float lift);

private:
    StripMeshBuilder         stripBuilder_;
    std::vector<BorderPoint> polyline_;   /* scratch, reused across loops */
};

}  // namespace game
