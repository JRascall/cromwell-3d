#include "render/geometry/MeshVertexBuffer.hpp"

#include <cstring>

namespace xcom {
namespace {

/* UnloadMesh frees mesh arrays with RL_FREE, so they must come from MemAlloc
 * rather than new[] or std::vector's allocator. */
template <typename T>
T* copyToMeshMemory(const std::vector<T>& source)
{
    const auto bytes = static_cast<unsigned int>(source.size() * sizeof(T));
    T* destination = static_cast<T*>(MemAlloc(bytes));
    std::memcpy(destination, source.data(), bytes);
    return destination;
}

}  // namespace

Mesh MeshVertexBuffer::uploadMesh() const
{
    Mesh mesh = { 0 };
    const int count = vertexCount();
    if (count == 0) return mesh;

    mesh.vertexCount   = count;
    mesh.triangleCount = count / 3;

    mesh.vertices  = copyToMeshMemory(positions_);
    mesh.normals   = copyToMeshMemory(normals_);
    mesh.tangents  = copyToMeshMemory(tangents_);
    mesh.texcoords = copyToMeshMemory(texcoords_);
    mesh.colors    = copyToMeshMemory(colours_);

    UploadMesh(&mesh, false);
    return mesh;
}

}  // namespace xcom
