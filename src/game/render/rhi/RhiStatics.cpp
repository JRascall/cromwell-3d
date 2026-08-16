#include "game/render/rhi/RhiStatics.hpp"

#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/material/DeviceMaterials.hpp"
#include "cromwell/render/RenderScene.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"
#include "cromwell/style/SurfaceKind.hpp"
#include "game/render/DrawLayers.hpp"
#include "game/render/scene/RenderFilter.hpp"
#include "game/render/scene/StoreyGeometryEmitter.hpp"
#include "game/world/World.hpp"

#include "cromwell/diag/Logger.hpp"

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
    /* THE RENDERABLES COME OUT BEFORE THE MESHES GO, and the order is the mesh
     * lifetime rule made concrete: a scene holds a REFERENCE to a mesh, so
     * destroying one while a renderable still names it leaves the scene able to
     * ask a dead handle to be drawn. On this backend that is a silently
     * ignored draw; on an explicit one it is a validation error or worse.
     * See RenderScene.hpp. */
    for (Chunk& chunk : chunks_) {
        if (scene_ != nullptr && chunk.id.valid()) scene_->remove(chunk.id);

        /* THE MESH AND ITS BUFFER ARE SEPARATE OBJECTS and both are ours — the
         * device deliberately does not destroy a mesh's buffers with it,
         * because meshes routinely share them. Here they do not, so both go. */
        if (chunk.mesh.valid())     device_.destroy(chunk.mesh);
        if (chunk.vertices.valid()) device_.destroy(chunk.vertices);
    }

    chunks_.clear();
    scene_ = nullptr;
    triangleCount_ = 0;
    renderableCount_ = 0;
}

void RhiStatics::rebuild(const World& world, cromwell::RenderScene& scene)
{
    release();
    scene_ = &scene;

    const Lattice& lattice = world.lattice();

    /* THE BUDGET CHECK THE static_assert CANNOT MAKE. A lattice's extents are
     * runtime values, so the assert in RenderFilter.hpp bounds what this game
     * has SPENT and this catches a map that exceeds it. Without it a fourth
     * storey past the budget would get no storey bit, become unhideable by the
     * cutaway, and read as "the iso level stopped working on tall maps". */
    if (lattice.storeys() > kMaxStoreys) {
        LOGGER.error("rhi: this map has {} storeys and the cutaway can address {} - "
                     "the storeys above will not hide. See RenderFilter.hpp",
                     lattice.storeys(), kMaxStoreys);
    }

    const StoreyGeometryEmitter emitter(world);
    cromwell::SurfaceBuffers buffers;

    const cromwell::rhi::VertexLayout layout = cromwell::MeshVertexBuffer::deviceLayout();

    /* HOW MANY CHUNKS ACROSS, rounded up so the last one overhangs a map that
     * is not a whole number of chunks wide. The emitter clamps. */
    const int chunksX = (lattice.width() + kChunkTiles - 1) / kChunkTiles;
    const int chunksY = (lattice.height() + kChunkTiles - 1) / kChunkTiles;

    for (int storey = 0; storey < lattice.storeys(); storey++)
    for (int cy = 0; cy < chunksY; cy++)
    for (int cx = 0; cx < chunksX; cx++) {
        buffers.clear();
        emitter.emit(storey, cx * kChunkTiles, cy * kChunkTiles,
                     (cx + 1) * kChunkTiles, (cy + 1) * kChunkTiles, buffers);

        for (int i = 0; i < cromwell::SurfaceBuffers::bucketCount; i++) {
            const cromwell::SurfaceKind   kind = kindOfSlot(i);
            const cromwell::SurfaceFacing facing = facingOfSlot(i);

            const cromwell::MeshVertexBuffer& source = buffers(kind, facing);
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
             * reach. Chunking is what makes that affordable: a demolition
             * rebuilds the chunks it touched rather than the whole map. */
            desc.access = cromwell::rhi::BufferAccess::CpuToGpuOnce;

            Chunk chunk;
            chunk.vertices = device_.createBuffer(desc);
            if (!chunk.vertices.valid()) continue;

            device_.updateBuffer(chunk.vertices, packed.data(), packed.size(), 0);

            /* NON-INDEXED: the emitter produces triangle soup. See
             * IRenderDevice::createMesh, whose index arguments are optional for
             * exactly this geometry. */
            chunk.mesh = device_.createMesh(layout, chunk.vertices, vertices);
            if (!chunk.mesh.valid()) {
                device_.destroy(chunk.vertices);
                continue;
            }

            /* ---- and this is the whole conversion -----------------------
             *
             * IDENTITY TRANSFORM, because the emitter writes FINAL WORLD
             * POSITIONS — the box emitter has already placed every vertex. So
             * the local bounds it reports are already world bounds, and the
             * matrix is identity rather than a placement. A mesh that carried
             * its own origin would set both; nothing here does.
             *
             * WHITE TINT, so the emitter's own per-surface vertex colours come
             * through untouched.
             *
             * castsShadow FROM THE SURFACE KIND, which is what the sun's pass
             * used to filter on for itself. A window transmits light rather
             * than blocking it and belongs in the transmission plane; the
             * portal pad is a gameplay marker lying flush on the floor and does
             * not physically intercept light. Both facts are properties of the
             * surface, and now they are recorded on the surface. */
            const cromwell::RenderableDesc desc2 =
                cromwell::RenderableDesc()
                    .withMesh(chunk.mesh, source.bounds())
                    .withMaterial(cromwell::DeviceMaterials::idOf(kind))
                    .withFilterFlags(surfaceFlags(storey, facing, drawLayer::kStatics))
                    .withCastsShadow(cromwell::castsSunShadow(kind));

            chunk.id = scene.add(desc2);
            if (!chunk.id.valid()) {
                device_.destroy(chunk.mesh);
                device_.destroy(chunk.vertices);
                continue;
            }

            triangleCount_ += static_cast<int>(vertices / 3);
            renderableCount_++;
            chunks_.push_back(chunk);
        }
    }
}

}  // namespace game
