#include "cromwell/camera/OrbitCamera.hpp"

#include "raymath.h"

#include <cmath>

namespace cromwell {

OrbitCamera::OrbitCamera()
{
    camera_.up         = Vector3{ 0.0f, 1.0f, 0.0f };
    camera_.fovy       = 50.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
}

void OrbitCamera::applyPreset(CameraPreset preset, const std::array<float, 6>& freeCamera)
{
    switch (preset) {
        case CameraPreset::Free:
            camera_.position = Vector3{ freeCamera[0], freeCamera[1], freeCamera[2] };
            camera_.target   = Vector3{ freeCamera[3], freeCamera[4], freeCamera[5] };
            break;
        case CameraPreset::Staircase:
            camera_.position = Vector3{ 21.0f, 5.5f, 11.5f };
            camera_.target   = Vector3{ 13.5f, 2.2f, 17.0f };
            break;
        default:
            camera_.position = Vector3{ 34.0f, 24.0f, -6.0f };
            camera_.target   = Vector3{ 11.0f,  1.0f, 12.0f };
            break;
    }
}

void OrbitCamera::orbit(Vector2 mouseDelta)
{
    const Vector3 offset = Vector3Subtract(camera_.position, camera_.target);
    const float radius = Vector3Length(offset);

    const float yaw   = std::atan2(offset.z, offset.x) - mouseDelta.x * 0.006f;
    float pitch = std::asin(Clamp(offset.y / radius, -1.0f, 1.0f)) + mouseDelta.y * 0.006f;
    pitch = Clamp(pitch, 0.08f, 1.50f);            /* never under the floor */

    camera_.position = Vector3{
        camera_.target.x + radius * std::cos(pitch) * std::cos(yaw),
        camera_.target.y + radius * std::sin(pitch),
        camera_.target.z + radius * std::cos(pitch) * std::sin(yaw) };
}

void OrbitCamera::pan(float forward, float right, float deltaSeconds, bool fast,
                      int gridWidth, int gridHeight)
{
    const Vector3 toTarget = Vector3Subtract(camera_.target, camera_.position);
    const float distance = Vector3Length(toTarget);

    const Vector3 flat  = Vector3Normalize(Vector3{ toTarget.x, 0.0f, toTarget.z });
    const Vector3 side  = Vector3Normalize(Vector3CrossProduct(flat, camera_.up));

    Vector3 movement = Vector3Add(Vector3Scale(flat, forward), Vector3Scale(side, right));
    if (Vector3Length(movement) <= 0.001f) return;

    const float speed = distance * 0.55f * deltaSeconds * (fast ? 2.5f : 1.0f);
    movement = Vector3Scale(Vector3Normalize(movement), speed);

    /* keep the rig near the board rather than losing it in the void */
    Vector3 target = Vector3Add(camera_.target, movement);
    target.x = Clamp(target.x, -8.0f, static_cast<float>(gridWidth) + 8.0f);
    target.z = Clamp(target.z, -8.0f, static_cast<float>(gridHeight) + 8.0f);

    const Vector3 applied = Vector3Subtract(target, camera_.target);
    camera_.target   = target;
    camera_.position = Vector3Add(camera_.position, applied);
}

void OrbitCamera::zoom(float wheelDelta)
{
    if (wheelDelta == 0.0f) return;
    const Vector3 offset = Vector3Subtract(camera_.position, camera_.target);
    const float distance = Clamp(Vector3Length(offset) - wheelDelta * 1.8f, 4.0f, 90.0f);
    camera_.position = Vector3Add(camera_.target,
                                  Vector3Scale(Vector3Normalize(offset), distance));
}

}  // namespace cromwell
