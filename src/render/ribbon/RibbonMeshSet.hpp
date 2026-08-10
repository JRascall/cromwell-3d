/* RibbonMeshSet.hpp — the built ribbon strips.
 *
 * SINGLE RESPONSIBILITY: own the strip meshes and their per-strip metadata,
 * and rebuild them from loops. Drawing is RibbonRenderer's job.
 *
 * RAII: the destructor unloads every mesh.
 */
#pragma once

#include "raylib.h"

#include "core/border/LoopSet.hpp"
#include "core/world/World.hpp"
#include "render/ribbon/Ring.hpp"
#include "render/ribbon/StripMeshBuilder.hpp"

#include <vector>

namespace xcom {

class RibbonMeshSet {
public:
    struct Strip {
        Mesh  mesh = { 0 };
        int   minZ = 0;
        Color colour = WHITE;
        Ring  ring = Ring::Move;
    };

    ~RibbonMeshSet() { clear(); }

    RibbonMeshSet(const RibbonMeshSet&) = delete;
    RibbonMeshSet& operator=(const RibbonMeshSet&) = delete;
    RibbonMeshSet() = default;

    void clear();

    /* Appends strips for every loop. Both rings get identical treatment —
     * the sprint ring used to be nudged 0.06 further in to stop the two
     * z-fighting, which accidentally became the only reason it cleared wall
     * art while the move ring vanished inside it. The wall clearance in
     * LoopPolyliner is the real fix. */
    void append(const World& world, const LoopSet& loops, Color colour, Ring ring,
                float width, float lift);

    const std::vector<Strip>& strips() const { return strips_; }
    bool empty() const { return strips_.empty(); }

private:
    std::vector<Strip>       strips_;
    StripMeshBuilder         stripBuilder_;
    std::vector<BorderPoint> polyline_;   /* scratch, reused across loops */
};

}  // namespace xcom
