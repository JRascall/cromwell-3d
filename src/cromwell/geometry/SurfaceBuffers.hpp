/* SurfaceBuffers.hpp — one vertex buffer per material class and facing.
 *
 * SINGLE RESPONSIBILITY: give the geometry emitter somewhere to put each box
 * according to what it is made of and which way it faces.
 *
 * This is where per-material batching actually happens. The emitter walks the
 * tile data once and drops every box into its bucket; StaticsMesh then uploads
 * one mesh per non-empty buffer. The alternative — sorting afterwards, or a
 * draw call per box — is either a second pass over the geometry or a thousand
 * state changes, and this costs neither.
 *
 * TWO AXES NOW, AND THE SECOND ONE IS NEARLY FREE. Kind is the batching unit
 * because a draw call binds one texture set; facing is the CUTAWAY unit,
 * because removing the walls between the camera and a building's interior
 * means removing whole directions at once (see SurfaceFacing.hpp). Splitting
 * here rather than at draw time is what keeps the cutaway a matter of skipping
 * buffers instead of a per-fragment test.
 *
 * THE GRID IS MOSTLY EMPTY, ON PURPOSE. Only walls and window glass ever carry
 * a real facing; floors, ramps, mass and cover all land in `None`, so the vast
 * majority of these buffers never take a vertex and never allocate. Keeping
 * the array rectangular rather than special-casing the two kinds that need it
 * costs a few hundred bytes of empty vectors and saves the emitter and the
 * mesh builder from both having to know which kinds are splittable.
 *
 * WHAT IT COSTS AT DRAW TIME, stated because the kind split's whole argument
 * was draw-call count: a storey's walls can now become up to five meshes
 * rather than one. On this lattice that is four extra calls per storey in the
 * worst case, against the couple of dozen the world already submits.
 */
#pragma once

#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/style/SurfaceFacing.hpp"
#include "cromwell/style/SurfaceKind.hpp"

#include <array>

namespace cromwell {

class SurfaceBuffers {
public:
    /* The facing-aware accessor. */
    MeshVertexBuffer& operator()(SurfaceKind kind, SurfaceFacing facing)
    {
        return buffers_[slot(kind, facing)];
    }
    const MeshVertexBuffer& operator()(SurfaceKind kind, SurfaceFacing facing) const
    {
        return buffers_[slot(kind, facing)];
    }

    /* Unfaced geometry — the common case, and the reason most of the emitter
     * did not have to change. A floor slab has no outward direction that means
     * anything, so it goes where nothing will ever cut it. */
    MeshVertexBuffer& operator[](SurfaceKind kind)
    {
        return (*this)(kind, SurfaceFacing::None);
    }
    const MeshVertexBuffer& operator[](SurfaceKind kind) const
    {
        return (*this)(kind, SurfaceFacing::None);
    }

    static constexpr int bucketCount = kSurfaceKindCount * kSurfaceFacingCount;

    void clear()
    {
        for (MeshVertexBuffer& buffer : buffers_) buffer.clear();
    }

private:
    static constexpr std::size_t slot(SurfaceKind kind, SurfaceFacing facing)
    {
        return indexOf(kind) * static_cast<std::size_t>(kSurfaceFacingCount) + indexOf(facing);
    }

    std::array<MeshVertexBuffer, bucketCount> buffers_;
};

}  // namespace cromwell
