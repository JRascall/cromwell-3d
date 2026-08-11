/* Vec2.hpp — a point or direction in the plane.
 *
 * SINGLE RESPONSIBILITY: be two floats with the arithmetic that goes with them.
 *
 * WHY THIS EXISTS SEPARATELY FROM Vec3. Screen space is two-dimensional and
 * saying so in the type is worth more than the eight bytes it saves: a UI
 * vertex, a cursor position and a texture coordinate are not points in a world
 * that happen to sit at z = 0, and a function taking Vec3 for a screen position
 * invites exactly that confusion. The UI geometry builders are the first
 * caller; anything screen-space or parametric is the general case.
 *
 * NO raylib, for the same reason Vec3 has none — cromwell's headless half uses
 * vectors without linking a window library, and the UI's geometry builders are
 * deliberately on that side so they can be tested without opening a window. The
 * layout is identical to raylib's Vector2 (two floats, in order), so the render
 * boundary converts for free; see math/RaylibInterop.hpp.
 *
 * PUBLIC MEMBERS, THE SAME DELIBERATE EXCEPTION Vec3 documents at length. For a
 * mathematical value type the representation IS the interface, and there is no
 * combination of two floats that is an invalid vector, so there is no invariant
 * for a setter to protect. Do not "fix" this.
 */
#pragma once

#include <cmath>

namespace cromwell {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    /* ---- named constructors ------------------------------------------- */
    static constexpr Vec2 zero() { return { 0.0f, 0.0f }; }

    /* Unit vector at `radians`, measured the way screen space measures: x to
     * the right, y DOWNWARD, so a positive angle turns clockwise on screen.
     * Every angle in the UI geometry is in this convention — the loaders all
     * anchor at 12 o'clock (-pi/2) and sweep clockwise, which is what a spinner
     * does. */
    static Vec2 fromAngle(float radians)
    {
        return { std::cos(radians), std::sin(radians) };
    }

    /* ---- arithmetic ---------------------------------------------------- */
    Vec2 operator+(const Vec2& rhs) const { return { x + rhs.x, y + rhs.y }; }
    Vec2 operator-(const Vec2& rhs) const { return { x - rhs.x, y - rhs.y }; }
    Vec2 operator*(float s) const { return { x * s, y * s }; }
    Vec2 operator/(float s) const { return { x / s, y / s }; }

    Vec2& operator+=(const Vec2& rhs) { x += rhs.x; y += rhs.y; return *this; }
    Vec2& operator-=(const Vec2& rhs) { x -= rhs.x; y -= rhs.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }

    Vec2 operator-() const { return { -x, -y }; }

    /* ---- geometry ------------------------------------------------------ */
    float length() const { return std::sqrt(x * x + y * y); }
    float lengthSquared() const { return x * x + y * y; }

    /* Normalised, or the zero vector when there is no direction to return.
     * Degenerate input is a real case here — a chip whose two corners coincide
     * has a zero-length edge, and a NaN normal would poison every vertex built
     * from it, so this returns something drawable instead of something
     * infectious. */
    Vec2 normalised() const
    {
        const float len = length();
        return len > 1e-8f ? Vec2{ x / len, y / len } : Vec2{ 0.0f, 0.0f };
    }

    /* Rotated a quarter turn, which in y-down screen space is the OUTWARD
     * normal of an edge wound clockwise. The outline builders lean on this. */
    Vec2 perpendicular() const { return { y, -x }; }

    float dot(const Vec2& rhs) const { return x * rhs.x + y * rhs.y; }
};

inline Vec2 operator*(float s, const Vec2& v) { return { v.x * s, v.y * s }; }

inline Vec2 lerp(const Vec2& a, const Vec2& b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

}  // namespace cromwell
