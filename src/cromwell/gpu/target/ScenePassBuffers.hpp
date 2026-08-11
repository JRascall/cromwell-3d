/* ScenePassBuffers.hpp — everything a scene pass needs BESIDES its colour
 * target, sized together.
 *
 * SINGLE RESPONSIBILITY: own the three screen-space buffers a full-quality
 * render needs — the depth prepass, the ambient occlusion it feeds, and the
 * decal DBuffer it is unprojected into — and keep them the same size as each
 * other. It runs no passes; the renderer does that, into these.
 *
 * ==================== WHY THIS IS A THING AND NOT THREE ====================
 *
 * BECAUSE SCREEN-SPACE EFFECTS BELONG TO A CAMERA, NOT TO A FRAME. Every one of
 * these is reconstructed from a depth buffer rendered from a specific viewpoint
 * at a specific resolution:
 *
 *   the PREPASS holds depth and world normals for one camera;
 *   SSAO orients its sampling hemisphere from those normals and reads that
 *     depth, so it is only meaningful for the camera that wrote them;
 *   the DBUFFER is written by unprojecting that same depth to find the surface
 *     under each pixel of a projector box.
 *
 * Sample any of them from a different camera and you do not get a degraded
 * result, you get a wrong one — occlusion smeared in the shape of a view
 * nobody is looking through, decals projected onto whatever the OTHER camera
 * happened to have at that pixel. Which is why a second camera that wants these
 * effects must have its own set, and why the set travels as one object: three
 * buffers at three different sizes is the same bug with more steps.
 *
 * ================= WHAT IT COSTS, SO THE CHOICE IS INFORMED ================
 *
 * A FULL-QUALITY SECOND CAMERA IS NOT FREE and the price is paid twice over:
 *
 *   MEMORY, for a depth+normal target, two occlusion targets and three DBuffer
 *   attachments at the capture's resolution;
 *   TIME, for a second geometry pass into the prepass and a second SSAO blur,
 *   on top of the lit pass the capture was already paying for.
 *
 * So it is opt-in per capture, which is exactly the distinction between a
 * minimap and a security camera: a map drawn from directly above has no use for
 * contact shadows in creases nobody can see at that scale, and a CCTV feed
 * looking down a corridor at head height wants everything the main view has. One
 * of those should allocate these and the other should not.
 *
 * See camera/Camera.hpp, which owns one of these when asked to.
 *
 * NOT THE COLOUR TARGET. That is the capture's own HdrTarget, because it is the
 * one buffer whose size the CALLER cares about — it is what comes out. These
 * are internal to producing it.
 */
#pragma once

#include "cromwell/decal/DecalBuffer.hpp"
#include "cromwell/gpu/target/DepthTarget.hpp"
#include "cromwell/post/AmbientOcclusion.hpp"

namespace cromwell {

class ScenePassBuffers {
public:
    ScenePassBuffers() = default;

    ScenePassBuffers(const ScenePassBuffers&) = delete;
    ScenePassBuffers& operator=(const ScenePassBuffers&) = delete;

    /* Allocates all three at `width` x `height` and loads the occlusion pass's
     * shaders.
     *
     * PARTIAL SUCCESS IS A REAL OUTCOME and is reported per part rather than as
     * one bool. A machine whose driver refuses the occlusion format should still
     * get a capture with decals, and one with no decal shader should still get
     * occlusion — the alternative is an all-or-nothing switch that turns a
     * missing extension into a missing feature. Returns false only when the
     * prepass itself could not be made, which is the one part nothing else can
     * work without. */
    bool create(int width, int height);

    /* No-op at the same size. Safe to call every frame from a layout that
     * rarely changes. */
    bool resize(int width, int height);

    /* True when the depth prepass exists — the part everything else is built
     * on. `occlusionAvailable` and `decalsAvailable` narrow it. */
    bool valid() const { return depth_.valid(); }
    bool occlusionAvailable() const { return occlusion_.available(); }
    bool decalsAvailable() const { return decals_.valid(); }

    int width() const { return depth_.width(); }
    int height() const { return depth_.height(); }

    DepthTarget&      depth() { return depth_; }
    const DepthTarget& depth() const { return depth_; }

    AmbientOcclusion&       occlusion() { return occlusion_; }
    const AmbientOcclusion& occlusion() const { return occlusion_; }

    DecalBuffer&       decals() { return decals_; }
    const DecalBuffer& decals() const { return decals_; }

private:
    DepthTarget      depth_;
    AmbientOcclusion occlusion_;
    DecalBuffer      decals_;

    bool loaded_ = false;
};

}  // namespace cromwell
