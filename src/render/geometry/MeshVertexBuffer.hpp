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

#include "render/geometry/SurfaceVertex.hpp"

#include <vector>

namespace xcom {

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

private:
    std::vector<float>         positions_;
    std::vector<float>         normals_;
    std::vector<float>         tangents_;
    std::vector<float>         texcoords_;
    std::vector<unsigned char> colours_;
};

}  // namespace xcom
