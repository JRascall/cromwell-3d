/* BoxEmitter.hpp — an axis-aligned box as 12 textured, lit triangles.
 *
 * SINGLE RESPONSIBILITY: emit box geometry with the right winding, face
 * normal, tangent frame and texture coordinate.
 *
 * Wound counter-clockwise seen from outside, so backface culling keeps
 * working. Templated on the sink rather than virtual, so the baked path
 * (writing into vertex arrays) and any immediate path share one definition at
 * no call cost.
 *
 * UVs ARE A WORLD-SPACE PLANAR PROJECTION, not a per-box unwrap. Each face
 * takes its two in-plane world axes directly as (u, v), which means a texture
 * runs continuously across abutting tiles instead of restarting at every box —
 * no visible grid, no seams where two floor slabs meet, and no unwrap step for
 * geometry that is generated from tile data and rebuilt whenever a grenade
 * goes off. Density is a material property (PbrMaterial::uvScale) applied in
 * the shader, so retiling a surface costs no rebake.
 *
 * Authored meshes will arrive with their own UVs and tangents and simply
 * bypass this; the vertex layout is the same either way.
 *
 * A Sink must provide: void vertex(const SurfaceVertex&).
 */
#pragma once

#include "render/geometry/SurfaceVertex.hpp"

namespace xcom {
namespace detail {

/* Which two world axes a face projects its texture from, and the tangent frame
 * that goes with them. Handedness is chosen per face so that
 * cross(normal, tangent) * handedness points along +v — the packing every
 * normal-map baker and glTF exporter uses. */
struct FaceBasis {
    Vector3 normal;
    Vector3 tangent;
    float   handedness;
    int     uAxis;      /* 0 = x, 1 = y, 2 = z */
    int     vAxis;
};

inline constexpr FaceBasis kFaces[6] = {
    { {  0.0f,  1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, -1.0f, 0, 2 },   /* +Y top    */
    { {  0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f },  1.0f, 0, 2 },   /* -Y bottom */
    { {  1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, -1.0f, 2, 1 },   /* +X        */
    { { -1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f },  1.0f, 2, 1 },   /* -X        */
    { {  0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f, 0.0f },  1.0f, 0, 1 },   /* +Z        */
    { {  0.0f,  0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, -1.0f, 0, 1 },   /* -Z        */
};

inline float axis(const Vector3& v, int index)
{
    return index == 0 ? v.x : (index == 1 ? v.y : v.z);
}

}  // namespace detail

template <typename Sink>
void emitBox(Sink& sink,
             float centreX, float centreY, float centreZ,
             float sizeX, float sizeY, float sizeZ,
             Color colour)
{
    const float x0 = centreX - sizeX * 0.5f, x1 = centreX + sizeX * 0.5f;
    const float y0 = centreY - sizeY * 0.5f, y1 = centreY + sizeY * 0.5f;
    const float z0 = centreZ - sizeZ * 0.5f, z1 = centreZ + sizeZ * 0.5f;

    /* A box face is flat, so all four corners share one normal and one tangent
     * — no smoothing, no averaging. That hard crease is what a tile lattice
     * should look like. */
    const auto quad = [&](const detail::FaceBasis& face,
                          Vector3 a, Vector3 b, Vector3 c, Vector3 d) {
        const auto put = [&](const Vector3& p) {
            SurfaceVertex v;
            v.position = p;
            v.normal   = face.normal;
            v.tangent  = Vector4{ face.tangent.x, face.tangent.y, face.tangent.z,
                                  face.handedness };
            v.uv       = Vector2{ detail::axis(p, face.uAxis), detail::axis(p, face.vAxis) };
            v.colour   = colour;
            sink.vertex(v);
        };
        put(a); put(b); put(c);
        put(a); put(c); put(d);
    };

    quad(detail::kFaces[0], { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, { x0, y1, z0 });
    quad(detail::kFaces[1], { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 });
    quad(detail::kFaces[2], { x1, y0, z1 }, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 });
    quad(detail::kFaces[3], { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 });
    quad(detail::kFaces[4], { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 });
    quad(detail::kFaces[5], { x1, y0, z0 }, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 });
}

}  // namespace xcom
