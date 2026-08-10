/* OrbitCamera.hpp — the camera rig.
 *
 * SINGLE RESPONSIBILITY: hold the camera and move it. Reading the mouse and
 * keyboard is InputHandler's job; this takes deltas.
 *
 * ORBIT is hand-rolled rather than UpdateCamera(CAMERA_THIRD_PERSON), because
 * that mode eats WASD internally — which is exactly why the pan keys did
 * nothing. Mouse delta only; the keys stay ours.
 */
#pragma once

#include "raylib.h"

#include <array>

namespace cromwell {

/* The rig's starting viewpoints.
 *
 * DEFINED HERE, not in the game's CliOptions, which is where it used to live.
 * A preset is a property of the camera rig — the rig is what knows how to
 * assume one — and having the engine's camera include the game's command line
 * to learn about itself was the whole of that dependency. CliOptions now
 * includes this header and stores one of these. */
enum class CameraPreset : int { Default = 0, Staircase, Free };

class OrbitCamera {
public:
    OrbitCamera();

    void applyPreset(CameraPreset preset, const std::array<float, 6>& freeCamera);

    const Camera3D& camera() const { return camera_; }

    /* Mouse delta in pixels. Pitch is clamped so the rig never goes under the
     * floor. */
    void orbit(Vector2 mouseDelta);

    /* `direction` is a unit-ish vector in rig space: +x right, +z forward.
     * Speed scales with zoom distance so it feels the same close in and far
     * out. The target is clamped near the board rather than lost in the void. */
    void pan(float forward, float right, float deltaSeconds, bool fast,
             int gridWidth, int gridHeight);

    void zoom(float wheelDelta);

private:
    Camera3D camera_{};
};

}  // namespace cromwell
