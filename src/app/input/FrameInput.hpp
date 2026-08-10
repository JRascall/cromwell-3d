/* FrameInput.hpp — one frame's worth of device state.
 *
 * SINGLE RESPONSIBILITY: carry input as data, so Application decides what it
 * MEANS without ever calling IsKeyPressed itself.
 */
#pragma once

#include "raylib.h"

namespace xcom {

struct FrameInput {
    float deltaSeconds = 0.0f;

    /* discrete actions */
    bool setStoreyGround = false;
    bool setStoreyMiddle = false;
    bool setStoreyTop    = false;
    bool cycleRing       = false;
    bool toggleCutaway   = false;
    bool toggleLos       = false;
    bool toggleCover     = false;
    bool toggleGrenade   = false;
    bool toggleOcclusion = false;
    bool toggleBake      = false;
    bool toggleFlatView  = false;   /* geometry without any shading — see Application */
    bool toggleDevView   = false;   /* show/hide the F1 dev panel                     */

    /* Copy the camera to the clipboard as a ready-to-paste --cam argument.
     * Reproducing "it looks wrong from here" otherwise means describing a
     * viewpoint in prose and hoping it lands; this makes it exact. */
    bool copyCamera      = false;
    bool resetWorld      = false;

    /* camera */
    bool    orbiting = false;
    Vector2 mouseDelta{};
    float   panForward = 0.0f;
    float   panRight   = 0.0f;
    bool    panFast    = false;
    float   wheel      = 0.0f;

    /* sun, as held-down rates rather than presses — lighting is tuned by
     * sweeping it and watching, not by stepping it */
    float sunAzimuthRate   = 0.0f;
    float sunElevationRate = 0.0f;

    /* pointer */
    Vector2 mousePosition{};
    bool    leftPressed  = false;
    bool    leftReleased = false;

    bool windowResized = false;
};

}  // namespace xcom
