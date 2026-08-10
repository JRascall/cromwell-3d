/* DevModel.hpp — everything the dev UI reports, gathered once per frame.
 *
 * SINGLE RESPONSIBILITY: carry state to the F1 panels. FrameRenderer fills it
 * from the frame it is about to draw; DevView reads it and never queries the
 * game itself. That one-way flow is the reason the dev UI can be switched off
 * without changing what the renderer does.
 *
 * IT WAS THE HUD'S MODEL. The on-screen text panel in the top-left corner is
 * gone — the dev panel reports all of it, in a form that can be interacted with
 * rather than only read — so the model moved to the only thing still reading
 * it. The name HUD is now free for the real one.
 */
#pragma once

#include "game/lattice/Cell.hpp"

#include <optional>
#include <string>

namespace game {

struct DevModel {
    std::string selectedName;
    Cell        selectedCell;

    int  isoLevel = 0;

    const char* ringOverrideName = "auto";
    bool softCutaway = true;
    bool losMode = false;
    bool showCover = true;
    bool grenadeArmed = false;

    int moveLoops = 0, moveEdges = 0;
    int sprintLoops = 0, sprintEdges = 0;

    std::optional<Cell>  hoverCell;
    std::optional<float> hoverCost;     /* absent when unreachable */
    bool                 hoverRestOk = false;

    /* lighting, so the sun can be tuned without guessing where it is */
    float sunAzimuth = 0.0f;
    float sunElevation = 0.0f;
    bool  shadowsActive = false;
    bool  occlusionActive = false;
    bool  bakedSun = false;
    int   debugView = 0;   /* 0 off, 1 geometry, 2 probe mirror, 3 rooms */

    /* How many reflection probes the room partition placed. Shown beside the
     * rooms view, where "how many rooms did it find" is the first question the
     * view raises and the one number the colours cannot answer. */
    int   probeCount = 0;

    /* WHERE THE CAMERA IS, spelled as the --cam argument that reproduces it.
     *
     * Not a convenience. A viewpoint described in prose — "looking at the south
     * wall from outside" — is not the same viewpoint, and an artefact that only
     * appears from one angle cannot be investigated from a different one. This
     * is the line that turns "it looks wrong here" into something anybody else
     * can render. F3 copies it. */
    std::string cameraArgs;

    std::string status;
};

}  // namespace game
