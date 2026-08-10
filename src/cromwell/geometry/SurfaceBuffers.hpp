/* SurfaceBuffers.hpp — one vertex buffer per material class.
 *
 * SINGLE RESPONSIBILITY: give the geometry emitter somewhere to put each box
 * according to what it is made of.
 *
 * This is where per-material batching actually happens. The emitter walks the
 * tile data once and drops every box into its kind's buffer; StaticsMesh then
 * uploads one mesh per non-empty buffer. The alternative — sorting afterwards,
 * or a draw call per box — is either a second pass over the geometry or a
 * thousand state changes, and this costs neither.
 */
#pragma once

#include "cromwell/geometry/MeshVertexBuffer.hpp"
#include "cromwell/style/SurfaceKind.hpp"

#include <array>

namespace cromwell {

class SurfaceBuffers {
public:
    MeshVertexBuffer& operator[](SurfaceKind kind) { return buffers_[indexOf(kind)]; }
    const MeshVertexBuffer& operator[](SurfaceKind kind) const { return buffers_[indexOf(kind)]; }

    void clear()
    {
        for (MeshVertexBuffer& buffer : buffers_) buffer.clear();
    }

private:
    std::array<MeshVertexBuffer, kSurfaceKindCount> buffers_;
};

}  // namespace cromwell
