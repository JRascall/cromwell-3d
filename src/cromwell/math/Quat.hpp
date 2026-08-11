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

/* ============== POINTING THINGS AT OTHER THINGS ============================
 *
 * Everything above builds a rotation from numbers a human typed. Everything
 * below builds one from a DIRECTION the game computed, which is what gameplay
 * actually has in hand — a vector to the target, a surface normal, a velocity.
 * Unity ships all three of these and they are the most-called quaternion
 * functions in it, for exactly that reason.
 * ========================================================================== */

/* The rotation whose FORWARD (+Z) points along `forward` and whose up is as
 * close to `up` as it can be — Unity's Quaternion.LookRotation, Unreal's
 * FRotationMatrix::MakeFromXZ.
 *
 * Aim a turret, face a soldier at what it is shooting, orient a decal, point a
 * camera. +Z is forward because Vec3::forward() is (0,0,1); +Y is up because
 * this engine is Y-up.
 *
 * DEGENERATE INPUT IS HANDLED RATHER THAN UNDEFINED, and it has to be: the two
 * cases are a zero-length forward (the target is exactly where you are — which
 * happens on the frame something reaches its destination) and a forward
 * parallel to up (looking straight down, which is a camera's normal state).
 * Both make the cross product vanish, and the naive version returns a
 * quaternion full of NaNs that then propagates into a transform and makes an
 * entire model disappear with no error anywhere. */
inline Quat lookRotation(Vec3 forward, Vec3 up = Vec3::up())
{
    Vec3 axisZ = forward.normalised();
    if (axisZ.lengthSquared() < 0.5f) return Quat::identity();

    Vec3 axisX = cross(up, axisZ);
    if (axisX.lengthSquared() < 1e-12f) {
        /* Looking along `up`. Any roll about the view axis is equally correct,
         * so pick one deterministically instead of producing NaNs — a camera
         * that snaps to an arbitrary but STABLE roll at the zenith is a known
         * quantity; one that produces NaN is not. */
        axisX = anyPerpendicular(axisZ);
    }
    axisX = axisX.normalised();
    const Vec3 axisY = cross(axisZ, axisX);

    /* Shepperd's method: pick the branch whose divisor is largest, so the
     * division never happens by something near zero. The naive single-branch
     * conversion loses most of its precision when the trace approaches -1. */
    const float trace = axisX.x + axisY.y + axisZ.z;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        return Quat{ (axisY.z - axisZ.y) / s, (axisZ.x - axisX.z) / s,
                     (axisX.y - axisY.x) / s, 0.25f * s }
            .normalised();
    }
    if (axisX.x > axisY.y && axisX.x > axisZ.z) {
        const float s = std::sqrt(1.0f + axisX.x - axisY.y - axisZ.z) * 2.0f;
        return Quat{ 0.25f * s, (axisY.x + axisX.y) / s, (axisZ.x + axisX.z) / s,
                     (axisY.z - axisZ.y) / s }
            .normalised();
    }
    if (axisY.y > axisZ.z) {
        const float s = std::sqrt(1.0f + axisY.y - axisX.x - axisZ.z) * 2.0f;
        return Quat{ (axisY.x + axisX.y) / s, 0.25f * s, (axisZ.y + axisY.z) / s,
                     (axisZ.x - axisX.z) / s }
            .normalised();
    }
    const float s = std::sqrt(1.0f + axisZ.z - axisX.x - axisY.y) * 2.0f;
    return Quat{ (axisZ.x + axisX.z) / s, (axisZ.y + axisY.z) / s, 0.25f * s,
                 (axisX.y - axisY.x) / s }
        .normalised();
}

/* The shortest rotation taking `from` onto `to` — Unity's
 * Quaternion.FromToRotation.
 *
 * Aligning something to a surface: a decal to a wall's normal, a footprint to
 * the ground, a shield to the direction of a hit. Different from lookRotation,
 * which pins a whole orientation; this one only promises that one vector lands
 * on the other and leaves the roll about it alone.
 *
 * NO TRIGONOMETRY. The half-angle identity gives the quaternion directly from
 * the cross product and the dot — `w = 1 + cos` and the axis unnormalised —
 * which is both faster and better conditioned than acos followed by
 * fromAxisAngle. */
inline Quat fromToRotation(Vec3 from, Vec3 to)
{
    const Vec3 a = from.normalised();
    const Vec3 b = to.normalised();
    if (a.lengthSquared() < 0.5f || b.lengthSquared() < 0.5f) return Quat::identity();

    const float cosine = dot(a, b);
    if (cosine >= 1.0f - 1e-6f) return Quat::identity();

    if (cosine <= -1.0f + 1e-6f) {
        /* EXACTLY OPPOSED, and this is the case the identity above cannot
         * express: every axis perpendicular to `a` is an equally valid
         * half-turn, and the cross product is zero so it names none of them.
         * Left unhandled it returns a zero quaternion, which normalises to
         * identity — a "rotation" that leaves the vector pointing exactly the
         * wrong way, silently. */
        return Quat::fromAxisAngle(anyPerpendicular(a), 3.14159265358979323846f);
    }

    const Vec3 axis = cross(a, b);
    return Quat{ axis.x, axis.y, axis.z, 1.0f + cosine }.normalised();
}

/* The angle between two orientations, in radians. */
inline float angleBetween(Quat a, Quat b)
{
    /* Absolute, because q and -q are the same orientation — without it, half of
     * all pairs report the reflex angle. */
    const float cosine = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(cosine > 1.0f ? 1.0f : cosine);
}

/* Toward `to`, by at most `maxRadians` — Unity's Quaternion.RotateTowards.
 *
 * TURN RATE, which is what makes a unit feel like it has mass instead of
 * snapping to face things. slerp cannot express it: slerp takes a FRACTION, so
 * its angular speed depends on how far there is to go and a unit spins fastest
 * when it has furthest to turn, which is exactly backwards. This moves at a
 * fixed rate and arrives exactly. */
inline Quat rotateTowards(Quat from, Quat to, float maxRadians)
{
    const float angle = angleBetween(from, to);
    if (angle <= maxRadians || angle <= 1e-6f) return to;
    return slerp(from, to, maxRadians / angle);
}

/* Yaw, pitch and roll back out, in the same convention and order fromEuler
 * takes them — so `toEuler(fromEuler(y, p, r))` returns what went in.
 *
 * FOR DISPLAY, SERIALISATION AND AUTHORING, not for doing rotation maths with.
 * Everything gimbal lock does to Euler angles it still does here; the reason
 * this exists is that a dev panel slider, a saved file and a command line are
 * all in degrees, and without it there is no way back out of a quaternion. */
inline void toEuler(Quat q, float& yaw, float& pitch, float& roll)
{
    const Quat u = q.normalised();

    /* Straight out of the rotation matrix Y*X*Z builds: sin(pitch) is one
     * element, and yaw and roll are each an atan2 of a pair. */
    const float sinPitch = 2.0f * (u.w * u.x - u.y * u.z);

    if (std::fabs(sinPitch) >= 1.0f - 1e-6f) {
        /* GIMBAL LOCK — pointing straight up or straight down. Yaw and roll
         * become the same axis, so only their COMBINATION is recoverable; roll
         * is pinned to zero and the whole turn is reported as yaw. Any split is
         * arbitrary, and this one at least round-trips through fromEuler.
         *
         * WHICH combination flips with the pitch direction: straight up leaves
         * (yaw - roll) determined, straight down leaves (yaw + roll). Hence the
         * sign, which is not decoration — without it, looking straight DOWN
         * comes back with its yaw negated, and a camera restored from a saved
         * file faces the opposite way. */
        const float toward = sinPitch > 0.0f ? 1.0f : -1.0f;
        pitch = toward * 1.57079632679489661923f;
        roll = 0.0f;
        yaw = std::atan2(toward * 2.0f * (u.x * u.y - u.w * u.z),
                         1.0f - 2.0f * (u.y * u.y + u.z * u.z));
        return;
    }

    pitch = std::asin(sinPitch);
    yaw = std::atan2(2.0f * (u.x * u.z + u.w * u.y), 1.0f - 2.0f * (u.x * u.x + u.y * u.y));
    roll = std::atan2(2.0f * (u.x * u.y + u.w * u.z), 1.0f - 2.0f * (u.x * u.x + u.z * u.z));
}

/* The three axes of an orientation, as directions. What a caller usually wants
 * a rotation FOR — "which way is this thing facing" — without having to
 * remember that forward is +Z. */
inline Vec3 forwardOf(Quat q) { return rotate(q, Vec3::forward()); }
inline Vec3 upOf(Quat q) { return rotate(q, Vec3::up()); }
inline Vec3 rightOf(Quat q) { return rotate(q, Vec3::right()); }

inline bool nearlyEqual(Quat a, Quat b, float tolerance = 1e-5f)
{
    /* Compares ORIENTATIONS, so q and -q count as equal — see slerp. */
    const float cosine = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return cosine >= 1.0f - tolerance;
}

}  // namespace cromwell
