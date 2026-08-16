/* RhiOverlays.hpp — the interface drawn INTO the world, as renderables.
 *
 * SINGLE RESPONSIBILITY: build a mesh per overlay layer from the game's own
 * state, rebuild it when that state changes, and keep one renderable per layer
 * registered in a RenderScene. It draws nothing.
 *
 * ================= WHY THIS IS NOT A PORT OF OverlayRenderer ==============
 *
 * `OverlayRenderer` draws the same four layers for the raylib renderer with
 * `DrawCube` and `DrawLine3D` straight into immediate mode. rhi/MIGRATION.md
 * §4.4 originally claimed the overlays "go through the UI kit, so §4.1 landing
 * is most of what they needed" — they do not, and that entry was written from
 * the name of the thing rather than from its code. They are WORLD-SPACE
 * geometry, and they were the last thing on the device path with no answer.
 *
 * §4.4 offers two: let them become debug-draw primitives and accept the look
 * changing, or make them renderables the game registers. This is the second.
 * The first would have been a second way for the game to push triangles at the
 * device, invented in the same month the first one was removed.
 *
 * ============ THE ONE SHAPE CHANGE THE RETAINED SCENE DEMANDS =============
 *
 * **The visibility overlay used to emit one draw per standable cell, EVERY
 * FRAME.** As renderables that would be thousands of registrations a frame,
 * which is the wrong use of a retained scene — the whole economy of one is that
 * what does not change is not re-stated.
 *
 * So it becomes ONE MESH PER STOREY, rebuilt when the field CHANGES. That is
 * strictly less work than before: the per-frame loop only ever looked cheap
 * because immediate mode hid it behind a function call per cube. The game now
 * pays a rebuild on selection or a move — a few hundred boxes, a few times a
 * second at most — instead of paying the same walk sixty times a second forever.
 *
 * ================= HOW "CHANGED" IS DECIDED, AND WHY A HASH ===============
 *
 * By hashing the inputs each layer is built from, and rebuilding when the hash
 * moves.
 *
 * THE OBVIOUS ALTERNATIVE IS A KEY MADE OF THE THINGS THAT CHANGE THE FIELD —
 * the selected unit, its position, the iso level — and it is wrong in a way
 * that is invisible until it is not. That list is a claim about every cause,
 * and it goes stale the first time a new one appears: a grenade demolishes a
 * wall, the field genuinely changes, none of the keyed values move, and the
 * overlay keeps showing line of sight through a wall that is no longer there.
 * The symptom reads as a LOS bug in the simulation, which is the most expensive
 * possible place to look for it.
 *
 * A hash over the field is a few thousand bytes once a frame — comfortably
 * under a microsecond, against a rebuild it is protecting that costs orders of
 * magnitude more — and it is correct BY CONSTRUCTION rather than by an argument
 * about causes. CLAUDE.md's rule applies exactly: this is not a per-cell or
 * per-ray loop, and buying correctness at this price is not an optimisation
 * question.
 *
 * ===================== WHOSE OVERLAYS THESE ARE ==========================
 *
 * Every renderable here carries a VIEWER MASK, and that is not decoration for a
 * single-player game. Two players in one shared world each have their own
 * selection, so each has their own visibility field, their own cover shields
 * and their own hover plate — and all of those sit in the ONE scene that world
 * owns. The viewer bit is what makes them visible to exactly one pane. See
 * cromwell/render/Renderable.hpp, where the field exists precisely because this
 * case was missed on the design's first pass.
 *
 * They are also `castsShadow(false)` and `visibleInReflections(false)`. A
 * selection marker that cast a shadow would be one player's interface
 * darkening the floor; one that reached a probe capture would appear, mirrored
 * and stale, in every player's reflections including those who do not own it.
 */
#pragma once

#include "cromwell/render/Renderable.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace cromwell {
class MeshVertexBuffer;
class RenderScene;
namespace rhi { class IRenderDevice; }
}  // namespace cromwell

namespace game {

struct FrameView;

class RhiOverlays {
public:
    explicit RhiOverlays(cromwell::rhi::IRenderDevice& device);
    ~RhiOverlays();

    RhiOverlays(const RhiOverlays&) = delete;
    RhiOverlays& operator=(const RhiOverlays&) = delete;

    /* Rebuilds whatever changed and leaves the scene holding one renderable per
     * layer. Cheap on a frame where nothing moved: four hashes and no uploads.
     *
     * `viewer` is which pane these belong to. kAllViewers is the single-player
     * answer and is what a game with one camera should pass. */
    void sync(cromwell::RenderScene& scene, const FrameView& view,
              cromwell::ViewerMask viewer);

    void release();

    int renderableCount() const;

private:
    /* ONE MESH THAT IS REBUILT. The device has no notion of a mesh whose vertex
     * count changes, so a rebuild destroys and remakes both the buffer and the
     * mesh and hands the new pair to RenderScene::setMesh.
     *
     * THAT IS WHY setMesh EXISTS. §4.12 left it open — "either setMesh exists or
     * an overlay rebuild is remove-and-re-add" — and this is the case that
     * settles it: remove-and-re-add would recycle the scene's slot and
     * invalidate the id held right here, turning every rebuild into
     * bookkeeping. A per-change mesh swap is the overlay's NORMAL path.
     *
     * THE ALTERNATIVE, IF IT EVER MATTERS: a buffer kept at its high-water mark
     * with a vertex COUNT on the draw, the way the debug line pass already
     * works. It would trade two device objects per rebuild for a field on
     * DrawItem, and there is no measurement yet that asks for it. */
    struct Layer {
        cromwell::rhi::MeshHandle   mesh;
        cromwell::rhi::BufferHandle vertices;
        cromwell::RenderableId      id;

        /* WHAT THIS MESH WAS BUILT FROM. Zero means "nothing built", which is
         * distinct from a hash that happens to be zero because an empty layer
         * is given a hash of one. */
        std::uint64_t key = 0;
    };

    /* Rebuilds one layer from a filled vertex buffer, or hides it when the
     * buffer is empty. `key` is what the buffer was built from. */
    void update(cromwell::RenderScene& scene, Layer& layer, std::uint64_t key,
                const cromwell::MeshVertexBuffer& built,
                cromwell::ViewerMask viewer, cromwell::FilterFlags flags);

    void releaseLayer(Layer& layer);

    cromwell::rhi::IRenderDevice& device_;
    cromwell::RenderScene*        scene_ = nullptr;

    /* ---- the four layers -------------------------------------------------
     *
     * VISIBILITY IS ONE LAYER PER STOREY and the other three are one each, and
     * the difference is what the cutaway does to them.
     *
     * A storey's worth of visibility plates carries that storey's filter bit,
     * so raising or lowering the iso level hides and shows them with no rebuild
     * at all — the same "the cutaway is a skip, not a rebuild" property the
     * static world has had since it was split per storey. Keyed on the cut
     * instead, every change of floor would rediscover and re-upload thousands
     * of boxes.
     *
     * The other three are single-cell things at the selection or the cursor,
     * and take their one cell's storey bit. */
    std::vector<Layer> visibility_;
    Layer              cover_;
    Layer              hover_;
    Layer              path_;
};

}  // namespace game
