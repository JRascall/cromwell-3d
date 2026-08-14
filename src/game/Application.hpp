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

#include "cromwell/camera/CameraDirector.hpp"
#include "cromwell/entities/FixedTimestep.hpp"
#include "cromwell/input/FrameInput.hpp"
#include "cromwell/input/InputHandler.hpp"
#include "cromwell/platform/IPlatform.hpp"
#include "cromwell/platform/ISurface.hpp"
#include "cromwell/input/IInput.hpp"
#include "cromwell/platform/IClock.hpp"
#include "cromwell/input/PointerFocus.hpp"
#include "cromwell/steam/SteamAvatar.hpp"
#include "cromwell/steam/SteamClient.hpp"
#include "game/controllers/PlayerController.hpp"
#include "game/entities/pawns/CameraPawn.hpp"
#include "game/ui/state/UIStateMachine.hpp"
#include "game/cli/CliOptions.hpp"
#include "game/render/FrameRenderer.hpp"
#include "game/render/rhi/RhiFrameRenderer.hpp"
#include "game/render/FrameView.hpp"
#include "game/state/GameState.hpp"

/* For the clamp in splashProgress. Named rather than relied on arriving through
 * one of the headers above, which is how an include that everything depended on
 * gets removed from somewhere unrelated and breaks this file. */
#include <algorithm>

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
     * produced, and blanks whatever a UI surface is currently swallowing — a
     * drag on a slider must not also orbit the camera, and a click on a HUD
     * button must not also order a move. Which surfaces those are is decided by
     * focus_ rather than by a list of ifs here; see PointerFocus. */
    FrameInput arbitrate(FrameInput input);

    /* This frame, as the renderer needs to see it. The only place the
     * controller's state and the view toggles are read together.
     *
     * NOT const, because the view it builds hands out a non-const pointer to
     * view_ — the dev panel edits those settings in place and they have to land
     * on the one that lasts. See FrameView::settings. */
    FrameView buildFrameView();

    CliOptions options_;
    GameState  state_;

    /* THE SIMULATION CLOCK, and the rate is this game's decision rather than
     * the engine's — see FixedTimestep.hpp on why an engine that serves an RTS,
     * a shooter and this cannot pick one number. Sixty is chosen here because
     * the simulation is turn-based and event-driven, so the rate buys nothing
     * above the frame rate and nothing is gained by going lower; a lockstep RTS
     * on this engine would sensibly run a quarter of it.
     *
     * Speed control, if this game ever wants one, scales what is handed to
     * advance() — pause passes zero — and never touches the rate. */
    cromwell::FixedTimestep timestep_ = cromwell::FixedTimestep{}.withRate(60);

    /* Declared before the controller: the controller borrows the renderer's
     * DecalSet, and members are constructed in declaration order. */
    FrameRenderer    renderer_;
    PlayerController controller_;

    /* The pawn the controller possesses. It polls the controller for intent
     * and moves itself; nothing here drives it. */
    CameraPawn pawn_;

    /* WHICH CAMERA THE SCREEN SHOWS. Defaults to the pawn's; F5 cuts to the
     * plan camera and back as the worked example, and anything holding a
     * Camera can be cut to. Deliberately not the controller's — a controller
     * interprets input, and a kill-cam or a scripted cut is not input. See
     * cromwell/camera/CameraDirector.hpp for the argument. */
    CameraDirector director_;

    /* THE SECOND PLAYER, AS A CAMERA — a rig with no controller, standing in
     * until a real one exists. It orbits the board slowly so its splitscreen
     * pane is visibly LIVE rather than a photograph; real multiplayer
     * replaces the spin with a possessed pawn per player and changes nothing
     * else, because the pane just mirrors whatever this camera does. See
     * FrameRenderer::setPaneSource. */
    OrbitCamera playerTwoRig_;

    /* Which screen the game is on. The renderer reads it to decide whether
     * there is a world to draw at all. */
    UIStateMachine ui_;

    /* WHO OWNS THE CURSOR AND THE KEYBOARD. Rebuilt every frame from whatever
     * is on screen — the dev panel, the widget kit, the embedded browser — and
     * asked before any input is treated as the world's. It lives here rather
     * than in the renderer because it is the frame loop's arbitration, and this
     * class is the one whose entire job is order. */
    PointerFocus focus_;

    /* A MINIMUM, NOT A DURATION, and the distinction is the whole point of it.
     *
     * The splash exists to cover loading, and loading time is the one thing
     * nobody can predict: it is a cold disk on one machine and a warm cache on
     * the next, and it will grow as there is more to load. Timing the splash to
     * the work makes its length a symptom of the hardware — a brand that
     * flashes past on a fast machine and outstays its welcome on a slow one.
     *
     * So the splash holds for AT LEAST this long and longer if the work is not
     * finished. Fast machines see the floor; slow ones see the work; nobody
     * sees a flicker. The cost is up to six seconds of deliberate waiting on a
     * machine that had nothing to wait for, which is what every studio that
     * ships a logo screen has decided is worth paying.
     *
     * splashLoadComplete() is the other half of the condition and is where
     * asynchronous loading reports in when there is any. */
    static constexpr float kSplashMinimumSeconds = 6.0f;
    float splashElapsed_ = 0.0f;

    /* Whether everything the splash is covering has finished.
     *
     * Nothing loads asynchronously yet: the world is built, the shaders
     * compiled and the sun baked before the window is even revealed, all of it
     * on this thread, so by the time a splash frame is drawn there is nothing
     * outstanding and this is true from the first frame. The minimum above is
     * therefore the only thing holding the splash today.
     *
     * WHEN THERE IS ASYNCHRONOUS LOADING, THIS IS WHERE IT REPORTS IN — return
     * false while it runs and the splash will wait for it, with no other change
     * anywhere. Written as a function rather than a flag so that the caller
     * reads as the sentence it is meant to be. */
    bool splashLoadComplete() const { return true; }

    /* How far the splash is through, 0..1, for the loading bar drawn over it.
     *
     * THE SAME TWO CONDITIONS AS THE EXIT, and it has to be, or the bar
     * disagrees with the screen it is drawn on: full while the splash sits
     * there, or still climbing when it cuts away. So it is the minimum of the
     * two — the time floor, and the work.
     *
     * Today the work half is instantly complete, so this is the timer and the
     * bar fills smoothly over six seconds. When asynchronous loading arrives,
     * splashLoadComplete() gains a fraction to report and this is the second of
     * the two lines that change.
     *
     * UNDER --splash IT SITS AT FULL, which is honest: the exit is suppressed,
     * not pending. A bar that looped to look busy would be lying about a screen
     * that is deliberately parked. */
    float splashProgress() const
    {
        const float byTime = kSplashMinimumSeconds > 0.0f
            ? splashElapsed_ / kSplashMinimumSeconds
            : 1.0f;
        return std::clamp(std::min(byTime, splashLoadComplete() ? 1.0f : 0.99f), 0.0f, 1.0f);
    }

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

    /* ---- the machine, behind one door ----------------------------------
     *
     * THE WINDOW, THE CONTEXT, INPUT, TIMING, STORAGE AND THE RENDER DEVICE,
     * all of it. Created in run() rather than at construction because it opens
     * a window, and a constructed-but-not-run Application should not.
     *
     * NOTHING IN THIS CLASS NAMES A BACKEND ANY MORE. Which platform answers is
     * a CMakeLists decision — see the two-axis note there — so a console port
     * changes no line of this file. `platform_->surface()` is a console's
     * display on one target and a resizable window on another, and the loop
     * below cannot tell and does not ask.
     *
     * Held by pointer because IPlatform is an interface and the concrete type
     * is deliberately not visible here. */
    std::unique_ptr<IPlatform> platform_;

    /* THE RENDERER BEING BUILT, when --renderer rhi selected it. Null on the
     * raylib path, which is the default and stays so until parity. Created in
     * run() because it borrows the platform. */
    std::unique_ptr<RhiFrameRenderer> rhiRenderer_;

    /* SHUT THE MACHINE DOWN, IN THE ONE ORDER THAT WORKS.
     *
     * The device renderer holds meshes, buffers and textures that only the
     * device can free, and the device dies with the platform — so it has to be
     * released first, every time, on every path out of run(). There are five of
     * those: the normal exit and four early returns for a failed asset load and
     * the three self-test modes.
     *
     * A FUNCTION AND NOT FIVE COPIES, because the first version was five copies
     * of `platform_.reset()` and the renderer was added to none of them. The
     * result was an access violation in a destructor after main had already
     * printed its exit status — which is invisible when the game is launched
     * from Explorer, and reads as "it crashes on quit sometimes" when it is
     * not. Ordering rules that live in a comment get followed at four sites out
     * of five; ordering rules that live in a function get followed. */
    void releasePlatform();

    /* Reads better than platform_->surface() at the dozen sites that want one,
     * and keeps the null check in one place: past run()'s early return these
     * are never null. */
    ISurface& surface() { return platform_->surface(); }
    IInput&   deviceInput() { return platform_->input(); }
    IClock&   clock() { return platform_->clock(); }

    /* What the player and the dev panel have switched on. Written by
     * applyInput, and written directly by the dev panel — the renderer is
     * handed a pointer to this rather than a copy of it, precisely so a
     * checkbox in the panel and a keypress in the game edit the same bytes. */
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
