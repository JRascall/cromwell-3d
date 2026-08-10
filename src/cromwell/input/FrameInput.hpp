/* FrameInput.hpp — one frame's worth of device state.
 *
 * SINGLE RESPONSIBILITY: carry input as data, so Application decides what it
 * MEANS without ever calling IsKeyPressed itself.
 */
#pragma once

#include "raylib.h"

namespace cromwell {

struct FrameInput {
    float deltaSeconds = 0.0f;

    /* discrete actions */
    bool setStoreyGround = false;
    bool setStoreyMiddle = false;
    bool setStoreyTop    = false;

    /* Hand the storey cut back to whatever the game wants to derive it from —
     * the three above are a statement that the player wants a specific floor
     * and nothing else. Separate from them because "no opinion" is not one of
     * the floors and encoding it as one would need a sentinel. */
    bool setStoreyDynamic = false;
    bool cycleRing       = false;
    bool toggleCutaway   = false;
    bool toggleLos       = false;
    bool toggleCover     = false;
    bool toggleGrenade   = false;
    bool toggleOcclusion = false;
    bool toggleBake      = false;
    bool toggleFlatView  = false;   /* geometry without any shading — see Application */
    bool toggleDevView   = false;   /* show/hide the F1 dev panel                     */

    /* F9: start recording profiler frames, F9 again to stop and write the
     * capture. A toggle rather than a hold, because the interesting window is
     * usually "from just before that stutter until just after" and you cannot
     * hold a key through something you are also trying to reproduce. */
    bool toggleCapture   = false;

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

}  // namespace cromwell
