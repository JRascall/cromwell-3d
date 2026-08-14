/* Mat4.hpp — a 4x4 transform, in the engine's own convention.
 *
 * SINGLE RESPONSIBILITY: hold sixteen floats, compose them, and build the
 * handful of matrices a renderer actually needs — translate, scale, rotate,
 * look-at, perspective, orthographic.
 *
 * =================== THE THREE CONVENTIONS, STATED ONCE ====================
 *
 * A matrix library has three free choices and every one of them silently
 * transposes, mirrors or inverts a scene if it disagrees with the code around
 * it. None of them is discoverable from a function signature, so all three are
 * written down here and nowhere else.
 *
 * 1. COLUMN-MAJOR STORAGE. m[column * 4 + row]. This is what GLSL, MSL and
 *    every console shading language expect in a uniform buffer, so a matrix
 *    uploads as sixteen contiguous floats with no repacking.
 *
 * 2. COLUMN-VECTOR CONVENTION. A transform applies as `v' = M * v`, and a
 *    composition reads right to left: `projection * view * model` puts the
 *    model transform nearest the vector, which is the order it is applied in.
 *    This is the graphics-standard reading and the one every shader in the tree
 *    already assumes.
 *
 * 3. CLIP-SPACE DEPTH RUNS 0 TO 1, NOT -1 TO 1. This is the one that is a
 *    decision rather than a habit, and it is made for the ports.
 *
 * ==================== WHY 0..1 DEPTH, AND WHY IT MATTERS ===================
 *
 * OpenGL is the only target that wants -1..1. Metal, Vulkan, D3D and every
 * console API use 0..1, and GL can be told to match with a one-line
 * glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE).
 *
 * So the choice is: adopt GL's convention and have four backends fix it up on
 * every projection matrix forever, or adopt everyone else's and have the GL
 * backend say one sentence. The second is obviously right, and it is only
 * tempting to get wrong because GL is the backend that exists today.
 *
 * THE GL BACKEND ACTUALLY DOING IT IS NOT OPTIONAL, and this header said it
 * would happen for some time before it did. Without the call the projections
 * below still produce 0..1, GL still squashes them into 0.5..1.0 on the way to
 * the depth buffer, and NOTHING IS CLIPPED — so the scene draws, looks right,
 * and every depth COMPARISON silently fails: shadows vanish and SSAO
 * reconstructs the wrong positions. See the note beside resolveClipControl in
 * rhi/pc/opengl/OpenGlRenderDevice.cpp, which also explains why it is a
 * per-pass call there rather than a one-off, and why ARB_clip_control (core in
 * GL 4.5, not 4.3) makes 4.5 the real floor for that backend.
 *
 * IT ALSO BUYS DEPTH PRECISION. A 0..1 range is what reverse-Z needs — map the
 * near plane to 1 and the far plane to 0, use a float depth buffer and a
 * Greater test, and the floating-point exponent's density near zero lines up
 * with the far plane where precision is otherwise worst. That turns z-fighting
 * at distance from a tuning problem into a non-problem. With -1..1 the trick
 * does not work at all, because half the range is spent before zero.
 *
 * The engine does not force reverse-Z here — `perspective` is the ordinary
 * mapping — but choosing 0..1 is what leaves the door open.
 *
 * ================== NOT BINARY-COMPATIBLE WITH raylib =====================
 *
 * Vec2, Vec3 and Vec4 are deliberately laid out like raylib's, so the crossing
 * points cast for free. THIS TYPE IS NOT, and the difference is called out
 * because assuming otherwise transposes a matrix silently — which does not
 * crash, it renders a plausible-looking wrong scene. raylib's Matrix declares
 * its fields in an order that does not match this storage, so conversion is an
 * explicit function in math/RaylibInterop.hpp, never a cast or a memcpy.
 *
 * PUBLIC MEMBER, the same exception Vec2/Vec3/Vec4 take: for a mathematical
 * value type the representation is the interface, and no arrangement of sixteen
 * floats is an invalid one.
 */
#pragma once

#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/math/Vec4.hpp"

#include <cmath>

namespace cromwell {

struct Mat4 {
    /* Column-major: m[c * 4 + r]. Identity by default, because a
     * default-constructed transform that scaled everything to nothing would be
     * a uniquely unhelpful zero value. */
    float m[16] = { 1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f };

    static constexpr Mat4 identity() { return Mat4{}; }

    constexpr float  at(int row, int column) const { return m[column * 4 + row]; }
    constexpr float& at(int row, int column)       { return m[column * 4 + row]; }

    /* The raw floats, for a uniform upload. Sixteen contiguous, column-major,
     * which is what every shading language here wants. */
    const float* data() const { return m; }

    /* ---- named constructors --------------------------------------------*/

    static constexpr Mat4 translation(Vec3 t)
    {
        Mat4 result;
        result.at(0, 3) = t.x;
        result.at(1, 3) = t.y;
        result.at(2, 3) = t.z;
        return result;
    }

    static constexpr Mat4 scaling(Vec3 s)
    {
        Mat4 result;
        result.at(0, 0) = s.x;
        result.at(1, 1) = s.y;
        result.at(2, 2) = s.z;
        return result;
    }

    static Mat4 rotation(Quat q)
    {
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        Mat4 r;
        r.at(0, 0) = 1.0f - 2.0f * (yy + zz);
        r.at(0, 1) = 2.0f * (xy - wz);
        r.at(0, 2) = 2.0f * (xz + wy);

        r.at(1, 0) = 2.0f * (xy + wz);
        r.at(1, 1) = 1.0f - 2.0f * (xx + zz);
        r.at(1, 2) = 2.0f * (yz - wx);

        r.at(2, 0) = 2.0f * (xz - wy);
        r.at(2, 1) = 2.0f * (yz + wx);
        r.at(2, 2) = 1.0f - 2.0f * (xx + yy);
        return r;
    }

    /* Scale, then rotate, then translate — the order that means what everyone
     * expects "place this object" to mean. Spelled out because composing it by
     * hand in the wrong order is a classic, and the wrong order produces an
     * object that orbits the origin instead of spinning in place. */
    static Mat4 transform(Vec3 position, Quat rotationQ, Vec3 scale)
    {
        return translation(position) * rotation(rotationQ) * scaling(scale);
    }

    /* ---- view and projection -------------------------------------------*/

    /* WORLD TO EYE. Right-handed, looking down -Z in eye space, which is the
     * convention the shaders here are written against. */
    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
    {
        const Vec3 forward = (target - eye).normalised();
        const Vec3 right   = cross(forward, up).normalised();
        const Vec3 trueUp  = cross(right, forward);

        Mat4 v;
        v.at(0, 0) = right.x;    v.at(0, 1) = right.y;    v.at(0, 2) = right.z;
        v.at(1, 0) = trueUp.x;   v.at(1, 1) = trueUp.y;   v.at(1, 2) = trueUp.z;
        v.at(2, 0) = -forward.x; v.at(2, 1) = -forward.y; v.at(2, 2) = -forward.z;

        v.at(0, 3) = -dot(right, eye);
        v.at(1, 3) = -dot(trueUp, eye);
        v.at(2, 3) = dot(forward, eye);
        return v;
    }

    /* EYE TO CLIP, with depth mapped to 0..1 — see the header note. `fovY` is
     * in radians, vertical, because a vertical field of view is the one that
     * stays meaningful when the window is resized horizontally. */
    static Mat4 perspective(float fovY, float aspect, float nearPlane, float farPlane)
    {
        const float f = 1.0f / std::tan(fovY * 0.5f);

        Mat4 p;
        p.m[0] = 0.0f; p.m[5] = 0.0f; p.m[10] = 0.0f; p.m[15] = 0.0f;
        p.at(0, 0) = f / aspect;
        p.at(1, 1) = f;
        p.at(2, 2) = farPlane / (nearPlane - farPlane);
        p.at(2, 3) = (farPlane * nearPlane) / (nearPlane - farPlane);
        p.at(3, 2) = -1.0f;
        return p;
    }

    /* THE REVERSE-Z FORM: near maps to 1, far to 0. Pair it with a float depth
     * target, a Greater compare and a clear to zero. Offered as its own
     * constructor rather than a flag because every one of those three has to
     * change WITH it — a reverse-Z projection with an ordinary Less test
     * renders nothing at all, and a flag makes that look like one decision
     * instead of four. */
    static Mat4 perspectiveReversed(float fovY, float aspect, float nearPlane, float farPlane)
    {
        const float f = 1.0f / std::tan(fovY * 0.5f);

        Mat4 p;
        p.m[0] = 0.0f; p.m[5] = 0.0f; p.m[10] = 0.0f; p.m[15] = 0.0f;
        p.at(0, 0) = f / aspect;
        p.at(1, 1) = f;
        p.at(2, 2) = nearPlane / (farPlane - nearPlane);
        p.at(2, 3) = (farPlane * nearPlane) / (farPlane - nearPlane);
        p.at(3, 2) = -1.0f;
        return p;
    }

    /* Depth to 0..1, as above. The sun's shadow projection is one of these. */
    static Mat4 orthographic(float left, float right, float bottom, float top,
                             float nearPlane, float farPlane)
    {
        Mat4 o;
        o.at(0, 0) = 2.0f / (right - left);
        o.at(1, 1) = 2.0f / (top - bottom);
        o.at(2, 2) = 1.0f / (nearPlane - farPlane);

        o.at(0, 3) = -(right + left) / (right - left);
        o.at(1, 3) = -(top + bottom) / (top - bottom);
        o.at(2, 3) = nearPlane / (nearPlane - farPlane);
        return o;
    }

    /* ---- operations -----------------------------------------------------*/

    Mat4 transposed() const
    {
        Mat4 t;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t.at(r, c) = at(c, r);
        return t;
    }

    /* THE FAST INVERSE, and it is only correct for a rotation-plus-translation
     * — no scale, no shear, no projection. A view matrix is exactly that, and
     * it is the inverse the renderer takes most often.
     *
     * NAMED SO IT CANNOT BE REACHED FOR BY ACCIDENT. Passing a projection or a
     * scaled model matrix here returns a confidently wrong answer rather than
     * failing, which is why the general `inverse()` below exists and why this
     * one says what it assumes in its name. */
    Mat4 inverseRigid() const
    {
        Mat4 r;
        for (int c = 0; c < 3; c++)
            for (int row = 0; row < 3; row++) r.at(row, c) = at(c, row);

        const Vec3 t{ at(0, 3), at(1, 3), at(2, 3) };
        r.at(0, 3) = -(r.at(0, 0) * t.x + r.at(0, 1) * t.y + r.at(0, 2) * t.z);
        r.at(1, 3) = -(r.at(1, 0) * t.x + r.at(1, 1) * t.y + r.at(1, 2) * t.z);
        r.at(2, 3) = -(r.at(2, 0) * t.x + r.at(2, 1) * t.y + r.at(2, 2) * t.z);
        return r;
    }

    /* The general one, by cofactors. Needed for unprojecting a depth buffer —
     * which the SSAO and decal passes both do — where the matrix being inverted
     * is a projection and inverseRigid would be nonsense.
     *
     * A SINGULAR MATRIX RETURNS IDENTITY rather than filling the result with
     * infinities. A degenerate projection is a bug upstream, and identity keeps
     * the symptom local and visible instead of spreading NaN through every
     * fragment that samples the result. */
    Mat4 inverse() const;

    /* Full 4x4: a point in homogeneous space, perspective divide included. */
    Vec3 transformPoint(Vec3 v) const
    {
        const Vec4 out = *this * Vec4::point(v);
        return out.homogenised();
    }

    /* NO TRANSLATION APPLIED — for a direction or a normal. Note that a normal
     * under a non-uniform scale needs the inverse transpose, not this; that is
     * the caller's problem because only the caller knows whether its matrix has
     * one. */
    Vec3 transformDirection(Vec3 v) const
    {
        return Vec3{ at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z,
                     at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z,
                     at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z };
    }

    Mat4& operator*=(const Mat4& rhs) { *this = *this * rhs; return *this; }

    friend Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        Mat4 result;
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                result.at(r, c) = a.at(r, 0) * b.at(0, c) + a.at(r, 1) * b.at(1, c)
                                + a.at(r, 2) * b.at(2, c) + a.at(r, 3) * b.at(3, c);
            }
        }
        return result;
    }

    friend Vec4 operator*(const Mat4& a, Vec4 v)
    {
        return Vec4{ a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w,
                     a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w,
                     a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w,
                     a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w };
    }
};

inline Mat4 Mat4::inverse() const
{
    const float* a = m;
    float inv[16];

    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15]
             + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15]
             - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15]
             + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14]
             - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];

    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15]
             - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15]
             + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15]
             - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14]
             + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];

    inv[2]  =  a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15]
             + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15]
             - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15]
             + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14]
             - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];

    inv[3]  = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11]
             - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11]
             + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11]
             - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10]
             + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];

    const float determinant = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (determinant == 0.0f) return Mat4::identity();

    const float scale = 1.0f / determinant;
    Mat4 result;
    for (int i = 0; i < 16; i++) result.m[i] = inv[i] * scale;
    return result;
}

}  // namespace cromwell
