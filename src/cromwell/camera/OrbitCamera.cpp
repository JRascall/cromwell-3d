#include "cromwell/camera/OrbitCamera.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {

/* The rig's motion is written against Camera's own placement methods, whose
 * semantics carry the intent: `at()` moves the camera and KEEPS its look
 * direction (so panning moves the view rather than swinging it), and
 * `at().lookingAt()` re-aims after a move (what orbit and zoom want, since both
 * move the eye around a fixed target). */

void OrbitCamera::orbit(Vec2 mouseDelta)
{
    const Vec3 target = camera_.target();
    const Vec3 offset = camera_.position() - target;
    const float radius = offset.length();
    if (radius < 1.0e-4f) return;   /* eye on the target: no orbit is defined */

    const float yaw = std::atan2(offset.z, offset.x) - mouseDelta.x * 0.006f;
    float pitch = std::asin(std::clamp(offset.y / radius, -1.0f, 1.0f))
                  + mouseDelta.y * 0.006f;
    pitch = std::clamp(pitch, 0.08f, 1.50f);       /* never under the floor */

    camera_.at({ target.x + radius * std::cos(pitch) * std::cos(yaw),
                 target.y + radius * std::sin(pitch),
                 target.z + radius * std::cos(pitch) * std::sin(yaw) })
           .lookingAt(target);
}

void OrbitCamera::pan(float forward, float right, float deltaSeconds, bool fast,
                      int gridWidth, int gridHeight)
{
    const Vec3 toTarget = camera_.target() - camera_.position();
    const float distance = toTarget.length();

    const Vec3 flat = Vec3{ toTarget.x, 0.0f, toTarget.z }.normalised();
    const Vec3 side = cross(flat, camera_.up()).normalised();

    Vec3 movement = flat * forward + side * right;
    if (movement.length() <= 0.001f) return;

    const float speed = distance * 0.55f * deltaSeconds * (fast ? 2.5f : 1.0f);
    movement = movement.normalised() * speed;

    /* keep the rig near the board rather than losing it in the void */
    Vec3 target = camera_.target() + movement;
    target.x = std::clamp(target.x, -8.0f, static_cast<float>(gridWidth) + 8.0f);
    target.z = std::clamp(target.z, -8.0f, static_cast<float>(gridHeight) + 8.0f);

    /* at() keeps the look direction, so moving the eye by the applied delta
     * lands the target exactly on the clamped point. */
    camera_.at(camera_.position() + (target - camera_.target()));
}

void OrbitCamera::zoom(float wheelDelta)
{
    if (wheelDelta == 0.0f) return;
    const Vec3 target = camera_.target();
    const Vec3 offset = camera_.position() - target;
    const float distance =
        std::clamp(offset.length() - wheelDelta * 1.8f, 4.0f, 90.0f);
    camera_.at(target + offset.normalised() * distance).lookingAt(target);
}

}  // namespace cromwell
