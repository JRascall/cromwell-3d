#include "game/render/scene/StaticsMesh.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "cromwell/geometry/SurfaceBuffers.hpp"
#include "cromwell/lighting/PbrShader.hpp"
#include "cromwell/post/PrepassShader.hpp"
#include "game/render/scene/StoreyGeometryEmitter.hpp"

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

StaticsMesh::StaticsMesh() = default;

StaticsMesh::~StaticsMesh() { unloadMeshes(); }

void StaticsMesh::unloadMeshes()
{
    for (std::array<StoreyMesh, kBucketCount>& storey : storeys_)
        for (StoreyMesh& entry : storey)
            if (entry.built) { UnloadMesh(entry.mesh); entry.built = false; }
}

void StaticsMesh::rebuild(const World& world)
{
    unloadMeshes();
    storeys_.assign(static_cast<std::size_t>(world.lattice().storeys()),
                    std::array<StoreyMesh, kBucketCount>{});
    triangleCount_ = 0;
    drawCalls_ = 0;

    const StoreyGeometryEmitter emitter(world);
    SurfaceBuffers buffers;

    for (int storey = 0; storey < world.lattice().storeys(); storey++) {
        buffers.clear();
        emitter.emit(storey, buffers);

        /* One walk over every (kind, facing) bucket. The empty ones — which is
         * most of them, since only walls and glass are ever faced — cost a
         * branch each and upload nothing. */
        for (int i = 0; i < kBucketCount; i++) {
            const MeshVertexBuffer& source = buffers(kindOfSlot(i), facingOfSlot(i));
            if (source.empty()) continue;

            StoreyMesh& target = storeys_[static_cast<std::size_t>(storey)][static_cast<std::size_t>(i)];
            target.mesh  = source.uploadMesh();
            target.built = true;
            triangleCount_ += target.mesh.triangleCount;
            drawCalls_++;
        }
    }
}

void StaticsMesh::draw(const CutawayView& cutaway, const Material& material,
                       bool castersOnly) const
{
    const Matrix identity = MatrixIdentity();
    for (int storey = 0; storey < static_cast<int>(storeys_.size()) && storey <= cutaway.maxStorey; storey++) {
        const std::array<StoreyMesh, kBucketCount>& meshes =
            storeys_[static_cast<std::size_t>(storey)];

        for (int i = 0; i < kBucketCount; i++) {
            const StoreyMesh& entry = meshes[static_cast<std::size_t>(i)];
            if (!entry.built) continue;
            if (!cutaway.shows(facingOfSlot(i))) continue;
            if (castersOnly && !castsSunShadow(kindOfSlot(i))) continue;
            DrawMesh(entry.mesh, material, identity);
        }
    }
}

void StaticsMesh::drawPrepass(const CutawayView& cutaway, const Material& material,
                              const MaterialLibrary& library,
                              const PrepassShader& shader) const
{
    const Matrix identity = MatrixIdentity();
    for (int storey = 0; storey < static_cast<int>(storeys_.size()) && storey <= cutaway.maxStorey; storey++) {
        const std::array<StoreyMesh, kBucketCount>& meshes =
            storeys_[static_cast<std::size_t>(storey)];

        for (int i = 0; i < kBucketCount; i++) {
            const StoreyMesh& entry = meshes[static_cast<std::size_t>(i)];
            if (!entry.built) continue;
            if (!cutaway.shows(facingOfSlot(i))) continue;

            /* factorsOf().x is the roughness SCALAR. Where a material has a
             * packed map its green channel modulates this per texel, and the
             * prepass does not sample it — the G-buffer therefore carries the
             * material's roughness rather than the surface's. For deciding how
             * sharply to trace a reflection that is close enough; the lit pass
             * still uses the real per-texel value for the shading itself. */
            shader.setRoughness(library.factorsOf(kindOfSlot(i)).x);
            DrawMesh(entry.mesh, material, identity);
        }
    }
}

/* Blended, and NOT writing depth: a transparent surface that wrote depth would
 * hide whatever is drawn behind it afterwards — including another pane. Panes
 * are not sorted against each other; two overlapping windows will blend in
 * submission order rather than back to front, which on a tile lattice needs a
 * fairly contrived view to notice. */
void StaticsMesh::drawTransparentLit(const CutawayView& cutaway, const MaterialLibrary& library,
                                     const PbrShader& shader) const
{
    const Matrix identity = MatrixIdentity();

    rlDisableDepthMask();
    for (int storey = 0; storey < static_cast<int>(storeys_.size()) && storey <= cutaway.maxStorey; storey++) {
        const std::array<StoreyMesh, kBucketCount>& meshes =
            storeys_[static_cast<std::size_t>(storey)];

        for (int i = 0; i < kBucketCount; i++) {
            const SurfaceKind kind = kindOfSlot(i);
            if (!isTransparent(kind)) continue;
            if (!cutaway.shows(facingOfSlot(i))) continue;

            const StoreyMesh& entry = meshes[static_cast<std::size_t>(i)];
            if (!entry.built) continue;

            const MaterialLibrary::Handle handle = library.handleOf(kind);
            shader.setMaterialFactors(library.factorsOf(kind));
            shader.setMaterialOptions(library.optionsOf(kind));
            shader.setMaterialTransmission(library.transmissionOf(handle));
            shader.setGlass(library.glassParamsOf(handle),
                            library.glassEdgeOf(handle),
                            library.glassRemapOf(handle),
                            library.glassGrimeOf(handle));
            DrawMesh(entry.mesh, library.material(kind), identity);
        }
    }
    rlEnableDepthMask();
}

void StaticsMesh::drawKind(const CutawayView& cutaway, SurfaceKind kind,
                           const Material& material) const
{
    const Matrix identity = MatrixIdentity();
    for (int storey = 0; storey < static_cast<int>(storeys_.size()) && storey <= cutaway.maxStorey; storey++) {
        for (int f = 0; f < kSurfaceFacingCount; f++) {
            const SurfaceFacing facing = static_cast<SurfaceFacing>(f);
            if (!cutaway.shows(facing)) continue;

            const StoreyMesh& entry =
                storeys_[static_cast<std::size_t>(storey)][slot(kind, facing)];
            if (entry.built) DrawMesh(entry.mesh, material, identity);
        }
    }
}

void StaticsMesh::drawLit(const CutawayView& cutaway, const MaterialLibrary& library,
                          const PbrShader& shader, bool includeTransparent) const
{
    const Matrix identity = MatrixIdentity();
    for (int storey = 0; storey < static_cast<int>(storeys_.size()) && storey <= cutaway.maxStorey; storey++) {
        const std::array<StoreyMesh, kBucketCount>& meshes =
            storeys_[static_cast<std::size_t>(storey)];

        for (int i = 0; i < kBucketCount; i++) {
            const StoreyMesh& entry = meshes[static_cast<std::size_t>(i)];
            if (!entry.built) continue;
            if (!cutaway.shows(facingOfSlot(i))) continue;

            const SurfaceKind kind = kindOfSlot(i);
            if (isTransparent(kind) && !includeTransparent)
                continue;   /* drawn later, see the header */

            /* The textures ride in the material and are bound by DrawMesh; the
             * scalars are two uniforms pushed alongside it. */
            shader.setMaterialFactors(library.factorsOf(kind));
            shader.setMaterialOptions(library.optionsOf(kind));
            shader.setMaterialTransmission(library.transmissionOf(library.handleOf(kind)));
            DrawMesh(entry.mesh, library.material(kind), identity);
        }
    }
}

}  // namespace game
