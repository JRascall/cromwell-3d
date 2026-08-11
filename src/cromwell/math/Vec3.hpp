/* Vec3.hpp — a point or direction in three dimensions.
 *
 * SINGLE RESPONSIBILITY: be three floats with the arithmetic that goes with
 * them.
 *
 * ENGINE-WIDE, NOT ENTITY-SPECIFIC. This started life inside Entity.hpp because
 * an entity needed a position, which is exactly the wrong reason for a type to
 * live somewhere: a vector is used by transforms, bounds, lighting, physics,
 * cameras and geometry, and none of those should include an entity header to
 * get at one.
 *
 * NO raylib. cromwell's headless half — the simulation, the tests, any tool —
 * uses vectors without linking a window library. The layout is deliberately
 * identical to raylib's Vector3 (three floats, in order), so the render
 * boundary converts for free; see math/RaylibInterop.hpp for the casts, which
 * is the ONLY place the two types meet.
 *
 * PUBLIC MEMBERS, AND THIS IS THE DELIBERATE EXCEPTION to the project's
 * encapsulation rule. For a mathematical value type the representation IS the
 * interface: `v.x += 1` is the vocabulary of every graphics codebase, every
 * other vector library does the same, and hiding three floats behind
 * `setX(x() + 1)` would make every equation in the engine unreadable while
 * protecting an invariant that does not exist — there is no combination of
 * three floats that is an invalid vector. Encapsulation buys nothing here and
 * costs legibility everywhere. Classes with behaviour and invariants still get
 * private members; see the note in Component.hpp for the ordinary case.
 */
#pragma once

#include <cmath>

namespace cromwell {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    /* ---- named constructors ------------------------------------------- */
    static constexpr Vec3 zero()    { return { 0.0f, 0.0f, 0.0f }; }
    static constexpr Vec3 one()     { return { 1.0f, 1.0f, 1.0f }; }

    /* Y-up, right-handed — the convention raylib uses and therefore the one
     * this engine uses. Naming them is what stops a sign error becoming a
     * discussion. */
    static constexpr Vec3 up()      { return { 0.0f, 1.0f, 0.0f }; }
    static constexpr Vec3 down()    { return { 0.0f, -1.0f, 0.0f }; }
    static constexpr Vec3 right()   { return { 1.0f, 0.0f, 0.0f }; }
    static constexpr Vec3 left()    { return { -1.0f, 0.0f, 0.0f }; }
    static constexpr Vec3 forward() { return { 0.0f, 0.0f, 1.0f }; }
    static constexpr Vec3 back()    { return { 0.0f, 0.0f, -1.0f }; }

    /* ---- magnitude ------------------------------------------------------ */

    /* Prefer this to length() wherever the comparison is all you need — a
     * square root that only feeds a `<` is a square root nobody asked for. */
    float lengthSquared() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSquared()); }

    /* Returns the zero vector when there is no direction to normalise, rather
     * than dividing by zero and producing NaNs that spread silently through
     * everything downstream. */
    Vec3 normalised() const
    {
        const float len = length();
        if (len <= 1e-8f) return zero();
        return { x / len, y / len, z / len };
    }
};

/* ---- arithmetic --------------------------------------------------------- */
inline constexpr Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline constexpr Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline constexpr Vec3 operator-(Vec3 v)         { return { -v.x, -v.y, -v.z }; }
inline constexpr Vec3 operator*(Vec3 v, float s) { return { v.x * s, v.y * s, v.z * s }; }
inline constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }
inline constexpr Vec3 operator/(Vec3 v, float s) { return { v.x / s, v.y / s, v.z / s }; }

/* Component-wise, for scales and extents rather than for geometry. */
inline constexpr Vec3 operator*(Vec3 a, Vec3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }

inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { a = a - b; return a; }
inline Vec3& operator*=(Vec3& v, float s) { v = v * s; return v; }
inline Vec3& operator/=(Vec3& v, float s) { v = v / s; return v; }

/* Exact comparison, deliberately. An epsilon compare hidden behind == is a trap
 * — it is not transitive, so it breaks sorting and containers. Where a
 * tolerance is wanted, ask for it by name with nearlyEqual(). */
inline constexpr bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline constexpr bool operator!=(Vec3 a, Vec3 b) { return !(a == b); }

/* ---- products ----------------------------------------------------------- */
inline constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline constexpr Vec3 cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

/* ---- helpers ------------------------------------------------------------ */
inline float distanceSquared(Vec3 a, Vec3 b) { return (a - b).lengthSquared(); }
inline float distance(Vec3 a, Vec3 b) { return (a - b).length(); }

inline constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

inline bool nearlyEqual(Vec3 a, Vec3 b, float tolerance = 1e-5f)
{
    return distanceSquared(a, b) <= tolerance * tolerance;
}

inline constexpr Vec3 minPerAxis(Vec3 a, Vec3 b)
{
    return { a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z };
}

inline constexpr Vec3 maxPerAxis(Vec3 a, Vec3 b)
{
    return { a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z };
}

/* ================== DIRECTION WORK: the vocabulary gameplay needs ==========
 *
 * These are the operations that otherwise get written out longhand at every
 * call site, differently each time. Unity and Unreal both ship all of them, and
 * the reason is not convenience — it is that the longhand versions have failure
 * cases (a zero-length input, a degenerate axis, an unnormalised normal) that
 * nobody remembers to handle when the expression is three symbols inline.
 * ========================================================================== */

/* The part of `v` that lies in the plane — Unity's Vector3.ProjectOnPlane.
 *
 * THE THIRD-PERSON CAMERA'S WORKHORSE, and every movement system's: take the
 * camera's look direction, flatten it onto the ground plane, and that is
 * "forward" for the player's input. Doing it inline is where the sideways-drift
 * bug comes from, because the obvious expression assumes `normal` is unit
 * length and it usually is not. */
inline Vec3 projectOnPlane(Vec3 v, Vec3 normal)
{
    const float lengthSquared = normal.lengthSquared();
    if (lengthSquared <= 1e-12f) return v;  /* no plane to project onto */
    return v - normal * (dot(v, normal) / lengthSquared);
}

/* The component of `v` ALONG `direction`. The other half of the split above —
 * `projectOnAxis(v, n) + projectOnPlane(v, n) == v`, which is worth knowing
 * when decomposing a velocity into "into the wall" and "along it". */
inline Vec3 projectOnAxis(Vec3 v, Vec3 direction)
{
    const float lengthSquared = direction.lengthSquared();
    if (lengthSquared <= 1e-12f) return Vec3::zero();
    return direction * (dot(v, direction) / lengthSquared);
}

/* Mirrored about a surface — Unity's Vector3.Reflect. Ricochets, bounces, a
 * grenade off a wall. `normal` is normalised here rather than assumed, because
 * a surface normal that arrived from a cross product frequently is not. */
inline Vec3 reflect(Vec3 v, Vec3 normal)
{
    const Vec3 unit = normal.normalised();
    return v - unit * (2.0f * dot(v, unit));
}

/* The unsigned angle between two directions, in radians. */
inline float angleBetween(Vec3 a, Vec3 b)
{
    const float lengths = std::sqrt(a.lengthSquared() * b.lengthSquared());
    if (lengths <= 1e-12f) return 0.0f;

    /* Clamped before acos: dot/lengths can land at 1.0000001 through rounding,
     * and acos of that is NaN — which then spreads into whatever the angle fed.
     * A silent NaN in an AI's facing check is a miserable afternoon. */
    const float cosine = dot(a, b) / lengths;
    return std::acos(cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine));
}

/* The angle from `a` to `b`, signed by which way round `axis` you would turn —
 * Unity's Vector3.SignedAngle.
 *
 * "IS THE TARGET TO MY LEFT OR MY RIGHT" is one of the most-asked questions in
 * a game and the unsigned angle cannot answer it. Turn-in-place animation
 * selection, strafe direction, which way a unit should circle, whether to
 * signal a left or a right turn — all of them are this. */
inline float signedAngle(Vec3 a, Vec3 b, Vec3 axis)
{
    const float unsignedAngle = angleBetween(a, b);
    return dot(axis, cross(a, b)) < 0.0f ? -unsignedAngle : unsignedAngle;
}

/* Toward `target`, by at most `maxDistance` — Unity's Vector3.MoveTowards.
 *
 * NOT lerp, and the difference matters. Lerp moves a FRACTION of the remaining
 * gap, so it never arrives and its speed depends on how far away the target is;
 * this moves a fixed amount and lands exactly. Anything with a speed in metres
 * per second wants this one. */
inline Vec3 moveTowards(Vec3 from, Vec3 target, float maxDistance)
{
    const Vec3 delta = target - from;
    const float length = delta.length();
    if (length <= maxDistance || length <= 1e-8f) return target;
    return from + delta * (maxDistance / length);
}

/* Capped in length, direction unchanged. A velocity limiter, and the safe way
 * to write it — the inline version divides by a length that can be zero. */
inline Vec3 clampLength(Vec3 v, float maxLength)
{
    const float lengthSquared = v.lengthSquared();
    if (lengthSquared <= maxLength * maxLength || lengthSquared <= 1e-12f) return v;
    return v * (maxLength / std::sqrt(lengthSquared));
}

/* Any unit vector perpendicular to `v`. WHICH one is unspecified and callers
 * must not depend on it — it exists so that code needing "some axis across
 * this direction" (a billboard's up, a rotation axis for an exactly-opposed
 * pair) has one answer rather than three subtly different ones.
 *
 * Crosses against the axis `v` leans on LEAST, which is what keeps the result
 * well-conditioned; crossing against a fixed axis produces a near-zero vector
 * whenever `v` happens to be near it. */
inline Vec3 anyPerpendicular(Vec3 v)
{
    const Vec3 unit = v.normalised();
    const float ax = std::fabs(unit.x);
    const float ay = std::fabs(unit.y);
    const float az = std::fabs(unit.z);

    const Vec3 leastAligned = (ax <= ay && ax <= az) ? Vec3{ 1.0f, 0.0f, 0.0f }
                            : (ay <= az)             ? Vec3{ 0.0f, 1.0f, 0.0f }
                                                     : Vec3{ 0.0f, 0.0f, 1.0f };
    return cross(unit, leastAligned).normalised();
}

}  // namespace cromwell
