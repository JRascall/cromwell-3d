#include "cromwell/geometry/MeshVertexBuffer.hpp"

#include <cstring>

namespace cromwell {
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

/* ---- the device path ------------------------------------------------------
 *
 * ONE STRUCT, DESCRIBED ONCE AND PACKED ONCE. The offsets below and the writes
 * in interleave() have to agree, and the only thing keeping them honest is that
 * they are adjacent and share these constants — which is why the constants
 * exist rather than the numbers being written twice. */
namespace {

constexpr uint32_t kPositionOffset = 0;                    /* 3 floats */
constexpr uint32_t kNormalOffset   = kPositionOffset + 12; /* 3 floats */
constexpr uint32_t kTangentOffset  = kNormalOffset + 12;   /* 4 floats */
constexpr uint32_t kUvOffset       = kTangentOffset + 16;  /* 2 floats */
constexpr uint32_t kColourOffset   = kUvOffset + 8;        /* 4 bytes  */
constexpr uint32_t kVertexStride   = kColourOffset + 4;    /* = 52     */

}  // namespace

rhi::VertexLayout MeshVertexBuffer::deviceLayout()
{
    rhi::VertexLayout layout;
    layout.stride = kVertexStride;
    layout.attributeCount = 5;
    layout.attributes[0] = { 0, kPositionOffset, rhi::VertexFormat::Float3 };
    layout.attributes[1] = { 1, kNormalOffset,   rhi::VertexFormat::Float3 };
    layout.attributes[2] = { 2, kTangentOffset,  rhi::VertexFormat::Float4 };
    layout.attributes[3] = { 3, kUvOffset,       rhi::VertexFormat::Float2 };

    /* NORMALISED, so the shader reads 0..1 rather than 0..255. Vertex colour is
     * a tint, and a shader multiplying by 255 instead of 1 is a white-hot
     * surface that looks like a lighting bug. */
    layout.attributes[4] = { 4, kColourOffset,   rhi::VertexFormat::UByte4Normalised };
    return layout;
}

std::vector<std::uint8_t> MeshVertexBuffer::interleave() const
{
    const std::size_t count = static_cast<std::size_t>(vertexCount());

    std::vector<std::uint8_t> out;
    out.resize(count * kVertexStride);

    for (std::size_t i = 0; i < count; i++) {
        std::uint8_t* vertex = out.data() + i * kVertexStride;

        std::memcpy(vertex + kPositionOffset, &positions_[i * 3], 12);
        std::memcpy(vertex + kNormalOffset,   &normals_[i * 3],   12);
        std::memcpy(vertex + kTangentOffset,  &tangents_[i * 4],  16);
        std::memcpy(vertex + kUvOffset,       &texcoords_[i * 2], 8);
        std::memcpy(vertex + kColourOffset,   &colours_[i * 4],   4);
    }
    return out;
}

}  // namespace cromwell
