/* SceneDrawList.hpp — what one view found in one scene, sorted, ready to draw.
 *
 * SINGLE RESPONSIBILITY: hold the two buckets a collected view produces and the
 * order they are drawn in.
 *
 * ================== INTERNAL. NOT PART OF THE PUBLIC SURFACE ==============
 *
 * The four published headers are Renderable, View, RenderScene and IScenePass.
 * This is not one of them and must not become one: it is the shape culling
 * happens to leave behind, and every future improvement — instancing, batching,
 * an indirect draw buffer, a GPU-side list — changes it. Publishing it would
 * freeze the one part of this design that is expected to move.
 *
 * ======================= WHY THE CALLER OWNS THE STORAGE ==================
 *
 * `RenderScene::collect` fills a list the caller supplies rather than returning
 * one, which is the same contract SpatialHash::queryRadius already has and the
 * same reason: this runs several times per frame per view — the sun, the camera,
 * a probe face — and a vector returned by value is an allocation and a free per
 * call, in a loop that is otherwise arithmetic. Kept across frames the vectors
 * settle at their high-water mark and allocate nothing.
 *
 * ============================= WHY TWO BUCKETS ============================
 *
 * Because they sort by different rules and there is no key that serves both.
 * Opaque wants to minimise state changes and to let the depth test throw work
 * away; translucent MUST be drawn back to front or the blend is wrong. Merging
 * them and sorting once would mean picking which of those two to break.
 */
#pragma once

#include "cromwell/material/MaterialId.hpp"
#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Vec4.hpp"
#include "cromwell/rhi/Handles.hpp"

#include <cstdint>
#include <vector>

namespace cromwell {

/* ONE DRAW, FLATTENED. Everything the pass needs is here, so issuing the list
 * touches no renderable record and asks the scene nothing — which is what makes
 * it legal for the pipeline to hold a list across the passes of one frame while
 * the game is free to keep editing the scene. */
struct DrawItem {
    Mat4 transform;
    Vec4 tint = Vec4::one();

    rhi::MeshHandle mesh;
    MaterialId      material;

    /* WHAT THIS OBJECT IS, for the custom depth pass — and unread by every
     * other pass, which is why it rides here rather than being looked up on the
     * scene at draw time. Collection already walked the record; asking again
     * per draw would be a second walk to recover something it had in hand. */
    std::uint8_t customStencil = 0;

    /* THE OPAQUE ORDER, PACKED SO ONE COMPARISON DECIDES IT. High bits first:
     * size class, then material, then mesh. See RenderScene.cpp for what goes
     * in each field and why size leads.
     *
     * FIELDS THAT COLLIDE HERE ARE HARMLESS, which is the difference between
     * this and the framebuffer cache key that packed a layer into three bits
     * (rhi/MIGRATION.md §5). That key had to be INJECTIVE — two resources
     * hashing alike meant a pass rendering into the wrong target. This one only
     * has to be a stable ordering: two items sharing a key draw adjacently in an
     * arbitrary but consistent order, which is what a sort key is for. */
    std::uint64_t sortKey = 0;

    /* THE TRANSLUCENT ORDER: how far this renderable's centre is from the eye,
     * squared. Per view, computed during collection, never cached on the
     * renderable — see RenderScene.hpp on why Source needs a cache here and we
     * do not. */
    float viewDistanceSquared = 0.0f;
};

class SceneDrawList {
public:
    void clear()
    {
        /* CAPACITY KEPT, CONTENTS DROPPED — see the header. */
        opaque_.clear();
        translucent_.clear();
    }

    std::vector<DrawItem>&       opaque() { return opaque_; }
    std::vector<DrawItem>&       translucent() { return translucent_; }
    const std::vector<DrawItem>& opaque() const { return opaque_; }
    const std::vector<DrawItem>& translucent() const { return translucent_; }

    bool empty() const { return opaque_.empty() && translucent_.empty(); }

    /* HOW MANY RENDERABLES THE VIEW REJECTED, for the dev panel and for the one
     * question a culler always has to answer: is it culling too much? A frame
     * where this is zero on a large world is a frustum that is not working, and
     * a frame where it equals the scene's size is one that culled the player's
     * own feet. Neither is visible from the picture alone. */
    int culled = 0;

private:
    std::vector<DrawItem> opaque_;
    std::vector<DrawItem> translucent_;
};

}  // namespace cromwell
