#include "cromwell/camera/Viewport.hpp"

#include "cromwell/math/RaylibInterop.hpp"

#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>

namespace cromwell {

namespace {

/* The projection matrix raylib itself would build for this camera and this
 * rectangle.
 *
 * REBUILT RATHER THAN READ BACK FROM rlgl, and deliberately: rlgl's current
 * projection is whatever the last pass left bound, which during a frame is
 * frequently a shadow map's or an overlay's ortho. Asking the pipeline what it
 * is projecting through gives the right answer only if you ask at the right
 * moment, and a picking helper has no way to know when that is.
 *
 * The cull distances DO come from rlgl, because those are global to the
 * renderer and the whole point is that this agrees with what was drawn — a
 * near plane guessed here rather than read would put picks and pixels
 * fractionally out of step, which reads as "the cursor is slightly off" and is
 * miserable to track down. This mirrors GetWorldToScreenEx in rcore.c; if
 * raylib's own construction ever changes, this is the file that has to follow
 * it. */
Matrix projectionMatrix(const Camera3D& camera, float aspect)
{
    const double near = rlGetCullDistanceNear();
    const double far  = rlGetCullDistanceFar();

    if (camera.projection == CAMERA_ORTHOGRAPHIC) {
        const double top = camera.fovy / 2.0;
        const double right = top * static_cast<double>(aspect);
        return MatrixOrtho(-right, right, -top, top, near, far);
    }
    return MatrixPerspective(static_cast<double>(camera.fovy) * DEG2RAD,
                             static_cast<double>(aspect), near, far);
}

Vector3 viewForward(const Camera3D& camera)
{
    return Vector3Normalize(Vector3Subtract(camera.target, camera.position));
}

}  // namespace

Viewport Viewport::ofWindow(const Camera3D& camera)
{
    return Viewport(camera, Vec2::zero(),
                    Vec2{ static_cast<float>(GetScreenWidth()),
                          static_cast<float>(GetScreenHeight()) });
}

Viewport::Viewport(const Camera3D& camera, Vec2 sizePx)
    : Viewport(camera, Vec2::zero(), sizePx) {}

Viewport::Viewport(const Camera3D& camera, Vec2 originPx, Vec2 sizePx)
    : camera_(camera), origin_(originPx), size_(sizePx)
{
    /* A zero-sized viewport divides by zero in every conversion below and would
     * hand back NaNs that poison whatever they touch. It happens for real —
     * a window minimised on Windows reports a zero framebuffer — so it is
     * clamped rather than asserted, and the frame draws nothing useful instead
     * of nothing at all. */
    size_.x = std::max(size_.x, 1.0f);
    size_.y = std::max(size_.y, 1.0f);
}

bool Viewport::contains(Vec2 screenPx) const
{
    return screenPx.x >= origin_.x && screenPx.x <= origin_.x + size_.x
        && screenPx.y >= origin_.y && screenPx.y <= origin_.y + size_.y;
}

Ray Viewport::rayThrough(Vec2 screenPx) const
{
    /* Normalised device coordinates: x right, y UP — the flip is the whole of
     * the difference between screen space and GL's clip space, and forgetting
     * it gives a ray that tracks the cursor upside down. */
    const Vec2 local = screenPx - origin_;
    const float ndcX = (2.0f * local.x) / size_.x - 1.0f;
    const float ndcY = 1.0f - (2.0f * local.y) / size_.y;

    const Matrix view = MatrixLookAt(camera_.position, camera_.target, camera_.up);
    const Matrix projection = projectionMatrix(camera_, aspect());

    const Vector3 nearPoint = Vector3Unproject(Vector3{ ndcX, ndcY, 0.0f }, projection, view);
    const Vector3 farPoint  = Vector3Unproject(Vector3{ ndcX, ndcY, 1.0f }, projection, view);

    Ray ray{};
    ray.direction = fromRaylib(Vector3Normalize(Vector3Subtract(farPoint, nearPoint)));

    /* PERSPECTIVE CONVERGES, ORTHOGRAPHIC DOES NOT. Every perspective ray leaves
     * the eye, so the eye is the origin. An ortho camera has no eye — its rays
     * are parallel and enter through a plane — so the origin is where the cursor
     * meets that plane, which is what unprojecting at z = -1 gives. See the
     * note on rayThrough in the header for why this is worth a branch. */
    ray.origin = fromRaylib(camera_.projection == CAMERA_ORTHOGRAPHIC
                                ? Vector3Unproject(Vector3{ ndcX, ndcY, -1.0f }, projection, view)
                                : camera_.position);
    return ray;
}

Vec3 Viewport::pointAtDepth(Vec2 screenPx, float metres) const
{
    const Ray ray = rayThrough(screenPx);
    const Vec3 forward = fromRaylib(viewForward(camera_));

    /* Along the VIEW AXIS, not along the ray — see the header. Dividing by the
     * ray's forward component is what turns a distance-from-eye into a depth,
     * and it is why a grid of these points is a flat plane rather than a bowl.
     * The component cannot be zero for a ray through the viewport (that would
     * be a 180-degree field of view), but it is guarded anyway because a
     * degenerate camera should give a useless point rather than an infinite
     * one. */
    const float alongView = dot(ray.direction, forward);
    const float t = std::abs(alongView) > 1.0e-6f ? metres / alongView : metres;

    return ray.at(t);
}

std::optional<Vec3> Viewport::pointOnPlane(Vec2 screenPx, Vec3 planePoint, Vec3 planeNormal) const
{
    const Ray ray = rayThrough(screenPx);
    const Vec3 normal = planeNormal.normalised();

    const float denominator = dot(ray.direction, normal);
    if (std::abs(denominator) < 1.0e-6f) {
        return std::nullopt;  /* looking along the plane; it has no near side */
    }

    const float t = dot(planePoint - ray.origin, normal) / denominator;
    if (t < 0.0f) {
        return std::nullopt;  /* behind the camera — see the header */
    }

    return ray.at(t);
}

std::optional<Vec3> Viewport::pointOnGround(Vec2 screenPx, float height) const
{
    return pointOnPlane(screenPx, Vec3{ 0.0f, height, 0.0f }, Vec3::up());
}

ScreenPoint Viewport::project(Vec3 worldPosition) const
{
    ScreenPoint point;

    const Vec3 toPoint = worldPosition - fromRaylib(camera_.position);
    point.distance = toPoint.length();
    point.depth = dot(toPoint, fromRaylib(viewForward(camera_)));

    /* THE TEST THAT SAVES EVERY CALLER. Depth along the view direction, not
     * straight-line distance: a point beside the camera is in front of it by
     * distance and behind it by depth, and it is depth the projection divides
     * by. Compared against the real near plane rather than zero, because a
     * point between the eye and the near plane does not survive the divide
     * either. Under an orthographic camera nothing converges and the divide is
     * safe, but a point behind the eye is still not visible, so the same test
     * holds. */
    const auto near = static_cast<float>(rlGetCullDistanceNear());
    if (point.depth <= near) {
        return point;
    }
    point.inFront = true;

    const Matrix view = MatrixLookAt(camera_.position, camera_.target, camera_.up);
    const Matrix projection = projectionMatrix(camera_, aspect());

    Quaternion clip{ worldPosition.x, worldPosition.y, worldPosition.z, 1.0f };
    clip = QuaternionTransform(clip, view);
    clip = QuaternionTransform(clip, projection);

    /* Guarded even though `inFront` has already ruled out the case that makes w
     * vanish: a caller handed a camera with a degenerate target and up would
     * otherwise get NaNs that spread silently into a layout. */
    const float w = std::abs(clip.w) > 1.0e-6f ? clip.w : 1.0e-6f;

    /* y is negated on the way out of clip space: GL's NDC is y-up, and
     * everything past this class is y-down device pixels. */
    const float ndcX = clip.x / w;
    const float ndcY = -clip.y / w;

    point.position = { origin_.x + (ndcX + 1.0f) * 0.5f * size_.x,
                       origin_.y + (ndcY + 1.0f) * 0.5f * size_.y };
    point.onScreen = contains(point.position);
    return point;
}

Vec2 Viewport::toNormalised(Vec2 screenPx) const
{
    return { (screenPx.x - origin_.x) / size_.x, (screenPx.y - origin_.y) / size_.y };
}

Vec2 Viewport::fromNormalised(Vec2 normalised) const
{
    return { origin_.x + normalised.x * size_.x, origin_.y + normalised.y * size_.y };
}

float Viewport::pixelsPerMetreAt(float depth) const
{
    /* Half the viewport's height in metres at this depth, from the vertical
     * field of view — then pixels per metre is half the height in pixels over
     * it. Vertical rather than horizontal because fovy is what raylib's camera
     * stores and what the projection above is built from, so an ultrawide window
     * widens the view instead of shrinking everything. */
    if (camera_.projection == CAMERA_ORTHOGRAPHIC) {
        /* fovy IS the visible height in world units under ortho, and it does not
         * change with depth — which is the entire reason to use one. */
        const float halfHeight = std::max(camera_.fovy * 0.5f, 1.0e-6f);
        return (size_.y * 0.5f) / halfHeight;
    }

    const float halfHeight = std::max(depth, 1.0e-4f)
                             * std::tan(camera_.fovy * DEG2RAD * 0.5f);
    return halfHeight > 1.0e-6f ? (size_.y * 0.5f) / halfHeight : 0.0f;
}

float Viewport::projectedRadius(Vec3 centre, float metres) const
{
    const ScreenPoint point = project(centre);
    if (!point.inFront) {
        return 0.0f;
    }
    return metres * pixelsPerMetreAt(point.depth);
}

Vec2 Viewport::clampToEdge(Vec2 screenPx, float marginPx) const
{
    /* The margin cannot exceed half the viewport, or the clamp inverts and a
     * marker lands outside the rectangle it was supposed to be pushed into. */
    const float inset = std::min(marginPx, std::min(size_.x, size_.y) * 0.5f);
    return { std::clamp(screenPx.x, origin_.x + inset, origin_.x + size_.x - inset),
             std::clamp(screenPx.y, origin_.y + inset, origin_.y + size_.y - inset) };
}

EdgeMarker Viewport::edgeMarker(Vec3 worldPosition, float marginPx) const
{
    EdgeMarker marker;
    const ScreenPoint point = project(worldPosition);

    /* BEHIND THE CAMERA, MIRRORED THROUGH THE CENTRE. `project` refuses to
     * produce a position for such a point, and rightly — but an offscreen
     * indicator is exactly the caller that still needs a DIRECTION for it. The
     * direction is the one thing that survives: reflect the world point's
     * screen-space offset through the centre and the arrow points back over the
     * correct shoulder instead of leading the player the wrong way round.
     *
     * The reflection is computed from a point pushed in front of the camera
     * along the line to the target, which is the cheapest thing that keeps the
     * bearing right without clipping the segment against the near plane. */
    Vec2 direction;
    if (point.inFront) {
        if (point.onScreen) {
            marker.position = point.position;
            return marker;  /* offscreen stays false — nothing to point at */
        }
        direction = point.position - centre();
    } else {
        /* Mirror: take the target's offset from the camera, flip it across the
         * view axis, project THAT, and invert the result about the centre. */
        const Vec3 forward = fromRaylib(viewForward(camera_));
        const Vec3 toPoint = worldPosition - fromRaylib(camera_.position);
        const Vec3 lateral = toPoint - forward * dot(toPoint, forward);

        /* One metre ahead is enough to project; the bearing does not depend on
         * how far ahead it is. */
        const Vec3 mirrored = fromRaylib(camera_.position) + forward + lateral;
        const ScreenPoint behind = project(mirrored);
        direction = behind.inFront ? (centre() - behind.position) : Vec2{ 0.0f, 1.0f };
    }

    if (direction.lengthSquared() < 1.0e-8f) {
        /* Dead centre and behind — no bearing exists. Point down, which reads as
         * "behind you" and is what a HUD would show anyway. */
        direction = { 0.0f, 1.0f };
    }

    marker.offscreen = true;
    marker.angleRadians = std::atan2(direction.y, direction.x);

    /* Cast from the centre along the bearing to the inset rectangle's edge,
     * rather than clamping the projected position: clamping a point that is far
     * off one axis pins it to a corner, and a row of markers all pile up in the
     * same corner instead of spreading along the edge the targets are actually
     * beyond. */
    const float inset = std::min(marginPx, std::min(size_.x, size_.y) * 0.5f);
    const float halfWidth = size_.x * 0.5f - inset;
    const float halfHeight = size_.y * 0.5f - inset;

    const Vec2 unit = direction.normalised();
    const float scaleX = std::abs(unit.x) > 1.0e-6f ? halfWidth / std::abs(unit.x) : 1.0e9f;
    const float scaleY = std::abs(unit.y) > 1.0e-6f ? halfHeight / std::abs(unit.y) : 1.0e9f;
    const float reach = std::min(scaleX, scaleY);

    marker.position = centre() + unit * reach;
    return marker;
}

}  // namespace cromwell
