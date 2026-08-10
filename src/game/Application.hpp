/* Application.hpp — the frame loop and the wiring.
 *
 * SINGLE RESPONSIBILITY: own the pieces and sequence them. Every decision it
 * makes is about ORDER — sample input, arbitrate it against the dev panel,
 * step the controller, hand what changed to the renderer, draw.
 *
 * IT USED TO BE 1615 LINES, because "own the pieces and sequence them" had
 * quietly grown to include being the renderer and being the input handler. The
 * two jobs that were not sequencing now live next door:
 *
 *   PlayerController   input -> intent -> the world. Owns the camera rig, the
 *                      selection, the hovered cell, the path preview and the
 *                      decal tool. Touches no GPU resource.
 *   FrameRenderer      every render target, shader and pass, and the order
 *                      they run in. Reads a FrameView; knows nothing about
 *                      what a mouse is.
 *
 * The two never speak. Where an action invalidates something the renderer
 * built — a grenade changes the geometry, the lighting AND the reach field —
 * the controller records it in an Outcome and applyOutcome() is the one place
 * that acts on it.
 */
#pragma once

#include "cromwell/input/FrameInput.hpp"
#include "cromwell/input/InputHandler.hpp"
#include "cromwell/steam/SteamAvatar.hpp"
#include "cromwell/steam/SteamClient.hpp"
#include "game/controllers/PlayerController.hpp"
#include "game/entities/pawns/CameraPawn.hpp"
#include "game/ui/state/UIStateMachine.hpp"
#include "game/cli/CliOptions.hpp"
#include "game/render/FrameRenderer.hpp"
#include "game/render/FrameView.hpp"
#include "game/state/GameState.hpp"

#if XC_HAVE_WEB
#include "cromwell/web/cef/WebRuntime.hpp"
#include "cromwell/web/surface/WebSelfTest.hpp"
#include "cromwell/web/surface/WebSurface.hpp"
#endif

#include <memory>

namespace game {

using namespace cromwell;

class Application {
public:
    explicit Application(CliOptions options);

    /* Opens the window, runs the loop, returns the process exit code. */
    int run();

private:
    void rebuildDerivedState();      /* reach + ribbons, after any data edit */

    /* Everything the controller invalidated, handed to the pieces that own it.
     * The controller records; this is the only place that acts. */
    void applyOutcome(const PlayerController::Outcome& outcome);

    /* The next free profiles/profile_NNN.json beside the executable. Numbered
     * rather than fixed, so an F9 capture cannot destroy the previous one. */
    std::string nextCapturePath() const;

    void applyInput(const FrameInput& input);

    /* Re-derives the storey cut from the selection while the cutaway is in its
     * dynamic mode. A no-op in manual. */
    void updateCutawayStorey();

    /* Folds the debug panel's clicks into the same FrameInput the keyboard
     * produced, and blanks whatever the panel is currently swallowing — a drag
     * on a slider must not also orbit the camera. */
    FrameInput arbitrate(FrameInput input);

    /* This frame, as the renderer needs to see it. The only place the
     * controller's state and the view toggles are read together. */
    FrameView buildFrameView() const;

    CliOptions options_;
    GameState  state_;

    /* Declared before the controller: the controller borrows the renderer's
     * DecalSet, and members are constructed in declaration order. */
    FrameRenderer    renderer_;
    PlayerController controller_;

    /* The pawn the controller possesses. It polls the controller for intent
     * and moves itself; nothing here drives it. */
    CameraPawn pawn_;

    /* Which screen the game is on. The renderer reads it to decide whether
     * there is a world to draw at all. */
    UIStateMachine ui_;

    /* How long the splash holds, and how far through it we are. */
    static constexpr float kSplashSeconds = 2.0f;
    float splashElapsed_ = 0.0f;

    /* WHERE THE SPLASH GOES NEXT, and it is deliberately not MainMenu yet.
     * The board is what is being worked on, and a menu between every build and
     * the thing under test is a click nobody wants to make fifty times a day.
     *
     * The menu and options screens are still built, still reachable, and still
     * tested - see tests/UIStateTests.cpp - so restoring the real flow is this
     * one word. */
    static constexpr UIState kAfterSplash = UIState::InGame;

    /* The window starts hidden and is revealed after the first presented
     * frame. See run(). */
    bool windowShown_ = false;

    InputHandler input_;

    /* What the player and the dev panel have switched on. Written by
     * applyInput, read by the renderer through buildFrameView. */
    ViewSettings view_;

    /* The Steam session. UNGUARDED BY ANY #if, unlike the web runtime above:
     * SteamClient compiles with or without the SDK and reports which, so this
     * header describes one class layout on every machine rather than two. See
     * cromwell/steam/SteamClient.hpp. */
    SteamClient steam_;

    /* The signed-in player's avatar, fetched over HTTPS from the community
     * site on a worker. Started once Steam reports a session; polled each
     * frame, and handed to the renderer to decode on the frame it lands. */
    SteamAvatar steamAvatar_;

#if XC_HAVE_WEB
    /* CEF's lifetime is the process's, so the runtime outlives the surface —
     * members are destroyed in reverse, and a browser torn down after
     * CefShutdown is an access violation. The surface itself lives on the
     * renderer, which is what draws it. */
    std::unique_ptr<WebRuntime> web_;
#endif
};

}  // namespace game
