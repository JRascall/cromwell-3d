#include "cromwell/decal/Decal.hpp"

#include "raymath.h"

#include <cmath>

namespace cromwell {

Decal Decal::onSurface(Vector3 point, Vector3 normal, float rotation,
                       Vector2 size, float depth)
{
    Decal decal;

    const Vector3 axis = Vector3Normalize(normal);

    /* ANY reference that is not parallel to the axis will do — the rotation
     * argument is what actually orients the decal, and this only has to give
     * that rotation something stable to start from. World up is the natural
     * choice on a tactical board (a decal on the ground then has its V running
     * north at rotation 0), and the fallback covers the walls, where up IS the
     * axis and the cross product would collapse to zero. */
    const Vector3 reference = (std::fabs(axis.y) > 0.99f) ? Vector3{ 0.0f, 0.0f, 1.0f }
                                                          : Vector3{ 0.0f, 1.0f, 0.0f };

    Vector3 tangent   = Vector3Normalize(Vector3CrossProduct(reference, axis));
    Vector3 bitangent = Vector3CrossProduct(axis, tangent);

    const float c = std::cos(rotation);
    const float s = std::sin(rotation);
    const Vector3 u = Vector3Add(Vector3Scale(tangent, c), Vector3Scale(bitangent, s));
    const Vector3 v = Vector3Subtract(Vector3Scale(bitangent, c), Vector3Scale(tangent, s));

    /* Columns, in raylib's layout: m0/m1/m2 is the image of local +X, m4/m5/m6
     * of local +Y, m8/m9/m10 of local +Z, and m12/m13/m14 the translation. The
     * shader reads these back as uModel[0..2].xyz to rebuild the decal's frame
     * without a second uniform. */
    Matrix& t = decal.transform;
    t.m0 = u.x * size.x;    t.m4 = v.x * size.y;    t.m8  = axis.x * depth;  t.m12 = point.x;
    t.m1 = u.y * size.x;    t.m5 = v.y * size.y;    t.m9  = axis.y * depth;  t.m13 = point.y;
    t.m2 = u.z * size.x;    t.m6 = v.z * size.y;    t.m10 = axis.z * depth;  t.m14 = point.z;
    t.m3 = 0.0f;            t.m7 = 0.0f;            t.m11 = 0.0f;            t.m15 = 1.0f;

    return decal;
}

}  // namespace cromwell
