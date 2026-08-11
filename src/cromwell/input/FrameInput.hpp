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

    /* F2: show/hide the UI widget gallery. A dev screen like the panel above,
     * and separate from it because it is not ImGui — it exercises the engine's
     * own widget kit, which is the only way to judge how it looks. */
    bool toggleUiGallery = false;

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

    /* F5: re-read shaders from disk without restarting.
     *
     * Shading is the one part of a renderer that is tuned by LOOKING at it, and
     * a rebuild-and-relaunch between every attempt is enough friction to stop
     * anyone iterating properly — you settle for the third guess rather than
     * the tenth. The shaders are plain files the app reads at runtime (see
     * ShaderLibrary, and the note in CMakeLists.txt on why the asset trees are
     * named in place rather than copied into the staging directory), so
     * re-reading one costs nothing structural.
     *
     * WHAT ACTUALLY RELOADS IS UP TO THE APPLICATION. This is a key, not a
     * mechanism: each pass that wants to participate has to be told, and a pass
     * that reloads must survive a shader that does not compile by keeping the
     * one it has. A half-typed edit should cost the frame it was typed in and
     * nothing else. */
    bool reloadShaders   = false;

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
