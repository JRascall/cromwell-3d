/* MathTests.cpp — the direction and rotation vocabulary.
 *
 * WHAT IS WORTH PINNING DOWN HERE. Not that a dot product multiplies and adds —
 * that is not a thing that breaks. What breaks in this file is the DEGENERATE
 * CASES, and they break silently:
 *
 *   - lookRotation with a zero-length forward, which happens on the exact frame
 *     something reaches its destination;
 *   - lookRotation straight up, which is a camera's normal state at the zenith;
 *   - fromToRotation between exactly opposed vectors, where the cross product
 *     names no axis and the naive answer is a rotation that does nothing;
 *   - acos of 1.0000001, which is NaN and then spreads.
 *
 * Every one of those produces NaN or a wrong-but-plausible orientation rather
 * than a crash, so it surfaces as a model that vanished or a turret that faces
 * backwards once in a while — the kind of bug that gets blamed on the renderer.
 *
 * The round-trips are the other half: fromEuler/toEuler and
 * lookRotation/forwardOf both have a convention that can be off by an axis or a
 * sign, and the only honest test of a convention is that it inverts.
 */
#include "cromwell/math/Mat4.hpp"
#include "cromwell/math/Quat.hpp"
#include "cromwell/math/Vec3.hpp"
#include "cromwell/math/Vec4.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

constexpr float kPi = 3.14159265358979323846f;

bool finite(Vec3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(Quat q)
{
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
        && std::isfinite(q.w);
}

bool nearly(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}

/* ---- vector directions ------------------------------------------------- */

void testProjectionSplitsAVectorExactly()
{
    const Vec3 v{ 3.0f, 4.0f, 5.0f };
    const Vec3 normal{ 0.0f, 2.0f, 0.0f };  /* deliberately NOT unit length */

    const Vec3 along = projectOnAxis(v, normal);
    const Vec3 across = projectOnPlane(v, normal);

    /* THE IDENTITY THAT MAKES BOTH USEFUL: the two halves reconstruct the
     * original. It also proves the unnormalised normal was handled — the naive
     * expression would scale `along` by four. */
    CHECK(nearlyEqual(along + across, v), "the two projections sum to the input");
    CHECK(nearly(across.y, 0.0f), "and nothing is left on the plane's normal");
    CHECK(nearly(along.y, 4.0f), "while the along component keeps its full length");
}

void testProjectOnPlaneSurvivesADegenerateNormal()
{
    const Vec3 v{ 1.0f, 2.0f, 3.0f };
    CHECK(nearlyEqual(projectOnPlane(v, Vec3::zero()), v),
          "a zero normal defines no plane, so nothing is projected away");
    CHECK(finite(projectOnAxis(v, Vec3::zero())), "and the axis half stays finite");
}

void testReflect()
{
    /* Straight down onto a floor comes straight back up. */
    const Vec3 bounced = reflect(Vec3{ 0.0f, -1.0f, 0.0f }, Vec3{ 0.0f, 1.0f, 0.0f });
    CHECK(nearlyEqual(bounced, Vec3{ 0.0f, 1.0f, 0.0f }), "a head-on bounce reverses");

    /* At 45 degrees it turns the corner and keeps its speed. */
    const Vec3 grazing = reflect(Vec3{ 1.0f, -1.0f, 0.0f }, Vec3{ 0.0f, 3.0f, 0.0f });
    CHECK(nearlyEqual(grazing, Vec3{ 1.0f, 1.0f, 0.0f }),
          "a glancing bounce keeps its tangent, and an unnormalised normal is fine");
}

void testSignedAngleKnowsLeftFromRight()
{
    const Vec3 forward = Vec3::forward();  /* +z */
    const Vec3 up = Vec3::up();

    /* THE QUESTION AN UNSIGNED ANGLE CANNOT ANSWER. Both of these are 90
     * degrees; only the sign says which way to turn. */
    const float toRight = signedAngle(forward, Vec3::right(), up);
    const float toLeft = signedAngle(forward, Vec3::left(), up);

    CHECK(nearly(std::abs(toRight), kPi * 0.5f), "a quarter turn, either way");
    CHECK(nearly(std::abs(toLeft), kPi * 0.5f), "a quarter turn, either way");
    CHECK(toRight * toLeft < 0.0f, "and the two turns have opposite signs");

    CHECK(nearly(angleBetween(forward, forward), 0.0f), "no angle to itself");
    CHECK(nearly(angleBetween(forward, -forward), kPi), "and half a turn to its opposite");
}

void testAngleBetweenNeverReturnsNaN()
{
    /* acos(1.0000001) is NaN, and dot/lengths lands there through rounding for
     * ordinary inputs. The clamp is not theoretical. */
    const Vec3 v{ 0.577350f, 0.577350f, 0.577350f };
    CHECK(std::isfinite(angleBetween(v, v)), "identical vectors give a finite angle");
    CHECK(std::isfinite(angleBetween(v, v * 1000.0f)), "and so do parallel ones of any length");
    CHECK(nearly(angleBetween(Vec3::zero(), v), 0.0f), "a zero vector has no angle, not a NaN");
}

void testMoveTowardsArrivesExactly()
{
    const Vec3 from{ 0.0f, 0.0f, 0.0f };
    const Vec3 to{ 10.0f, 0.0f, 0.0f };

    CHECK(nearlyEqual(moveTowards(from, to, 2.5f), Vec3{ 2.5f, 0.0f, 0.0f }),
          "it moves the distance asked for, not a fraction of the gap");

    /* THE DIFFERENCE FROM lerp: it lands, and it does not overshoot. */
    CHECK(nearlyEqual(moveTowards(from, to, 50.0f), to), "an overlong step stops at the target");
    CHECK(nearlyEqual(moveTowards(to, to, 1.0f), to), "and arriving twice is harmless");
}

void testClampLength()
{
    const Vec3 fast{ 30.0f, 40.0f, 0.0f };  /* length 50 */
    const Vec3 limited = clampLength(fast, 10.0f);

    CHECK(nearly(limited.length(), 10.0f), "capped to the limit");
    CHECK(nearly(angleBetween(fast, limited), 0.0f), "with the direction unchanged");
    CHECK(nearlyEqual(clampLength(Vec3{ 1.0f, 0.0f, 0.0f }, 10.0f), Vec3{ 1.0f, 0.0f, 0.0f }),
          "something already under the limit is untouched");
    CHECK(finite(clampLength(Vec3::zero(), 10.0f)), "and a zero vector does not divide by zero");
}

void testAnyPerpendicularIsActuallyPerpendicular()
{
    for (const Vec3 v : { Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 1.0f, 0.0f },
                          Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{ 1.0f, 1.0f, 1.0f },
                          Vec3{ -0.3f, 0.9f, 0.02f } }) {
        const Vec3 perpendicular = anyPerpendicular(v);
        CHECK(nearly(perpendicular.length(), 1.0f), "unit length");
        CHECK(nearly(dot(perpendicular, v.normalised()), 0.0f), "and genuinely perpendicular");
    }
}

/* ---- rotations --------------------------------------------------------- */

void testLookRotationPointsWhereItWasTold()
{
    const Vec3 target = Vec3{ 3.0f, 1.0f, -4.0f }.normalised();
    const Quat rotation = lookRotation(target);

    /* THE ROUND TRIP THAT DEFINES THE CONVENTION: whatever forward means, the
     * rotation must send it to the direction that was asked for. */
    CHECK(nearlyEqual(forwardOf(rotation), target, 1.0e-4f),
          "the rotation's forward is the direction it was built from");

    /* And the other two axes make a right-handed frame with it. */
    CHECK(nearly(dot(forwardOf(rotation), upOf(rotation)), 0.0f), "up is perpendicular");
    CHECK(nearly(dot(forwardOf(rotation), rightOf(rotation)), 0.0f), "right is perpendicular");
    CHECK(nearlyEqual(cross(rightOf(rotation), upOf(rotation)), forwardOf(rotation), 1.0e-4f),
          "and the frame is right-handed");
}

void testLookRotationKeepsUpUpright()
{
    const Quat rotation = lookRotation(Vec3{ 1.0f, 0.0f, 0.0f });

    /* Looking along the horizon, the up axis should still be up — that is the
     * whole promise of the second argument. */
    CHECK(upOf(rotation).y > 0.99f, "a horizontal look keeps its up vertical");
}

void testLookRotationSurvivesTheDegenerateCases()
{
    /* A ZERO FORWARD. Happens on the frame something arrives at its target and
     * the direction to it becomes (0,0,0). */
    const Quat none = lookRotation(Vec3::zero());
    CHECK(finite(none), "a zero forward gives a finite rotation");
    CHECK(nearlyEqual(none, Quat::identity()), "and specifically identity, not NaN");

    /* STRAIGHT UP AND STRAIGHT DOWN, where forward is parallel to up and the
     * cross product vanishes. A camera sits here whenever it looks at the
     * ground from directly above, which for a tactical game is most of the
     * time. */
    for (const Vec3 vertical : { Vec3::up(), Vec3::down() }) {
        const Quat rotation = lookRotation(vertical);
        CHECK(finite(rotation), "a vertical look is finite");
        CHECK(nearlyEqual(forwardOf(rotation), vertical, 1.0e-3f),
              "and still points where it was told");
    }
}

void testFromToRotationMapsOneOntoTheOther()
{
    const Vec3 a = Vec3{ 1.0f, 2.0f, 3.0f }.normalised();
    const Vec3 b = Vec3{ -4.0f, 0.5f, 2.0f }.normalised();

    const Quat rotation = fromToRotation(a, b);
    CHECK(nearlyEqual(rotate(rotation, a), b, 1.0e-4f), "a lands exactly on b");

    /* It must be the SHORTEST such rotation — a version that took the long way
     * round would still land, and would swing a decal through the wall. */
    CHECK(angleBetween(rotation, Quat::identity()) <= kPi + 1.0e-4f, "by the short arc");
}

void testFromToRotationHandlesTheOpposedCase()
{
    /* THE CASE THAT FAILS SILENTLY. The cross product of opposed vectors is
     * zero, so it names no axis; the unguarded formula produces a zero
     * quaternion, which normalises to identity — leaving the vector pointing
     * exactly the wrong way with no error anywhere. */
    for (const Vec3 v : { Vec3::up(), Vec3::right(), Vec3{ 0.6f, -0.8f, 0.0f } }) {
        const Quat reversal = fromToRotation(v, -v);
        CHECK(finite(reversal), "the opposed case is finite");
        CHECK(nearlyEqual(rotate(reversal, v), -v, 1.0e-4f),
              "and actually reverses the vector rather than doing nothing");
    }

    /* The identical case is the other end of the same guard. */
    CHECK(nearlyEqual(fromToRotation(Vec3::up(), Vec3::up()), Quat::identity()),
          "no rotation is needed between a vector and itself");
}

void testRotateTowardsTurnsAtAFixedRate()
{
    const Quat from = Quat::identity();
    const Quat to = Quat::fromAxisAngle(Vec3::up(), kPi);  /* half a turn */

    const float step = 0.1f;
    const Quat after = rotateTowards(from, to, step);
    CHECK(nearly(angleBetween(from, after), step, 1.0e-3f),
          "one step turns by exactly the rate given");

    /* THE DIFFERENCE FROM slerp, which is the reason this exists: the step is
     * the same size whether there is a lot left to turn or a little. slerp with
     * a fixed t would move 0.1 of the REMAINING angle and slow down forever. */
    const Quat quarter = Quat::fromAxisAngle(Vec3::up(), kPi * 0.5f);
    const Quat shortStep = rotateTowards(quarter, to, step);
    CHECK(nearly(angleBetween(quarter, shortStep), step, 1.0e-3f),
          "and the same rate from half the distance away");

    CHECK(nearlyEqual(rotateTowards(from, to, 10.0f), to), "an overlong step arrives exactly");
    CHECK(nearlyEqual(rotateTowards(to, to, 0.1f), to), "and arriving twice is harmless");
}

void testEulerRoundTrips()
{
    const float angles[] = { -2.5f, -1.0f, -0.2f, 0.0f, 0.3f, 1.2f, 2.9f };

    int checked = 0;
    for (const float yaw : angles) {
        for (const float pitch : { -1.0f, -0.3f, 0.0f, 0.7f, 1.4f }) {
            for (const float roll : { -2.0f, 0.0f, 0.5f }) {
                const Quat built = Quat::fromEuler(yaw, pitch, roll);

                float outYaw = 0.0f;
                float outPitch = 0.0f;
                float outRoll = 0.0f;
                toEuler(built, outYaw, outPitch, outRoll);

                /* COMPARED AS ORIENTATIONS, NOT AS TRIPLES. Several Euler
                 * triples name the same rotation, so requiring the numbers back
                 * would be testing a choice rather than a conversion. */
                CHECK(nearlyEqual(Quat::fromEuler(outYaw, outPitch, outRoll), built, 1.0e-4f),
                      "euler round-trips through the same orientation "
                      "(y %.2f p %.2f r %.2f)",
                      static_cast<double>(yaw), static_cast<double>(pitch),
                      static_cast<double>(roll));
                ++checked;
            }
        }
    }
    CHECK(checked == 105, "every combination was exercised (%d)", checked);
}

void testEulerAtGimbalLock()
{
    /* Pitch at ninety degrees: yaw and roll collapse onto the same axis. The
     * numbers cannot round-trip individually — only their difference is real —
     * so what is checked is that the ORIENTATION survives and nothing is NaN. */
    for (const float pitch : { kPi * 0.5f, -kPi * 0.5f }) {
        const Quat built = Quat::fromEuler(0.7f, pitch, 0.0f);

        float yaw = 0.0f;
        float outPitch = 0.0f;
        float roll = 0.0f;
        toEuler(built, yaw, outPitch, roll);

        CHECK(std::isfinite(yaw) && std::isfinite(outPitch) && std::isfinite(roll),
              "straight up gives finite angles");
        CHECK(nearlyEqual(Quat::fromEuler(yaw, outPitch, roll), built, 1.0e-3f),
              "and the orientation still round-trips at gimbal lock");
    }
}

void testAxisAccessors()
{
    const Quat quarterTurn = Quat::fromAxisAngle(Vec3::up(), kPi * 0.5f);

    /* Turning a quarter left about up sends forward (+z) onto right (+x) in a
     * right-handed Y-up frame. Pinning it here is what stops the convention
     * drifting the next time someone touches lookRotation. */
    CHECK(nearlyEqual(forwardOf(quarterTurn), Vec3::right(), 1.0e-4f),
          "a quarter turn about up takes forward to right");
    CHECK(nearlyEqual(upOf(quarterTurn), Vec3::up(), 1.0e-4f), "and leaves up alone");
    CHECK(nearlyEqual(forwardOf(Quat::identity()), Vec3::forward()),
          "identity faces forward");
}

/* ======================================================= Mat4 and Vec4 =====
 *
 * WHAT IS WORTH PINNING DOWN, and it is entirely the CONVENTIONS. Nobody writes
 * a matrix multiply that is wrong in a way tests catch; what goes wrong is that
 * storage order, vector convention or clip-space depth silently disagrees with
 * the code on the other side, and the result is a plausible-looking wrong scene
 * rather than a crash. Every check below pins one of those three down, so that
 * a port which gets one wrong fails here rather than in a screenshot.
 *
 * The 0..1 depth range is the one to watch hardest — it is the deliberate
 * choice Mat4.hpp argues for (Metal, Vulkan, D3D and the consoles all want it,
 * GL is the odd one out and can be told), and a backend that quietly reverts to
 * GL's -1..1 would halve the depth buffer's usable range without any other
 * symptom. */

void testMat4StorageIsColumnMajor()
{
    /* THE UPLOAD CONTRACT. A shader reads these sixteen floats straight out of
     * a uniform buffer, so where the translation lands in memory IS the
     * interface. Transposed, every object sits somewhere wrong and nothing
     * reports it. */
    const Mat4 t = Mat4::translation({ 1.0f, 2.0f, 3.0f });

    CHECK(nearly(t.at(0, 3), 1.0f), "translation x lives at (row 0, column 3)");
    CHECK(nearly(t.at(1, 3), 2.0f), "translation y lives at (row 1, column 3)");
    CHECK(nearly(t.at(2, 3), 3.0f), "translation z lives at (row 2, column 3)");

    /* Column-major means column 3 is the LAST four floats in memory. */
    CHECK(nearly(t.data()[12], 1.0f), "column-major: translation starts at float 12");
    CHECK(nearly(t.data()[13], 2.0f), "column-major: and runs contiguously");
    CHECK(nearly(t.data()[14], 3.0f), "column-major: three floats, then w");
    CHECK(nearly(t.data()[15], 1.0f), "the homogeneous w stays one");
}

void testCompositionAppliesRightToLeft()
{
    /* `translation * rotation` must rotate FIRST and then translate. The other
     * order makes an object orbit the origin instead of spinning in place,
     * which reads as an animation bug rather than a matrix one. */
    const Quat quarterTurn = Quat::fromAxisAngle({ 0.0f, 1.0f, 0.0f }, kPi * 0.5f);
    const Mat4 placed = Mat4::translation({ 10.0f, 0.0f, 0.0f }) * Mat4::rotation(quarterTurn);

    const Vec3 out = placed.transformPoint({ 1.0f, 0.0f, 0.0f });

    /* +X rotated a quarter turn about +Y goes to -Z, then translates to x=10. */
    CHECK(nearly(out.x, 10.0f, 1e-4f), "translation applied after the rotation (x %.3f)", out.x);
    CHECK(nearly(out.y, 0.0f, 1e-4f), "nothing moved in y (%.3f)", out.y);
    CHECK(nearly(out.z, -1.0f, 1e-4f), "the point rotated to -z first (%.3f)", out.z);
}

void testTransformDirectionIgnoresTranslation()
{
    /* The bug this prevents is normals that move with the object. */
    const Mat4 moved = Mat4::translation({ 100.0f, 100.0f, 100.0f });
    const Vec3 direction = moved.transformDirection({ 0.0f, 1.0f, 0.0f });

    CHECK(nearly(direction.x, 0.0f) && nearly(direction.y, 1.0f) && nearly(direction.z, 0.0f),
          "a direction is unaffected by translation (%.2f %.2f %.2f)",
          direction.x, direction.y, direction.z);
}

void testPerspectiveMapsDepthToZeroOne()
{
    /* THE DELIBERATE CHOICE, checked. Near must land on 0 and far on 1 — not
     * -1 and 1, which is GL's convention and the one every other backend would
     * then have to correct on every projection matrix forever. */
    constexpr float kNear = 0.1f;
    constexpr float kFar  = 100.0f;
    const Mat4 projection = Mat4::perspective(kPi * 0.25f, 16.0f / 9.0f, kNear, kFar);

    const Vec4 atNear = projection * Vec4::point({ 0.0f, 0.0f, -kNear });
    const Vec4 atFar  = projection * Vec4::point({ 0.0f, 0.0f, -kFar });

    CHECK(nearly(atNear.z / atNear.w, 0.0f, 1e-4f),
          "the near plane maps to depth 0, not -1 (got %.4f)", atNear.z / atNear.w);
    CHECK(nearly(atFar.z / atFar.w, 1.0f, 1e-4f),
          "the far plane maps to depth 1 (got %.4f)", atFar.z / atFar.w);
}

void testReversedPerspectiveSwapsTheEnds()
{
    /* Reverse-Z is what the 0..1 choice exists to leave room for: near to 1,
     * far to 0, so the float exponent's density near zero lands where precision
     * is otherwise worst. */
    constexpr float kNear = 0.1f;
    constexpr float kFar  = 100.0f;
    const Mat4 projection = Mat4::perspectiveReversed(kPi * 0.25f, 1.0f, kNear, kFar);

    const Vec4 atNear = projection * Vec4::point({ 0.0f, 0.0f, -kNear });
    const Vec4 atFar  = projection * Vec4::point({ 0.0f, 0.0f, -kFar });

    CHECK(nearly(atNear.z / atNear.w, 1.0f, 1e-4f),
          "reverse-Z: near maps to 1 (got %.4f)", atNear.z / atNear.w);
    CHECK(nearly(atFar.z / atFar.w, 0.0f, 1e-4f),
          "reverse-Z: far maps to 0 (got %.4f)", atFar.z / atFar.w);
}

void testOrthographicMapsDepthToZeroOne()
{
    const Mat4 projection = Mat4::orthographic(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 50.0f);

    const Vec4 atNear = projection * Vec4::point({ 0.0f, 0.0f, -1.0f });
    const Vec4 atFar  = projection * Vec4::point({ 0.0f, 0.0f, -50.0f });

    CHECK(nearly(atNear.z, 0.0f, 1e-4f), "ortho near maps to 0 (got %.4f)", atNear.z);
    CHECK(nearly(atFar.z, 1.0f, 1e-4f), "ortho far maps to 1 (got %.4f)", atFar.z);
}

void testLookAtPutsTheTargetDownNegativeZ()
{
    /* Right-handed, looking down -Z in eye space. A backend or a shader that
     * assumes +Z gets a mirrored world, which is uniquely hard to spot on
     * symmetrical geometry. */
    const Mat4 view = Mat4::lookAt({ 0.0f, 0.0f, 10.0f }, Vec3::zero(), { 0.0f, 1.0f, 0.0f });
    const Vec3 target = view.transformPoint(Vec3::zero());

    CHECK(nearly(target.x, 0.0f, 1e-4f) && nearly(target.y, 0.0f, 1e-4f),
          "the target is centred in eye space (%.3f %.3f)", target.x, target.y);
    CHECK(nearly(target.z, -10.0f, 1e-4f),
          "and ten units down NEGATIVE z (got %.3f)", target.z);
}

void testInverseRoundTrips()
{
    const Quat spin = Quat::fromEuler(0.7f, -0.3f, 0.2f);
    const Mat4 transform = Mat4::transform({ 3.0f, -4.0f, 5.0f }, spin, { 2.0f, 2.0f, 2.0f });
    const Mat4 back = transform.inverse();

    const Vec3 original{ 1.5f, -2.5f, 0.75f };
    const Vec3 there = transform.transformPoint(original);
    const Vec3 andBack = back.transformPoint(there);

    CHECK(nearly(andBack.x, original.x, 1e-3f) && nearly(andBack.y, original.y, 1e-3f)
              && nearly(andBack.z, original.z, 1e-3f),
          "the general inverse round-trips through a scaled transform (%.4f %.4f %.4f)",
          andBack.x, andBack.y, andBack.z);

    /* THE PROJECTION CASE, which is the one screen-space passes need: SSAO and
     * the decal projector both unproject a depth buffer, and inverseRigid would
     * be confidently wrong on a projection matrix. */
    const Mat4 projection = Mat4::perspective(kPi * 0.3f, 1.5f, 0.5f, 200.0f);
    const Mat4 unproject = projection.inverse();
    const Vec4 clip = projection * Vec4::point({ 2.0f, -1.0f, -8.0f });
    const Vec3 eye = (unproject * clip).homogenised();

    CHECK(nearly(eye.x, 2.0f, 1e-3f) && nearly(eye.y, -1.0f, 1e-3f)
              && nearly(eye.z, -8.0f, 1e-3f),
          "a projection inverts back to eye space (%.3f %.3f %.3f)", eye.x, eye.y, eye.z);
}

void testInverseRigidMatchesTheGeneralOneForAViewMatrix()
{
    /* The fast path is only correct for rotation-plus-translation. A view
     * matrix is exactly that, and this is what licenses using it there. */
    const Mat4 view = Mat4::lookAt({ 4.0f, 3.0f, 12.0f }, { 1.0f, 0.0f, 0.0f },
                                   { 0.0f, 1.0f, 0.0f });
    const Mat4 fast = view.inverseRigid();
    const Mat4 general = view.inverse();

    bool agree = true;
    for (int i = 0; i < 16; i++) agree = agree && nearly(fast.m[i], general.m[i], 1e-3f);
    CHECK(agree, "inverseRigid agrees with the general inverse on a view matrix");
}

void testSingularInverseDoesNotProduceNaN()
{
    /* A degenerate matrix is a bug upstream. Identity keeps the symptom local
     * and visible; infinities spread through every fragment that samples the
     * result and get blamed on the shader. */
    Mat4 collapsed = Mat4::scaling({ 1.0f, 0.0f, 1.0f });
    const Mat4 result = collapsed.inverse();

    bool finiteAll = true;
    for (int i = 0; i < 16; i++) finiteAll = finiteAll && std::isfinite(result.m[i]);
    CHECK(finiteAll, "a singular matrix inverts to something finite rather than NaN");
}

void testVec4PointAndDirectionDifferOnlyInW()
{
    /* The pair that prevents "my normals translate with the object". */
    const Vec3 v{ 1.0f, 2.0f, 3.0f };
    CHECK(nearly(Vec4::point(v).w, 1.0f), "a point has w = 1, so translation applies");
    CHECK(nearly(Vec4::direction(v).w, 0.0f), "a direction has w = 0, so it does not");
}

void testVec4HomogeniseSurvivesWWithNoDepth()
{
    /* w == 0 is a point at infinity — a real result for something on the eye
     * plane — and dividing anyway spreads infinities. */
    const Vec4 atInfinity{ 3.0f, 4.0f, 5.0f, 0.0f };
    const Vec3 out = atInfinity.homogenised();

    CHECK(nearly(out.x, 3.0f) && nearly(out.y, 4.0f) && nearly(out.z, 5.0f),
          "w = 0 returns the direction rather than infinities (%.2f %.2f %.2f)",
          out.x, out.y, out.z);
}

}  // namespace

int main()
{
    testProjectionSplitsAVectorExactly();
    testProjectOnPlaneSurvivesADegenerateNormal();
    testReflect();
    testSignedAngleKnowsLeftFromRight();
    testAngleBetweenNeverReturnsNaN();
    testMoveTowardsArrivesExactly();
    testClampLength();
    testAnyPerpendicularIsActuallyPerpendicular();

    testLookRotationPointsWhereItWasTold();
    testLookRotationKeepsUpUpright();
    testLookRotationSurvivesTheDegenerateCases();
    testFromToRotationMapsOneOntoTheOther();
    testFromToRotationHandlesTheOpposedCase();
    testRotateTowardsTurnsAtAFixedRate();
    testEulerRoundTrips();
    testEulerAtGimbalLock();
    testAxisAccessors();

    testMat4StorageIsColumnMajor();
    testCompositionAppliesRightToLeft();
    testTransformDirectionIgnoresTranslation();
    testPerspectiveMapsDepthToZeroOne();
    testReversedPerspectiveSwapsTheEnds();
    testOrthographicMapsDepthToZeroOne();
    testLookAtPutsTheTargetDownNegativeZ();
    testInverseRoundTrips();
    testInverseRigidMatchesTheGeneralOneForAViewMatrix();
    testSingularInverseDoesNotProduceNaN();
    testVec4PointAndDirectionDifferOnlyInW();
    testVec4HomogeniseSurvivesWWithNoDepth();

    if (g_failures == 0) {
        std::printf("math: all checks passed\n");
        return 0;
    }
    std::printf("math: %d check(s) failed\n", g_failures);
    return 1;
}
