/* OrbitCamera.hpp — the camera rig.
 *
 * SINGLE RESPONSIBILITY: hold the camera and move it. Reading the mouse and
 * keyboard is InputHandler's job; this takes deltas.
 *
 * ORBIT is hand-rolled rather than UpdateCamera(CAMERA_THIRD_PERSON), because
 * that mode eats WASD internally — which is exactly why the pan keys did
 * nothing. Mouse delta only; the keys stay ours.
 *
 * WHAT IT HOLDS IS A cromwell::Camera — the same type every other viewpoint in
 * the engine uses, not a raw Camera3D. That is the point of Camera.hpp's "one
 * type" rule reaching the player: the rig's camera carries its own layers and
 * projection, a render pass takes toRaylib() at the boundary exactly as a
 * capture's pass does, and a projection toggle on the player's view is
 * Camera::switchTo rather than a rig rewrite. The rig is therefore MOVE-ONLY,
 * because its camera is.
 */
#pragma once

#include "cromwell/camera/Camera.hpp"

namespace cromwell {

/* NO PRESETS HERE, and there briefly were — a `CameraPreset` enum whose
 * `Staircase` entry was a viewpoint on one game's demo map, hard-coded in an
 * engine header. That is game vocabulary in the engine, the same mistake
 * ViewLayers made when it shipped a `units` switch, and it fails the same
 * test: an RTS, an FPS and a third-person game embedding this rig have no
 * staircase. Where a game starts its camera is the game's data; it places the
 * rig through camera().at().lookingAt(), which is all a pose is. */

class OrbitCamera {
public:
    OrbitCamera() = default;

    /* THE camera — position, lens, layers, everything. Non-const so a caller
     * can edit its layers or switch its projection; the rig only ever moves it. */
    const Camera& camera() const { return camera_; }
    Camera&       camera()       { return camera_; }

    /* Mouse delta in pixels. Pitch is clamped so the rig never goes under the
     * floor. */
    void orbit(Vec2 mouseDelta);

    /* `direction` is a unit-ish vector in rig space: +x right, +z forward.
     * Speed scales with zoom distance so it feels the same close in and far
     * out. The target is clamped near the board rather than lost in the void. */
    void pan(float forward, float right, float deltaSeconds, bool fast,
             int gridWidth, int gridHeight);

    void zoom(float wheelDelta);

private:
    /* VERTICAL field of view, in degrees — which the named constructor makes
     * unambiguous. See camera/Projection.hpp for the fovy trap this closes. */
    Camera camera_ = Camera::perspective(60.0f);
};

}  // namespace cromwell
