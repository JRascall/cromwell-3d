/* CameraPawn.hpp — the camera the player possesses.
 *
 * SINGLE RESPONSIBILITY: hold the camera rig and move it.
 *
 * IT OWNS NO INPUT PARSING. Each frame it polls its PlayerController for pan,
 * zoom and orbit intent and applies the motion to itself. That is the
 * controller -> pawn flow, taken from PO's AInGameCameraEntity: the controller
 * reads input and holds intent, the pawn reacts. Communication is one-way, and
 * the pawn never tells the controller anything.
 *
 * WHY POLL RATHER THAN BE PUSHED. Pan and orbit are CONTINUOUS — they are true
 * for as long as a key is down, not at the instant it goes down — so a pawn
 * that waited to be told would need the controller to re-send every frame,
 * which is a push loop wearing a callback's clothes. Discrete things (a click,
 * a cancel) go the other way, as events. PO draws the line in the same place.
 *
 * Zoom is CONSUMED rather than read, because a wheel notch is a quantity that
 * must be applied exactly once; reading it twice would double the movement and
 * reading it never would swallow it.
 */
#pragma once

#include "cromwell/camera/OrbitCamera.hpp"

namespace game {

using namespace cromwell;

class PlayerController;
class World;

class CameraPawn {
public:
    /* Polls `controller` and moves. `world` supplies the bounds the rig is
     * clamped to — the pawn is a thing IN the world, so it is the pawn that
     * knows it may not be flown off the board. */
    void tick(float deltaSeconds, PlayerController& controller, const World& world);

    const OrbitCamera& rig() const { return rig_; }
    OrbitCamera&       rig()       { return rig_; }

    /* What the renderer draws through. */
    const Camera3D& camera() const { return rig_.camera(); }

private:
    OrbitCamera rig_;
};

}  // namespace game
