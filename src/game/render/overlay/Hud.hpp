/* Hud.hpp — the on-screen text panel.
 *
 * SINGLE RESPONSIBILITY: draw text. Everything it reports arrives in HudModel,
 * so it queries no game state and computes nothing.
 */
#pragma once

#include "raylib.h"

#include "game/lattice/Cell.hpp"
#include "cromwell/ribbon/Ring.hpp"

#include <optional>
#include <string>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

struct HudModel {
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
    RingMask visibleRings;
    Ring     solidRing = Ring::Move;

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

    /* Pushed down by whatever is occupying the top of the screen — the dev
     * toolbar, when it is up. The panel cannot move, being anchored to the
     * viewport, so the thing that can is this. */
    int topOffset = 0;
};

class Hud {
public:
    void draw(const HudModel& model) const;
};

}  // namespace game
