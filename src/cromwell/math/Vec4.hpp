/* Vec4.hpp — four floats that travel together.
 *
 * SINGLE RESPONSIBILITY: be four floats with the arithmetic that goes with
 * them, for the things that genuinely have four components.
 *
 * WHAT ACTUALLY USES THIS, because it is not "a Vec3 with one more": material
 * factors (metallic, roughness, occlusion, emissive strength), transmission
 * tints, glass remap ranges, homogeneous positions on their way through a
 * matrix, and shader uniforms that pack four unrelated scalars because a
 * uniform slot is a vec4 whether you fill it or not. Those last two are the
 * common case here and they are why this type carries no `normalise` or
 * `cross`: a packed uniform has no length worth taking.
 *
 * NO raylib, for the reason Vec3.hpp sets out at length and which now has teeth:
 * the engine ships to Windows, Linux, macOS and consoles, and a value type that
 * names a graphics API drags that API into every signature that carries one.
 * The layout is deliberately identical to raylib's Vector4 (four floats, in
 * order) so that the crossing points convert for free while the passes migrate
 * — see math/RaylibInterop.hpp, which is scaffolding with a demolition date.
 *
 * PUBLIC MEMBERS, the same deliberate exception to the project's encapsulation
 * rule that Vec2 and Vec3 take: for a mathematical value type the
 * representation IS the interface, and there is no combination of four floats
 * that is an invalid one.
 */
#pragma once

#include "cromwell/math/Vec3.hpp"

#include <cmath>

namespace cromwell {

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    /* ---- named constructors --------------------------------------------*/
    static constexpr Vec4 zero() { return { 0.0f, 0.0f, 0.0f, 0.0f }; }
    static constexpr Vec4 one()  { return { 1.0f, 1.0f, 1.0f, 1.0f }; }

    static constexpr Vec4 splat(float v) { return { v, v, v, v }; }

    /* A POINT, w = 1, so a translation applies to it. */
    static constexpr Vec4 point(Vec3 v) { return { v.x, v.y, v.z, 1.0f }; }

    /* A DIRECTION, w = 0, so a translation does NOT. Getting this pair the
     * wrong way round is the classic "my normals move with the object" bug, and
     * naming both is cheaper than remembering which literal means which. */
    static constexpr Vec4 direction(Vec3 v) { return { v.x, v.y, v.z, 0.0f }; }

    /* ---- access ---------------------------------------------------------*/
    constexpr Vec3 xyz() const { return { x, y, z }; }

    /* PERSPECTIVE DIVIDE, guarded. A w of zero is a point at infinity — a
     * legitimate result of projecting something on the eye plane — and dividing
     * anyway produces infinities that spread silently through whatever consumes
     * the result. Returns the direction unchanged instead, which is what a
     * point at infinity actually is. */
    constexpr Vec3 homogenised() const
    {
        return (w == 0.0f) ? xyz() : Vec3{ x / w, y / w, z / w };
    }

    /* ---- arithmetic, chaining like Vec2/Vec3 already do ------------------*/
    constexpr Vec4& operator+=(Vec4 v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
    constexpr Vec4& operator-=(Vec4 v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
    constexpr Vec4& operator*=(float s) { x *= s; y *= s; z *= s; w *= s; return *this; }
    constexpr Vec4& operator/=(float s) { x /= s; y /= s; z /= s; w /= s; return *this; }
};

constexpr Vec4 operator+(Vec4 a, Vec4 b) { return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
constexpr Vec4 operator-(Vec4 a, Vec4 b) { return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
constexpr Vec4 operator-(Vec4 v) { return { -v.x, -v.y, -v.z, -v.w }; }
constexpr Vec4 operator*(Vec4 v, float s) { return { v.x * s, v.y * s, v.z * s, v.w * s }; }
constexpr Vec4 operator*(float s, Vec4 v) { return v * s; }
constexpr Vec4 operator/(Vec4 v, float s) { return { v.x / s, v.y / s, v.z / s, v.w / s }; }

/* COMPONENTWISE, which is what a packed uniform wants — four independent
 * scalars scaled by four others. Deliberately not spelled `operator*` between
 * two Vec4s in a way that could be mistaken for a dot product. */
constexpr Vec4 mul(Vec4 a, Vec4 b) { return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w }; }

constexpr float dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

constexpr bool operator==(Vec4 a, Vec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
constexpr bool operator!=(Vec4 a, Vec4 b) { return !(a == b); }

inline Vec4 lerp(Vec4 a, Vec4 b, float t) { return a + (b - a) * t; }

}  // namespace cromwell
