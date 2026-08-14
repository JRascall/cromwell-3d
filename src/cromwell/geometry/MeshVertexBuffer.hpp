/* MeshVertexBuffer.hpp — vertex accumulation for a baked lit mesh.
 *
 * SINGLE RESPONSIBILITY: collect vertices and hand over a raylib Mesh.
 *
 * Five channels, all of which raylib's DrawMesh binds to standard attribute
 * locations with no help from us: position, normal, tangent, texcoord and
 * colour. That is exactly the set a tangent-space normal map needs, and
 * exactly what a glTF import would arrive carrying — so the same vertex layout
 * serves the generated placeholder boxes and finished authored meshes.
 */
#pragma once

#include "raylib.h"

#include "cromwell/geometry/SurfaceVertex.hpp"
#include "cromwell/rhi/Descriptors.hpp"

#include <cstdint>

#include <vector>

namespace cromwell {

class MeshVertexBuffer {
public:
    /* the BoxEmitter sink contract */
    void vertex(const SurfaceVertex& v)
    {
        positions_.push_back(v.position.x);
        positions_.push_back(v.position.y);
        positions_.push_back(v.position.z);

        normals_.push_back(v.normal.x);
        normals_.push_back(v.normal.y);
        normals_.push_back(v.normal.z);

        tangents_.push_back(v.tangent.x);
        tangents_.push_back(v.tangent.y);
        tangents_.push_back(v.tangent.z);
        tangents_.push_back(v.tangent.w);

        texcoords_.push_back(v.uv.x);
        texcoords_.push_back(v.uv.y);

        colours_.push_back(v.colour.r);
        colours_.push_back(v.colour.g);
        colours_.push_back(v.colour.b);
        colours_.push_back(v.colour.a);
    }

    void clear()
    {
        positions_.clear();
        normals_.clear();
        tangents_.clear();
        texcoords_.clear();
        colours_.clear();
    }

    bool empty() const { return positions_.empty(); }
    int  vertexCount() const { return static_cast<int>(positions_.size() / 3); }

    /* Uploads and returns a Mesh owning copies of this buffer's data.
     * UnloadMesh frees them with RL_FREE, so they must come from MemAlloc. */
    Mesh uploadMesh() const;

    /* ---- the device path -------------------------------------------------
     *
     * THE SAME VERTICES, INTERLEAVED. raylib wants one array per attribute and
     * rhi::IRenderDevice wants one buffer with a stride, so the two differ in
     * layout rather than in content — this produces the second form from the
     * same source, which is what lets both renderers be fed without building
     * the world twice.
     *
     * INTERLEAVED IS ALSO THE FASTER SHAPE. A vertex is read as a unit by the
     * vertex stage, so its attributes want to share a cache line; five separate
     * arrays means five streams and five cache lines per vertex. raylib's split
     * is a consequence of its API, not a choice worth preserving.
     *
     * NO INDICES, and that is not an omission: the box emitter produces
     * triangle soup, so a mesh made from this is non-indexed. See
     * IRenderDevice::createMesh, which takes an optional index buffer for
     * exactly this reason. */
    std::vector<std::uint8_t> interleave() const;

    /* WHAT interleave() PRODUCES, as the device needs it described. Static
     * because it is a property of SurfaceVertex rather than of any one buffer,
     * and because a pipeline has to be built with it before any geometry
     * exists.
     *
     * The locations here and the `layout(location = N)` in the shaders are one
     * contract read from two places — see assets/shaders/CONVENTIONS.md. */
    static rhi::VertexLayout deviceLayout();

private:
    std::vector<float>         positions_;
    std::vector<float>         normals_;
    std::vector<float>         tangents_;
    std::vector<float>         texcoords_;
    std::vector<unsigned char> colours_;
};

}  // namespace cromwell
