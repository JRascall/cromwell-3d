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

}  // namespace cromwell
