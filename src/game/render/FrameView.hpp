/* FrameView.hpp — everything the renderer needs to draw one frame.
 *
 * SINGLE RESPONSIBILITY: carry, as plain data, the answers the renderer would
 * otherwise have to go and fetch.
 *
 * WHY THIS EXISTS. FrameRenderer used to be part of Application, which also
 * owned the camera rig, the selection, the hovered cell and the path preview —
 * so a draw method simply read `hovered_` and nobody could tell, from the
 * renderer's own code, what it actually depended on. Application now fills one
 * of these each frame and hands it over, which makes that dependency a list
 * you can read rather than a search you have to perform.
 *
 * POINTERS FOR THE BIG THINGS, VALUES FOR THE SMALL ONES. The world and the
 * path preview are borrowed for the duration of the call and outlive it; the
 * toggles and the hovered index are cheaper to copy than to indirect.
 *
 * PUBLIC FIELDS, deliberately. This is a one-shot data carrier: no invariant
 * spans its fields, so no setter could validate anything; it is filled by one
 * caller, read by one callee, and does not outlive the call that made it. The
 * project's rule is private members behind accessors — see any component — and
 * this is the documented exception to it, not an oversight.
 */
#pragma once

#include "raylib.h"

#include "cromwell/decal/Decal.hpp"
#include "cromwell/overlay/RenderEffects.hpp"
#include "cromwell/overlay/ViewLayers.hpp"
#include "cromwell/ribbon/RibbonRenderer.hpp"
#include "game/movement/search/PathPoint.hpp"
#include "game/render/ribbon/RibbonTuning.hpp"
#include "game/render/scene/CutawayView.hpp"
#include "game/ui/state/UIState.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace game {

using namespace cromwell;

class GameState;
class MoveAnimator;
class RingSelector;

/* The view toggles: what the player and the dev panel have switched on.
 *
 * One struct rather than seven loose bools because they travel together —
 * every one of them is written by input and read by drawing, and a set that
 * moves as a unit should be named as one. */
struct ViewSettings {
    /* Which passes are submitted. */
    ViewLayers layers;

    /* Which lighting TERMS contribute, as opposed to which passes run. */
    RenderEffects effects;

    /* The ribbon's live numbers. Width, lift and colour are baked into
     * vertices, so a change here is a rebuild rather than a uniform. */
    RibbonTuning ribbon;

    /* F cycles the diagnostic views: 0 off, 1 geometry, 2 probes, 3 rooms,
     * 4 roughness, 5 occlusion. See PbrShader::setDebugView. */
    int debugView = 0;

    static constexpr int kDebugViewCount = 6;

    bool softCutaway  = true;
    bool showCover    = true;

    /* B toggles the baked sun against the shadow map, live. Defaults OFF: the
     * bake is the newer, less proven path, and a renderer should not ship its
     * experiment as the thing you get by default. */
    bool useBakedSun = false;

    /* Views that REPLACE surface shading, as opposed to adding something to an
     * ordinary frame. Geometry and rooms paint every fragment a flat colour,
     * so glass is drawn solid in the opaque pass and the blended pass is
     * skipped. The probe view is not one of these — its chrome balls need a
     * real scene to sit in — and writing `debugView != 0` instead is how it
     * would silently become one. */
    bool flatShading() const
    {
        return debugView == 1 || debugView == 3 ||
               debugView == 4 || debugView == 5;
    }
};

/* Who is signed in, for the dev panel. Strings rather than a SteamClient
 * reference: the renderer draws what it is told and cannot start a session. */
struct SteamStatus {
    bool        running = false;
    std::string reason;
    std::string persona;
    uint64_t    steamId = 0;
    std::string avatarState = "idle";
    std::string avatarUrl;
};

struct FrameView {
    /* Which screen the game is on. Outside InGame there is no world to
     * draw, and every pass below the UI is skipped. */
    UIState uiState = UIState::InGame;

    /* Seconds since the splash appeared. Carried rather than read from
     * GetTime() inside the renderer because the splash's effects RAMP from
     * zero and the ramp has to start when the image does — Application owns
     * that clock, and it is the same one that decides when the splash ends. */
    float splashSeconds = 0.0f;

    /* How far the splash is through, 0..1, for the loading bar drawn over it.
     * Carried for the same reason the clock above is: Application owns both
     * halves of the condition that ends the splash, so it is the only thing
     * that can say. See Application::splashProgress. */
    float splashProgress = 0.0f;

    const GameState* state = nullptr;

    Camera3D camera{};

    /* HOW MUCH OF THE WORLD THE CAMERA IS SHOWING — the storey cut and the
     * wall facings the camera angle removes, already decided.
     *
     * This is the CAMERA'S view of the world, and only the passes that draw
     * for the camera may use it. The sun's depth pass and the probe capture
     * take CutawayView::whole() instead, because what casts a shadow is a
     * question about the world and not about where the player is standing.
     * Getting that wrong is what made the lighting change when the iso level
     * did; see CutawayView.hpp for the full account. */
    CutawayView cutaway;

    /* ---- what the player is pointing at ------------------------------- */
    std::optional<int>            hovered;
    const std::vector<PathPoint>* preview  = nullptr;
    const MoveAnimator*           animator = nullptr;
    const RingSelector*           rings    = nullptr;

    /* Precomputed rather than passing the controller in: whether the selected
     * unit could legally end a move on the hovered cell. The overlays and the
     * HUD both want it, and it is the only thing either needed the controller
     * for. */
    bool hoverRestOk = false;

    bool grenadeArmed    = false;
    bool cursorOnSurface = false;

    /* The decal tool's ghost. A real Decal, submitted by the real pass after
     * the committed ones so it reads on top. */
    std::optional<Decal> decalGhost;

    /* The ribbon pass's per-frame settings, derived from the ring selection
     * and the cutaway — both of which are the controller's. */
    RibbonPassSettings ribbon;

    ViewSettings settings;

    SteamStatus steam;

    /* For the HUD. */
    std::string status;
    std::string cameraArgs;
};

}  // namespace game
