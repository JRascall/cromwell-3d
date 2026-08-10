/* Quat.hpp — a rotation in three dimensions.
 *
 * SINGLE RESPONSIBILITY: represent an orientation and compose, invert and apply
 * rotations.
 *
 * WHY QUATERNIONS RATHER THAN EULER ANGLES, since the question always comes
 * back. Euler angles gimbal-lock — pitch to ninety degrees and yaw and roll
 * become the same axis, so a camera looking straight down loses a degree of
 * freedom and snaps. They also do not interpolate: blending two Euler triples
 * takes a path through orientations nobody chose, which is what makes a turret
 * swing the long way round. A quaternion has neither problem, at the cost of
 * being unreadable by eye — so authoring still happens in degrees (fromEuler)
 * and everything after it is a quaternion.
 *
 * UNIT LENGTH IS THE INVARIANT, and it is not enforced. Repeated multiplication
 * drifts, so anything composing rotations over time should normalise
 * periodically. It is left to the caller rather than done on every operation
 * because the cost is a square root per rotation and the drift is a few
 * multiplications' worth per frame.
 *
 * NO raylib, same as Vec3: this lives in cromwell's headless half. Layout
 * matches raylib's Quaternion (x, y, z, w in that order) so the render boundary
 * converts for free — see math/RaylibInterop.hpp.
 *
 * PUBLIC MEMBERS, the same deliberate exception Vec3 documents: a rotation is a
 * value type whose representation is its interface.
 */
#pragma once

#include "cromwell/math/Vec3.hpp"

#include <cmath>

namespace cromwell {

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;   /* identity, not zero: a zero quaternion rotates nothing
                       * onto nothing and is never a valid orientation */

    static constexpr Quat identity() { return { 0.0f, 0.0f, 0.0f, 1.0f }; }

    /* `axis` need not be normalised; it is normalised here, because an
     * un-normalised axis silently scales the rotation. */
    static Quat fromAxisAngle(Vec3 axis, float radians)
    {
        const Vec3  unit = axis.normalised();
        const float half = radians * 0.5f;
        const float s = std::sin(half);
        return { unit.x * s, unit.y * s, unit.z * s, std::cos(half) };
    }

    /* Yaw (around Y), pitch (around X), roll (around Z), in radians, applied
     * in that order. Y-first because this engine is Y-up and yaw is the one a
     * camera changes most. */
    static Quat fromEuler(float yaw, float pitch, float roll)
    {
        const float cy = std::cos(yaw * 0.5f),   sy = std::sin(yaw * 0.5f);
        const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
        const float cr = std::cos(roll * 0.5f),  sr = std::sin(roll * 0.5f);

        return { cy * sp * cr + sy * cp * sr,
                 sy * cp * cr - cy * sp * sr,
                 cy * cp * sr - sy * sp * cr,
                 cy * cp * cr + sy * sp * sr };
    }

    float lengthSquared() const { return x * x + y * y + z * z + w * w; }
    float length() const { return std::sqrt(lengthSquared()); }

    Quat normalised() const
    {
        const float len = length();
        if (len <= 1e-8f) return identity();
        return { x / len, y / len, z / len, w / len };
    }

    /* The inverse, for a UNIT quaternion. Cheaper than a true inverse and
     * correct as long as the invariant holds — which is the other reason to
     * normalise after composing. */
    constexpr Quat conjugate() const { return { -x, -y, -z, w }; }
};

/* Composition. NOT commutative: `a * b` applies b first, then a — the same
 * order matrices compose in, so the two read alike. */
inline constexpr Quat operator*(Quat a, Quat b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

inline Quat& operator*=(Quat& a, Quat b) { a = a * b; return a; }

inline constexpr bool operator==(Quat a, Quat b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
inline constexpr bool operator!=(Quat a, Quat b) { return !(a == b); }

/* Rotates a vector. Uses the standard two-cross-product form rather than
 * building a matrix — fewer operations, and no matrix to keep in sync. */
inline Vec3 rotate(Quat q, Vec3 v)
{
    const Vec3 axis{ q.x, q.y, q.z };
    const Vec3 t = cross(axis, v) * 2.0f;
    return v + t * q.w + cross(axis, t);
}

/* Spherical interpolation: a constant-rate path along the shortest arc.
 *
 * The sign flip is the part that is easy to leave out and looks like a bug when
 * it is missing — q and -q are the SAME rotation, so without it a blend can
 * take the 350-degree route instead of the 10-degree one. */
inline Quat slerp(Quat a, Quat b, float t)
{
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

    if (cosine < 0.0f) {
        b = { -b.x, -b.y, -b.z, -b.w };
        cosine = -cosine;
    }

    /* Nearly parallel: sin(theta) goes to zero and the division blows up, so
     * fall back to a straight blend, which is accurate to well within a pixel
     * at these angles. */
    if (cosine > 0.9995f) {
        const Quat blended{ a.x + (b.x - a.x) * t,
                            a.y + (b.y - a.y) * t,
                            a.z + (b.z - a.z) * t,
                            a.w + (b.w - a.w) * t };
        return blended.normalised();
    }

    const float theta = std::acos(cosine);
    const float sinTheta = std::sin(theta);
    const float weightA = std::sin((1.0f - t) * theta) / sinTheta;
    const float weightB = std::sin(t * theta) / sinTheta;

    return { a.x * weightA + b.x * weightB,
             a.y * weightA + b.y * weightB,
             a.z * weightA + b.z * weightB,
             a.w * weightA + b.w * weightB };
}

inline bool nearlyEqual(Quat a, Quat b, float tolerance = 1e-5f)
{
    /* Compares ORIENTATIONS, so q and -q count as equal — see slerp. */
    const float cosine = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return cosine >= 1.0f - tolerance;
}

}  // namespace cromwell
