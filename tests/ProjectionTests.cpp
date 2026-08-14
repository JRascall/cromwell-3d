/* ProjectionTests.cpp — camera construction, and the fovy trap.
 *
 * SEPARATE FROM xcom_math_tests BECAUSE IT NEEDS raylib's Camera3D. Nothing
 * here calls a raylib FUNCTION — every helper under test builds a struct and
 * does arithmetic — but the struct's definition arrives with raylib.h, so this
 * binary links the engine's rendering half. It still opens no window.
 *
 * WHAT IT IS FOR. raylib's Camera3D has one field called fovy that means a
 * vertical ANGLE under perspective and a visible HEIGHT in world units under
 * orthographic. Same name, same type, unrelated units, and nothing checks — so
 * a camera can be silently wrong and still render a plausible picture. These
 * tests state the two meanings out loud, and pin the conversion that lets a
 * projection toggle land on the same view rather than on a random zoom.
 *
 * The other half is the degenerate look-at: every top-down camera looks along
 * the world's up axis, and an up vector parallel to the view direction makes
 * MatrixLookAt cross two parallel vectors. The result is NaN in every vertex —
 * a black screen with nothing in the log.
 */
#include "cromwell/camera/Camera.hpp"
#include "cromwell/camera/Projection.hpp"
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

bool nearly(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}

void testFramingDistanceRoundTrips()
{
    /* THE BRIDGE BETWEEN THE TWO PROJECTIONS' UNITS. A perspective camera is
     * framed by moving it; an orthographic one by setting a height. Converting
     * between them is what lets a projection toggle land on the same picture
     * rather than on a random zoom. */
    const float fov = 50.0f;
    for (const float height : { 1.0f, 8.0f, 24.0f, 250.0f }) {
        const float distance = framingDistance(height, fov);
        CHECK(nearly(framedHeight(distance, fov), height, 1.0e-2f),
              "distance and framed height are inverses (%.1f)", static_cast<double>(height));
    }

    /* A narrower lens has to stand further back for the same shot — the
     * relationship every camera operator knows, and worth pinning because
     * getting the half-angle wrong inverts it. */
    CHECK(framingDistance(10.0f, 20.0f) > framingDistance(10.0f, 60.0f),
          "a narrow field of view must stand further back");
}

void testTopDownCameraIsNotDegenerate()
{
    const Vec3 centre{ 12.0f, 0.0f, 12.0f };

    for (const Projection projection : { Projection::Orthographic, Projection::Perspective }) {
        const Camera3D camera = topDownCamera(centre, 26.0f, projection);

        /* LOOKING STRAIGHT DOWN IS THE DEGENERATE LOOK-AT, and it is what every
         * top-down camera does. An up vector parallel to the view direction
         * makes MatrixLookAt cross two parallel vectors, and every pixel comes
         * out NaN — a black screen with nothing in the log. */
        const Vec3 view = (fromRaylib(camera.target) - fromRaylib(camera.position)).normalised();
        const Vec3 up = fromRaylib(camera.up);
        CHECK(std::abs(dot(view, up)) < 0.01f, "the up vector is not parallel to the view");
        CHECK(nearly(up.y, 0.0f), "and is horizontal, so it decides which way is up on the map");

        CHECK(camera.position.y > camera.target.y, "the camera is above what it looks at");
    }
}

void testTopDownFramesTheSameSpanEitherWay()
{
    const Vec3 centre{ 12.0f, 0.0f, 12.0f };
    const float span = 26.0f;
    const float fov = 50.0f;

    const Camera3D ortho = topDownCamera(centre, span, Projection::Orthographic);
    const Camera3D perspective = topDownCamera(centre, span, Projection::Perspective, fov);

    CHECK(ortho.projection == CAMERA_ORTHOGRAPHIC, "one is orthographic");
    CHECK(perspective.projection == CAMERA_PERSPECTIVE, "the other is not");

    /* THE fovy FIELD MEANS TWO DIFFERENT THINGS and this is the assertion that
     * says so: under ortho it is the span in world units, under perspective it
     * is the angle in degrees. Same field, same struct, unrelated numbers. */
    CHECK(nearly(ortho.fovy, span), "the ortho camera's fovy IS the world span");
    CHECK(nearly(perspective.fovy, fov), "the perspective camera's fovy is degrees");

    /* And yet both frame the same amount of world, because the perspective
     * one's altitude was derived rather than guessed. */
    const float altitude = perspective.position.y - perspective.target.y;
    CHECK(nearly(framedHeight(altitude, fov), span, 1.0e-2f),
          "so the perspective camera sees exactly the same span (%.2f)",
          static_cast<double>(framedHeight(altitude, fov)));
}

void testMatchFramingPreservesTheView()
{
    const Vec3 centre{ 12.0f, 0.0f, 12.0f };
    const Camera3D perspective = topDownCamera(centre, 26.0f, Projection::Perspective, 50.0f);

    const Camera3D asOrtho = matchFraming(perspective, Projection::Orthographic);
    CHECK(asOrtho.projection == CAMERA_ORTHOGRAPHIC, "converted");
    CHECK(nearly(asOrtho.fovy, 26.0f, 1.0e-2f),
          "and shows the same span it was showing (%.2f)", static_cast<double>(asOrtho.fovy));

    /* Back again lands where it started — the toggle is reversible, which is
     * what stops a projection switch drifting the zoom every time it is used. */
    const Camera3D backAgain = matchFraming(asOrtho, Projection::Perspective, 50.0f);
    CHECK(backAgain.projection == CAMERA_PERSPECTIVE, "and back");
    CHECK(nearly(backAgain.position.y, perspective.position.y, 1.0e-2f),
          "to the same place it came from");

    /* Converting to the projection it already has is a no-op rather than a
     * re-derivation, so repeated calls cannot creep. */
    const Camera3D unchanged = matchFraming(perspective, Projection::Perspective);
    CHECK(nearly(unchanged.position.y, perspective.position.y),
          "a same-projection convert is a no-op");
}

/* ================= the Mat4 the rhi renderer draws through =================
 *
 * TWO CONVENTIONS EXIST IN THIS TREE AT ONCE during the port: raylib's passes
 * build their own matrices with GL's -1..1 clip depth, and Camera's produce
 * 0..1 for the device. Mixing them in one frame is a depth test against the
 * wrong range — geometry that vanishes or z-fights wholesale — so what is
 * checked here is that Camera's really are the 0..1 kind, and that the lens is
 * read in the right unit for each projection. */

void testCameraDepthRangeIsZeroToOne()
{
    cromwell::Camera camera = cromwell::Camera::perspective(60.0f);
    camera.at({ 0.0f, 0.0f, 0.0f }).lookingAt({ 0.0f, 0.0f, -1.0f });

    constexpr float kNear = 0.1f;
    constexpr float kFar  = 100.0f;
    const Mat4 projection = camera.projectionMatrix(16.0f / 9.0f, kNear, kFar);

    const Vec4 atNear = projection * Vec4::point({ 0.0f, 0.0f, -kNear });
    const Vec4 atFar  = projection * Vec4::point({ 0.0f, 0.0f, -kFar });

    CHECK(nearly(atNear.z / atNear.w, 0.0f, 1e-3f),
          "camera near plane maps to depth 0, not -1 (%.4f)", atNear.z / atNear.w);
    CHECK(nearly(atFar.z / atFar.w, 1.0f, 1e-3f),
          "camera far plane maps to depth 1 (%.4f)", atFar.z / atFar.w);
}

void testOrthographicLensIsAHeightNotAnAngle()
{
    /* THE fovy TRAP, checked on the matrix rather than argued about in a
     * comment. Under orthographic the lens is a world HEIGHT, so a 20-unit lens
     * must frame exactly 20 units top to bottom — reading it as degrees would
     * produce a wildly different scale that still renders something. */
    cromwell::Camera camera = cromwell::Camera::orthographic(20.0f);
    camera.at({ 0.0f, 10.0f, 0.0f }).lookingAt({ 0.0f, 0.0f, 0.0f });

    const Mat4 projection = camera.projectionMatrix(1.0f, 0.1f, 100.0f);

    /* Half the height maps to the top edge of clip space. */
    const Vec4 top = projection * Vec4::point({ 0.0f, 10.0f, -1.0f });
    CHECK(nearly(top.y, 1.0f, 1e-3f),
          "an orthographic lens of 20 frames 20 units tall (%.4f)", top.y);

    const Vec4 centre = projection * Vec4::point({ 0.0f, 0.0f, -1.0f });
    CHECK(nearly(centre.y, 0.0f, 1e-3f), "and centres on zero (%.4f)", centre.y);
}

void testViewMatrixPutsTheTargetDownNegativeZ()
{
    cromwell::Camera camera = cromwell::Camera::perspective(60.0f);
    camera.at({ 0.0f, 0.0f, 10.0f }).lookingAt({ 0.0f, 0.0f, 0.0f });

    const Vec3 target = camera.viewMatrix().transformPoint({ 0.0f, 0.0f, 0.0f });
    CHECK(nearly(target.z, -10.0f, 1e-3f),
          "the view matrix is right-handed, looking down -z (%.3f)", target.z);
}

}  // namespace

int main()
{
    testFramingDistanceRoundTrips();
    testTopDownCameraIsNotDegenerate();
    testTopDownFramesTheSameSpanEitherWay();
    testMatchFramingPreservesTheView();

    testCameraDepthRangeIsZeroToOne();
    testOrthographicLensIsAHeightNotAnAngle();
    testViewMatrixPutsTheTargetDownNegativeZ();

    if (g_failures == 0) {
        std::printf("projection: all checks passed\n");
        return 0;
    }
    std::printf("projection: %d check(s) failed\n", g_failures);
    return 1;
}
