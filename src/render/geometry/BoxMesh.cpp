#include "render/geometry/BoxMesh.hpp"

#include "raymath.h"

#include "render/geometry/BoxEmitter.hpp"
#include "render/geometry/MeshVertexBuffer.hpp"

namespace xcom {

BoxMesh::BoxMesh()
{
    MeshVertexBuffer buffer;
    emitBox(buffer, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, WHITE);

    mesh_  = buffer.uploadMesh();
    built_ = (mesh_.vertexCount > 0);
}

BoxMesh::~BoxMesh()
{
    if (built_) UnloadMesh(mesh_);
}

void BoxMesh::draw(const Material& material,
                   float centreX, float centreY, float centreZ,
                   float sizeX, float sizeY, float sizeZ) const
{
    if (!built_) return;

    /* Scale then translate. raylib applies its matrices to row vectors, so the
     * first operation is the left-hand argument. */
    const Matrix transform = MatrixMultiply(MatrixScale(sizeX, sizeY, sizeZ),
                                            MatrixTranslate(centreX, centreY, centreZ));
    DrawMesh(mesh_, material, transform);
}

}  // namespace xcom
