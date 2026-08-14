#include "game/render/rhi/RhiStatics.hpp"

#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/render/IGeometrySource.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "cromwell/style/SurfaceKind.hpp"
#include "game/render/scene/StoreyGeometryEmitter.hpp"
#include "game/world/World.hpp"

namespace game {
namespace {

/* THE SAME SLOT ARITHMETIC SurfaceBuffers PACKS WITH, unpacked. Duplicated from
 * StaticsMesh rather than shared for the duration of the migration — this file
 * replaces that one, and threading a shared helper through both would be work
 * thrown away at parity. */
constexpr cromwell::SurfaceKind kindOfSlot(int slot)
{
    return static_cast<cromwell::SurfaceKind>(slot / cromwell::kSurfaceFacingCount);
}

constexpr cromwell::SurfaceFacing facingOfSlot(int slot)
{
    return static_cast<cromwell::SurfaceFacing>(slot % cromwell::kSurfaceFacingCount);
}

}  // namespace

RhiStatics::RhiStatics(cromwell::rhi::IRenderDevice& device) : device_(device) {}

RhiStatics::~RhiStatics() { release(); }

void RhiStatics::release()
{
    /* THE MESH AND ITS BUFFER ARE SEPARATE OBJECTS and both are ours — the
     * device deliberately does not destroy a mesh's buffers with it, because
     * meshes routinely share them. Here they do not, so both go. */
    for (auto& storey : storeys_) {
        for (Bucket& bucket : storey) {
            if (bucket.mesh.valid())     device_.destroy(bucket.mesh);
            if (bucket.vertices.valid()) device_.destroy(bucket.vertices);
            bucket = Bucket{};
        }
    }
    storeys_.clear();
    triangleCount_ = 0;
    drawCalls_ = 0;
}

void RhiStatics::rebuild(const World& world)
{
    release();
    storeys_.assign(static_cast<std::size_t>(world.lattice().storeys()),
                    std::array<Bucket, kBucketCount>{});

    const StoreyGeometryEmitter emitter(world);
    cromwell::SurfaceBuffers buffers;

    const cromwell::rhi::VertexLayout layout = cromwell::MeshVertexBuffer::deviceLayout();

    for (int storey = 0; storey < world.lattice().storeys(); storey++) {
        buffers.clear();
        emitter.emit(storey, buffers);

        for (int i = 0; i < kBucketCount; i++) {
            const cromwell::MeshVertexBuffer& source = buffers(kindOfSlot(i), facingOfSlot(i));
            if (source.empty()) continue;

            const std::vector<std::uint8_t> packed = source.interleave();
            const uint32_t vertices = static_cast<uint32_t>(source.vertexCount());

            cromwell::rhi::BufferDesc desc;
            desc.name   = "statics";
            desc.bytes  = packed.size();
            desc.usage  = cromwell::rhi::BufferUsageVertex;

            /* WRITTEN ONCE AND READ EVERY FRAME. A rebuild destroys and remakes
             * rather than updating, so the buffer never changes after upload —
             * which is what lets the backend put it in memory the CPU cannot
             * reach. Destruction rebuilds a storey; that is rare enough that
             * the reallocation is cheaper than keeping every buffer host
             * visible for the frames it does not happen. */
            desc.access = cromwell::rhi::BufferAccess::CpuToGpuOnce;

            Bucket& bucket = storeys_[static_cast<std::size_t>(storey)]
                                     [static_cast<std::size_t>(i)];

            bucket.vertices = device_.createBuffer(desc);
            if (!bucket.vertices.valid()) continue;

            device_.updateBuffer(bucket.vertices, packed.data(), packed.size(), 0);

            /* NON-INDEXED: the emitter produces triangle soup. See
             * IRenderDevice::createMesh, whose index arguments are optional for
             * exactly this geometry. */
            bucket.mesh = device_.createMesh(layout, bucket.vertices, vertices);
            if (!bucket.mesh.valid()) continue;

            bucket.triangles = static_cast<int>(vertices / 3);
            triangleCount_ += bucket.triangles;
            drawCalls_++;
        }
    }
}

void RhiStatics::submit(cromwell::rhi::ICommandEncoder& encoder, const CutawayView& cutaway,
                        bool castersOnly,
                        const cromwell::DeviceMaterials* materials, bool translucent) const
{
    /* IDENTITY, ONCE, FOR THE WHOLE WORLD. The lattice is emitted in world
     * space — the box emitter writes final positions — so there is nothing to
     * transform, and the tint is white so the emitter's own per-surface vertex
     * colours come through untouched.
     *
     * PUSHED ANYWAY rather than skipped, and it is not a formality: push
     * constants persist on the bound program, so whatever the last body pushed
     * would still be in place if this did nothing. The order passes submit in
     * would then decide whether the world was drawn at the origin in soldier
     * blue — which is the kind of bug that appears when an unrelated pass is
     * reordered months later. See rhi/object.glsl on why one path and not two
     * pipelines. */
    const cromwell::ObjectPush world = cromwell::ObjectPush::identity();
    encoder.pushConstants(&world, sizeof world);

    /* THE CUTAWAY IS A SKIP, NOT A REBUILD — which is the whole reason the
     * geometry is split per storey and facing. Changing floor or rotating the
     * camera changes which buckets are submitted and touches no buffer. */
    const int storeyCount = static_cast<int>(storeys_.size());
    for (int storey = 0; storey < storeyCount; storey++) {
        if (storey > cutaway.maxStorey) break;

        for (int i = 0; i < kBucketCount; i++) {
            if (!cutaway.shows(facingOfSlot(i))) continue;
            if (castersOnly && !castsSunShadow(kindOfSlot(i))) continue;

            /* WHICH PASS THIS BUCKET BELONGS IN, and the MATERIAL decides — not
             * a hardcoded list of surface kinds. A surface becomes see-through
             * by saying `blend translucent` in its .mat file, so water is a
             * material rather than a feature.
             *
             * DRAWN IN ONE PASS OR THE OTHER, NEVER BOTH. A blended surface
             * reads whatever is already in the colour buffer, so it must follow
             * the opaque scene — and appearing in the opaque pass as well would
             * paint it solid before the transparent pass could blend it.
             *
             * With no materials bound — the sun's depth pass — everything reads
             * as opaque, which is correct: that pass already filtered by
             * castsSunShadow, and a depth-only shader has no material to bind. */
            const bool bucketTranslucent =
                materials != nullptr && materials->isTranslucent(kindOfSlot(i));
            if (bucketTranslucent != translucent) continue;

            const Bucket& bucket = storeys_[static_cast<std::size_t>(storey)]
                                           [static_cast<std::size_t>(i)];
            if (!bucket.mesh.valid()) continue;

            /* THE BUCKET'S MATERIAL, which is the whole reason the geometry is
             * split by surface kind in the first place: a bind and a draw
             * rather than a re-upload, because each kind owns its own block.
             * See DeviceMaterials on why that is a buffer each. */
            if (materials != nullptr) materials->bind(encoder, kindOfSlot(i));

            encoder.draw(bucket.mesh);
        }
    }
}

}  // namespace game
