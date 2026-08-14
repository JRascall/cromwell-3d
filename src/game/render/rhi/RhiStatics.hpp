/* RhiStatics.hpp — the static world as device geometry.
 *
 * SINGLE RESPONSIBILITY: build one device mesh per (storey, kind, facing) from
 * the world, and submit the ones a given cutaway allows.
 *
 * ================ WHY THIS EXISTS BESIDE StaticsMesh ======================
 *
 * StaticsMesh does the same job for the raylib renderer and is untouched. Two
 * builders rather than one that feeds both, deliberately: the alternative was
 * StaticsMesh holding raylib meshes AND device meshes together, which doubles
 * the static world's GPU memory for as long as the migration lasts — on the
 * SHIPPING path, to serve a development one.
 *
 * The duplication is small and temporary. Both read the same
 * StoreyGeometryEmitter and the same MeshVertexBuffer, so the geometry cannot
 * drift between them; only the upload differs, and that is exactly the
 * difference being migrated. This file replaces StaticsMesh at parity.
 *
 * ================== ONE MESH PER BUCKET, NOT ONE PER WORLD ================
 *
 * The same split StaticsMesh already justifies: a mesh per (storey, surface
 * kind, facing) is what makes the cutaway a matter of SKIPPING draws rather
 * than rebuilding geometry when the player changes floor or rotates the
 * camera. Most buckets are empty — only walls and glass are ever faced — and an
 * empty one costs a branch and uploads nothing.
 */
#pragma once

#include "cromwell/geometry/SurfaceBuffers.hpp"
#include "cromwell/rhi/Handles.hpp"
#include "game/render/scene/CutawayView.hpp"

#include <array>
#include <vector>

namespace cromwell {
class DeviceMaterials;
class IRenderDevice;
namespace rhi { class ICommandEncoder; class IRenderDevice; }
}  // namespace cromwell

namespace game {

class World;

class RhiStatics {
public:
    explicit RhiStatics(cromwell::rhi::IRenderDevice& device);
    ~RhiStatics();

    RhiStatics(const RhiStatics&) = delete;
    RhiStatics& operator=(const RhiStatics&) = delete;

    /* Safe to call repeatedly — the same contract StaticsMesh has: edit the
     * data, call rebuild, and the world reflects it. Releases what it held. */
    void rebuild(const World& world);

    /* Submits every bucket the cutaway allows. A pipeline must already be
     * bound; this class has no opinion about shading, which is what lets the
     * shadow pass and the depth prepass share it.
     *
     * `castersOnly` drops the surfaces that transmit light rather than block
     * it — the sun's pass wants that, the prepass does not, because a window
     * is still solid geometry to a depth test. */
    /* `materials` (may be null) binds each bucket's material before its draw.
     * Null for a pass that has no material block bound — the shadow map reads
     * position and nothing else, so binding one there would be work for a
     * shader that cannot see it. */
    void submit(cromwell::rhi::ICommandEncoder& encoder, const CutawayView& cutaway,
                bool castersOnly,
                const cromwell::DeviceMaterials* materials = nullptr,
                bool translucent = false) const;

    int triangleCount() const { return triangleCount_; }
    int drawCalls() const { return drawCalls_; }

private:
    void release();

    struct Bucket {
        cromwell::rhi::MeshHandle   mesh;
        cromwell::rhi::BufferHandle vertices;
        int                         triangles = 0;
    };

    static constexpr int kBucketCount = cromwell::SurfaceBuffers::bucketCount;

    cromwell::rhi::IRenderDevice& device_;
    std::vector<std::array<Bucket, kBucketCount>> storeys_;

    int triangleCount_ = 0;
    int drawCalls_ = 0;
};

}  // namespace game
