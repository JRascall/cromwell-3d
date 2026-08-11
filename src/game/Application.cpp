#include "game/Application.hpp"

#include "game/controllers/PlayerController.hpp"
#include "game/entities/pawns/CameraPawn.hpp"
#include "game/events/GameEvents.hpp"
#include "game/ui/state/UIStateMachine.hpp"
#include "cromwell/diag/Profile.hpp"
#include "cromwell/gpu/GpuProfiler.hpp"
#include "cromwell/services/Services.hpp"
#include "cromwell/settings/SettingKeys.hpp"
#include "cromwell/settings/Settings.hpp"

#include "raymath.h"
#include "rlgl.h"

#include "game/border/band/Band.hpp"
#include "game/border/band/BandExtractor.hpp"
#include "game/border/loop/LoopSet.hpp"
#include "game/light/RoomPartition.hpp"
#include "game/movement/search/PathReconstructor.hpp"
#include "game/render/dev/DecalDemo.hpp"
#include "cromwell/gpu/compute/ComputeSelfTest.hpp"
#include "cromwell/gpu/ShaderLibrary.hpp"
#include "game/render/Palette.hpp"
#include "cromwell/ribbon/RibbonConstants.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */
namespace {

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 800;

/* Custom depth stencil values. 0 stays reserved for "nothing was drawn", and
 * units are numbered from 1 upward so a consumer can outline one soldier, or
 * test a range for the whole squad. */
constexpr int kPropStencil      = 200;
constexpr int kFirstUnitStencil = 1;

std::string format(const char* text) { return text ? text : ""; }

}  // namespace

Application::Application(CliOptions options)
    : options_(std::move(options)), controller_(state_, renderer_.decals())
{
    state_.setMoveBudget(options_.moveBudget);
    state_.setIsoLevel(options_.isoLevel);
    state_.setLosMode(options_.losMode);
    state_.selectIndex(options_.selectedUnit);
    view_.debugView = options_.debugView % ViewSettings::ViewSettings::kDebugViewCount;
    if (options_.forceBothRings) controller_.rings().forceBothRings();

    /* The controller drives this pawn, and needs it back for the picking
     * ray. Set once - nothing re-possesses. */
    controller_.possess(pawn_);

    /* THE SETTINGS BAG, BEFORE ANY ENTITY EXISTS. cromwell reads its own
     * defaults out of this (see SettingKeys.hpp) and a component captures them
     * when it is constructed, so registering it after the roster was built
     * would leave those entities on the engine's fallbacks.
     *
     * Nothing here overrides the engine's numbers yet; the bag is registered
     * so the path is live and a value can be changed in one line rather than
     * by first inventing somewhere to put it. */
    Services::provide<Settings>(kGameSettings)
        .setFloat(settings::kThinkInterval, 0.1f);
}

/* ------------------------------------------------------------ lifecycle */
FrameView Application::buildFrameView() const
{
    FrameView view;
    view.state           = &state_;
    view.camera          = pawn_.camera();
    view.uiState         = ui_.state();
    view.splashSeconds   = splashElapsed_;
    view.splashProgress  = splashProgress();
    view.hovered         = controller_.hovered();
    view.preview         = &controller_.preview();
    view.animator        = &controller_.animator();
    view.rings           = &controller_.rings();
    view.hoverRestOk     = controller_.hovered() && controller_.canRestAt(*controller_.hovered());
    view.grenadeArmed    = controller_.grenadeArmed();
    view.cursorOnSurface = controller_.cursorOnSurface();
    view.decalGhost      = controller_.decalPreview();
    view.ribbon          = controller_.ribbonSettings(view_.softCutaway);
    view.cutaway         = controller_.cutawayView();
    view.settings        = view_;

    view.steam.running     = steam_.running();
    view.steam.reason      = steam_.reason();
    view.steam.persona     = steam_.personaName();
    view.steam.steamId     = steam_.steamId();
    view.steam.avatarUrl   = steamAvatar_.url();
    switch (steamAvatar_.state()) {
        case SteamAvatar::State::Fetching: view.steam.avatarState = "fetching..."; break;
        case SteamAvatar::State::Ready:    view.steam.avatarState = "ready"; break;
        case SteamAvatar::State::Failed:   view.steam.avatarState = steamAvatar_.error(); break;
        default:                           view.steam.avatarState = "idle"; break;
    }
    view.status          = controller_.status();
    view.cameraArgs      = controller_.cameraArguments();
    return view;
}

/* WHERE A CAPTURE LANDS, and it is not the working directory. This app is
 * launched from the project root, from builds/win and from Explorer, so a bare
 * "profile.json" goes to three different places depending on how you started
 * it — the same reason the log is opened beside the executable rather than
 * where the shell happened to be.
 *
 * NUMBERED, NOT OVERWRITTEN. A fixed name means the capture of the stutter you
 * just reproduced is destroyed by the next press of F9, which is invariably how
 * you find out. Probing for the first free number needs no clock and stays
 * sorted in a file listing. */
std::string Application::nextCapturePath() const
{
    namespace fs = std::filesystem;

    fs::path directory = fs::path(GetApplicationDirectory()) / "profiles";

    std::error_code error;
    fs::create_directories(directory, error);
    /* If the directory cannot be made — a read-only install — fall back beside
     * the executable rather than failing the capture outright. */
    if (error) directory = fs::path(GetApplicationDirectory());

    for (int index = 1; index < 1000; index++) {
        char name[32];
        std::snprintf(name, sizeof(name), "profile_%03d.json", index);

        const fs::path candidate = directory / name;
        if (!fs::exists(candidate, error)) return candidate.string();
    }

    /* A thousand captures in one directory is a housekeeping problem, not a
     * reason to refuse. Reuse the last slot. */
    return (directory / "profile_999.json").string();
}

void Application::rebuildDerivedState()
{
    controller_.rebuildDerivedState();
    applyOutcome(controller_.takeOutcome());
}

/* Everything the controller invalidated, handed to the pieces that own it.
 * The controller records; this is the only place that acts. */
void Application::applyOutcome(const PlayerController::Outcome& outcome)
{
    if (outcome.worldGeometryChanged) {
        renderer_.rebuildStatics(state_);
        renderer_.markProbesDirty();   /* the world the probes captured is gone */
    }

    if (outcome.rebakeCentre) {
        renderer_.addBlastFlash(
            static_cast<float>(outcome.rebakeCentre->x) + 0.5f,
            Lattice::cellBaseHeight(outcome.rebakeCentre->z) + 0.6f,
            static_cast<float>(outcome.rebakeCentre->y) + 0.5f);
        renderer_.rebakeAfterChange(state_, *outcome.rebakeCentre,
                                    outcome.rebakeRadius);
    }

    if (outcome.derivedStateChanged) renderer_.rebuildRibbons(state_, view_.ribbon);
}

/* THE DYNAMIC CUTAWAY'S ONE RULE: show the storey the selected unit is on.
 *
 * Derived every frame rather than pushed on selection, because the storey a
 * unit is ON changes without anything selecting it — walking up a ramp, being
 * animated along a path, falling through a floor that was just blown out. A
 * push would need every one of those to remember to update the cut; reading it
 * where it is used cannot go stale.
 *
 * It writes isoLevel_ rather than living beside it, so that every existing
 * reader — picking, the visibility overlay, the ribbon pass, the renderer —
 * keeps working unchanged and none of them has to learn that a mode exists. */
void Application::updateCutawayStorey()
{
    if (state_.cutawayMode() != CutawayMode::Dynamic) return;
    state_.setIsoLevel(Lattice::storeyOfZ(state_.selectedUnit().position().z));
}

/* ---------------------------------------------------------------- input */
void Application::applyInput(const FrameInput& input)
{
    /* A STOREY KEY IS A STATEMENT THAT THE AUTOMATIC ANSWER IS NOT WANTED, so
     * it latches manual rather than being overwritten a frame later by the
     * unit the player happens to have selected. 0 hands control back. */
    if (input.setStoreyGround) { state_.setIsoLevel(0); state_.setCutawayMode(CutawayMode::Manual); }
    if (input.setStoreyMiddle) { state_.setIsoLevel(1); state_.setCutawayMode(CutawayMode::Manual); }
    if (input.setStoreyTop) {
        state_.setIsoLevel(state_.world().lattice().storeys() - 1);
        state_.setCutawayMode(CutawayMode::Manual);
    }
    if (input.setStoreyDynamic) state_.setCutawayMode(CutawayMode::Dynamic);

    if (input.cycleRing)     controller_.rings().cycleOverride();
    if (input.toggleCutaway) view_.softCutaway = !view_.softCutaway;
    if (input.toggleCover)   view_.showCover = !view_.showCover;
    if (input.toggleGrenade) controller_.toggleGrenade();
    if (input.toggleOcclusion) renderer_.ao().setEnabled(!renderer_.ao().enabled());

    /* F9. Starting is silent; stopping writes the trace and says where, in the
     * status line and in the log — a capture whose path you have to guess is a
     * capture nobody opens. */
    if (input.toggleCapture) {
        Profiler& profiler = Profiler::instance();
        const bool wasCapturing = profiler.capturing();
        profiler.toggleCapture();

        if (wasCapturing) {
            const int  frames = profiler.capturedFrames();
            const std::string path = profiler.writeCapture(nextCapturePath());

            if (path.empty()) {
                controller_.setStatus("capture failed - could not write the trace");
                TraceLog(LOG_WARNING, "PROFILE: capture could not be written");
            } else {
                const std::string message =
                    "captured " + std::to_string(frames) + " frames -> " + path;
                controller_.setStatus(message + "  (open at ui.perfetto.dev)");
                TraceLog(LOG_INFO, "PROFILE: %s", message.c_str());
            }
        } else {
            controller_.setStatus("capturing - F9 again to stop and write");
        }
    }
    if (input.toggleBake) view_.useBakedSun = !view_.useBakedSun;
    if (input.toggleFlatView) view_.debugView = (view_.debugView + 1) % ViewSettings::kDebugViewCount;

    if (input.copyCamera) {
        /* The DEBUG VIEW rides along, because reproducing a view means both:
         * an artefact seen in the occlusion pass is not visible in the lit
         * frame, and a camera without the view that showed it is half the
         * information. Paste the whole line after the executable. */
        const std::string arguments =
            controller_.cameraArguments() +
            (view_.debugView != 0 ? " --debug-view " + std::to_string(view_.debugView) : "");

        SetClipboardText(arguments.c_str());
        controller_.setStatus("camera copied: " + arguments);

        /* In the log as well as on the clipboard: a clipboard survives exactly
         * until the next copy, and a viewpoint worth reporting is usually worth
         * still having after the session that found it. */
        TraceLog(LOG_INFO, "CAMERA: %s", arguments.c_str());
    }

    if (input.toggleLos) {
        state_.setLosMode(!state_.losMode());
        rebuildDerivedState();
    }

    /* 45 degrees a second: fast enough to sweep the whole arc while watching,
     * slow enough to stop on a look. */
    constexpr float kSunDegreesPerSecond = 45.0f;
    if (input.sunAzimuthRate != 0.0f)
        renderer_.sun().nudgeAzimuth(input.sunAzimuthRate * kSunDegreesPerSecond * input.deltaSeconds);
    if (input.sunElevationRate != 0.0f)
        renderer_.sun().nudgeElevation(input.sunElevationRate * kSunDegreesPerSecond * input.deltaSeconds);

    if (input.resetWorld) {
        state_.reset();
        state_.setMoveBudget(options_.moveBudget);
        renderer_.clearFlashes();
        controller_.setStatus("");
        renderer_.rebuildStatics(state_);
    renderer_.markProbesDirty();  /* the world the probes captured no longer exists */
        rebuildDerivedState();
    }
}

/* THE PANEL AND THE KEYBOARD ARE THE SAME INPUT DEVICE.
 *
 * Every debug control produces the request a key would have produced and is
 * applied by applyInput, so there is exactly one implementation of what
 * "toggle cover" means. The two absolute settings — a storey slider, a sun
 * slider — have no keyboard equivalent to borrow, so they land here.
 *
 * The clearing is the other half: while ImGui owns the cursor or the keyboard,
 * the game must not also see those events, or dragging the azimuth slider
 * orbits the camera underneath it. */
FrameInput Application::arbitrate(FrameInput input)
{
    const DevRequests requests = renderer_.takeDevRequests();

    if (renderer_.devWantsKeyboard()) {
        input.setStoreyGround = input.setStoreyMiddle = input.setStoreyTop = false;
        input.setStoreyDynamic = false;
        input.cycleRing = input.toggleCutaway = input.toggleLos = false;
        input.toggleCover = input.toggleGrenade = input.toggleOcclusion = false;
        input.toggleBake = input.toggleFlatView = input.resetWorld = false;
        input.panForward = input.panRight = 0.0f;
        input.sunAzimuthRate = input.sunElevationRate = 0.0f;
    }

    if (renderer_.devWantsMouse()) {
        input.orbiting     = false;
        input.wheel        = 0.0f;
        input.leftPressed  = false;
        input.leftReleased = false;
    }

#if XC_HAVE_WEB
    /* THE POINTER NEEDS NOTHING HERE. The page lives inside an ImGui window,
     * so any click that reaches it has already made WantCaptureMouse true and
     * the block above has already taken the mouse away from the world.
     *
     * The keyboard does need it, because ImGui cannot see a caret that is
     * inside the page rather than in one of its own fields. Asked of the
     * surface directly rather than cached from the last frame, so it is this
     * frame's answer — and it is false the moment focus leaves the field,
     * which is what keeps an open browser from swallowing the game's hotkeys
     * for as long as it is on screen. */
    if (renderer_.webPanel() && renderer_.webPanel()->wantsKeyboard()) {
        input.setStoreyGround = input.setStoreyMiddle = input.setStoreyTop = false;
        input.setStoreyDynamic = false;
        input.cycleRing = input.toggleCutaway = input.toggleLos = false;
        input.toggleCover = input.toggleGrenade = input.toggleOcclusion = false;
        input.toggleBake = input.toggleFlatView = input.resetWorld = false;
        input.panForward = input.panRight = 0.0f;
        input.sunAzimuthRate = input.sunElevationRate = 0.0f;
    }
#endif

    input.toggleCutaway   |= requests.toggleCutaway;
    input.toggleCover     |= requests.toggleCover;
    input.toggleLos       |= requests.toggleLos;
    input.toggleGrenade   |= requests.toggleGrenade;
    input.toggleOcclusion |= requests.toggleOcclusion;
    input.toggleBake      |= requests.toggleBake;
    input.toggleFlatView  |= requests.toggleFlatView;
    input.cycleRing       |= requests.cycleRing;
    input.resetWorld      |= requests.resetWorld;

    if (requests.cyclePreviewProbe && renderer_.probes().probeCount() > 0)
        renderer_.probes().setPreviewProbe((renderer_.probes().previewProbe() + 1) % renderer_.probes().probeCount());

    /* Same reasoning as the storey keys: moving the slider is an explicit
     * choice, so it latches manual rather than being overwritten next frame. */
    if (requests.isoLevel) {
        state_.setIsoLevel(*requests.isoLevel);
        state_.setCutawayMode(CutawayMode::Manual);
    }
    if (requests.sunAzimuth)   renderer_.sun().setAzimuth(*requests.sunAzimuth);
    if (requests.sunElevation) renderer_.sun().setElevation(*requests.sunElevation);

    if (requests.rebakeSun) renderer_.rebakeAll(state_);

    if (requests.clearDecals) renderer_.decals().clear();

    /* PRESENT MEANS ARMED, and its absence means disarmed — which is what makes
     * closing the panel cancel the tool rather than leaving a ghost stuck to the
     * cursor with no way to reach the button that turns it off. */
    controller_.setDecalTool(requests.decalBrush.has_value(),
                             requests.decalBrush.value_or(DevRequests::DecalPlacement{}),
                             renderer_.decalPassAvailable());

    /* Width, lift and colour live in the vertices, so the panel moving one is
     * a rebuild rather than a uniform. Here rather than mid-draw, where the
     * slider was actually dragged. */
    renderer_.rebuildRibbonsIfStale(state_, view_.ribbon);

    return input;
}

int Application::run()
{
    /* WHERE THE GAME OPENS, decided here rather than at the ui_.setState below
     * because Steam wants the same answer and wants it before the window. A
     * scripted run - a screenshot, a self-test - has nobody to click past a
     * menu, and a splash screen captured instead of the board is a silently
     * useless artefact. So anything non-interactive goes straight to the board;
     * a human gets the front end. */
    const bool scripted = options_.screenshotPath.has_value() ||
                          options_.webSelfTestPath.has_value() ||
                          options_.computeSelfTestPath.has_value();

    /* BEFORE InitWindow, and it has to be: the overlay hooks itself into the
     * graphics context when the first frame is presented, and a context created
     * before Steam was initialised never gets hooked - the overlay then does
     * nothing at all, silently, which is a miserable thing to debug.
     *
     * Skipped for scripted runs. Not a purity argument: connecting launches the
     * Steam client if it is not up, and a friend request sliding into frame 3
     * of a --shot is a screenshot that has to be retaken. --no-steam is the
     * same switch for an interactive run. */
    if (options_.steam && !scripted) {
        steam_.start();

        /* Only once there is an id to fetch for. The community site is asked
         * rather than the SDK because the same call then works for any account
         * - a lobby list - and returns the same 184x184 image. */
        if (steam_.running()) steamAvatar_.start(steam_.steamId());
    }

    /* HIDDEN UNTIL THERE IS SOMETHING WORTH LOOKING AT. Between InitWindow and
     * the first frame this process compiles every shader, builds the static
     * mesh, bakes the sun and starts Chromium - the better part of two seconds
     * during which a visible window is an empty grey rectangle that reads as a
     * hang. The window is revealed below, AFTER the first frame has been drawn
     * and presented, so the first thing that appears on screen is the splash
     * rather than a blank frame that then becomes the splash. */
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIDDEN |
                   (options_.screenshotPath ? 0 : FLAG_WINDOW_RESIZABLE));
    InitWindow(kWindowWidth, kWindowHeight, "xcom-c - XCOM 2 tile lattice");
    SetTargetFPS(60);

    /* The GPU profiler's context, immediately after the GL one exists and on
     * the thread that owns it — both are requirements, and both fail quietly
     * rather than loudly. No-ops unless built with -DXC_TRACY=ON. */
    CW_GPU_CONTEXT();
    CW_PROFILE_THREAD("main");

    /* The only thing that currently PROVES Steam came up, and the reason the
     * persona name is fetched at all. Cheap to lose the day there is a real
     * front end to put it on. */
    if (steam_.running()) {
        SetWindowTitle(TextFormat("xcom-c - XCOM 2 tile lattice - Steam: %s",
                                  steam_.personaName().c_str()));
    }

    if (!renderer_.initialise(kWindowWidth, kWindowHeight, options_, state_)) {
        std::fprintf(stderr, "FATAL: could not load assets/shaders - "
                             "run from the project root\n");
        CloseWindow();
        return 1;
    }

#if XC_HAVE_WEB
    /* DEGRADES, NEVER FAILS. A missing libcef.dll or a helper that did not get
     * staged costs the browser panel and nothing else, and the renderer this
     * is bolted onto has to keep working on a machine that has never heard of
     * Chromium. Hence a warning rather than a `return 1`.
     *
     * Created HERE rather than in the renderer: CEF's lifetime is the
     * process's, and the runtime has to outlive the surface the renderer
     * draws. */
    web_ = std::make_unique<WebRuntime>();
    if (web_->start()) {
        /* A starting size only. The browser tab resizes this to whatever it is
         * actually given the moment it opens, because anything else would be
         * scaled to fit and text does not survive that. */
        renderer_.setWebPanel(std::make_unique<WebSurface>(*web_, 1024, 700,
                                                           "https://www.google.com"));
    } else {
        TraceLog(LOG_WARNING, "WEB: disabled - %s", web_->reason().c_str());
        web_.reset();
    }
#endif

    /* WHERE THE GAME OPENS. Three inputs, and they are not symmetrical:
     *
     *   - `scripted`, decided at the top of run() because Steam needs the same
     *     answer before the window exists. A screenshot or a self-test skips
     *     the front end, since a capture of a splash instead of the board is a
     *     silently useless artefact.
     *   - --splash overrides that, and only for this decision: such a run still
     *     skips Steam and still exits on its shot frame. It exists so the
     *     splash can be captured at all.
     *   - --no-splash skips it whatever else was asked, which is the developer
     *     switch for not watching six seconds of it every build. */
    const bool openOnSplash = !options_.skipSplash &&
                              (!scripted || options_.forceSplash);

    ui_.setState(openOnSplash ? UIState::SplashScreen : UIState::InGame);

    /* The panel stays closed until F1, or until --dev-view asks for it — which
     * is the only way a screenshot run can have it, having no F1 to press. */
    renderer_.setupDevView(state_.world().lattice().storeys());
    if (options_.forceDevView) renderer_.setDevViewVisible(true);

    /* Diagnostic mode: prove the compute path and leave. Before the web test
     * and before the camera, because it depends on nothing but a live GL
     * context and its answer is a precondition for anything built on compute.
     * Exit code follows the result, so a build script can gate on it. */
    if (options_.computeSelfTestPath) {
        const std::string report = runComputeSelfTest(*options_.computeSelfTestPath);
        std::fputs(report.c_str(), stderr);

        const bool passed = computeSelfTestPassed();

#if XC_HAVE_WEB
        renderer_.setWebPanel(nullptr);
        if (web_) web_->stop();
        web_.reset();
#endif
        renderer_.shutdownDevView();
        CloseWindow();
        return passed ? 0 : 1;
    }

#if XC_HAVE_WEB
    /* Diagnostic mode: script the browser, write down what happened, leave.
     * Before the camera and the derived state because none of that matters
     * here — the only thing this run exercises is the web surface. */
    if (options_.webSelfTestPath) {
        if (web_) {
            const std::string report =
                runWebSelfTest(*web_, *options_.webSelfTestPath,
                               options_.webSelfTestUrl ? *options_.webSelfTestUrl
                                                       : std::string(),
                               options_.webSelfTestType ? *options_.webSelfTestType
                                                        : std::string());
            std::fputs(report.c_str(), stderr);
        } else {
            std::fputs("web self test: CEF is not running\n", stderr);
        }
        renderer_.setWebPanel(nullptr);
        if (web_) web_->stop();
        web_.reset();
        renderer_.shutdownDevView();
        CloseWindow();
        return 0;
    }
#endif

    pawn_.rig().applyPreset(options_.cameraPreset, options_.freeCamera);
    rebuildDerivedState();
    if (options_.detonateAt) {
        controller_.detonateAt(*options_.detonateAt);
        applyOutcome(controller_.takeOutcome());
    }

    int frames = 0;
    while (!WindowShouldClose()) {
        /* THE WHOLE FRAME, and every system inside it as its own row. The
         * panel is meant to answer the same question Unreal's stat breakdown
         * does — which system is eating the frame — and it can only name the
         * systems that carry a zone. */
        CW_PROFILE_ZONE_N("frame");
#if XC_HAVE_WEB
        /* Chromium's slice of the frame, first: nothing in the browser
         * advances without it, and everything below wants this frame's page
         * rather than the last one's. OnPaint lands inside this call, on this
         * thread — see WebSurface.hpp on why that matters. */
        if (web_) {
            CW_PROFILE_ZONE_N("web");
            web_->tick();
        }
#endif

        /* Steam's slice. Nothing dispatches without it - the overlay stops
         * responding and nothing else visibly breaks, which is exactly how a
         * missing RunCallbacks survives review. */
        {
            CW_PROFILE_ZONE_N("steam");
            steam_.tick();
        }

        FrameInput input = input_.sample(options_.mouseX, options_.mouseY);
        if (input.toggleDevView) renderer_.toggleDevView();
        if (input.toggleUiGallery) renderer_.toggleUiGallery();

#if XC_HAVE_WEB
        /* Before the frame that will draw it. The pointer and keyboard are
         * routed later, from inside DevView's browser tab, because that is the
         * only place that knows where the page ended up on screen. */
        if (renderer_.webPanel()) renderer_.webPanel()->upload();
#endif

        input = arbitrate(input);

        /* The render targets track the window whatever screen we are on: a
         * menu resized and then entered would otherwise land in the game with
         * buffers the wrong size. */
        if (input.windowResized) renderer_.resizeForWindow();

        if (ui_.state() == UIState::InGame) {
            CW_PROFILE_ZONE_N("simulation");

            applyInput(input);
            controller_.sampleCameraIntent(input);

            /* The pawn polls what the controller just parsed and moves
             * itself. */
            pawn_.tick(input.deltaSeconds, controller_, state_.world());

            /* The entity update cycle: every unit ticks, and its components
             * tick or think from there. */
            {
                CW_PROFILE_ZONE_N("entity tick");
                state_.roster().tick(input.deltaSeconds);
            }

            {
                CW_PROFILE_ZONE_N("effects");
                renderer_.updateEffects(input.deltaSeconds);
            }

            /* Pointer picking casts a ray into the world every frame the
             * cursor moves, so it is a genuine per-frame cost rather than
             * bookkeeping — and the first thing to suspect when the frame
             * time moves with the mouse. */
            if (controller_.animator().isRunning()) {
                CW_PROFILE_ZONE_N("move animation");
                controller_.stepAnimation(input.deltaSeconds);
            } else if (!input.orbiting && !renderer_.devWantsMouse()) {
                CW_PROFILE_ZONE_N("pointer pick");
                controller_.updatePointer(input);
            }

            /* THE DRAIN, and it has to be here rather than only at the call
             * sites that ask for a rebuild.
             *
             * Everything above records into the Outcome instead of touching the
             * renderer — a click that selects a unit, a click that throws a
             * grenade, the frame a move animation lands on. None of those go
             * through Application::rebuildDerivedState(), so without this the
             * flags sat in the controller until the next LOS toggle or world
             * reset happened to take them. The reach field was correct all
             * along; the ribbon and border MESHES were built from the unit's
             * old cell, which is exactly what a stale walkable area looks like.
             *
             * Cheap when nothing happened — three bool tests. */
            applyOutcome(controller_.takeOutcome());

            /* Last, so it sees this frame's selection and this frame's step of
             * a move animation rather than the previous one's. */
            updateCutawayStorey();
        } else {
            /* F5 re-reads the splash shader. Handled here rather than with the
             * in-game keys because this branch is the only place the front end
             * runs, and the splash is the only pass listening. */
            if (input.reloadShaders) renderer_.reloadShaders();

            /* The splash is timed rather than clicked through, so it needs the
             * clock even though it takes no input. The clock keeps running
             * under --splash too: the shader animates off it, so stopping it
             * would freeze the thing that flag exists to show. */
            splashElapsed_ += input.deltaSeconds;

            /* BOTH CONDITIONS, and in that order: the splash leaves once it has
             * been up for its minimum AND the work behind it is done. Either
             * one alone is a worse screen — a pure timer cuts away from
             * loading that has not finished, and a pure "when ready" flashes
             * past on a fast machine.
             *
             * --splash HOLDS IT REGARDLESS. Six seconds is the right length for
             * a splash and the wrong length for looking at one, so the flag
             * suppresses the exit entirely and it stays up until the window is
             * closed, which is what makes it usable for tuning the shader. */
            if (ui_.state() == UIState::SplashScreen && !options_.forceSplash &&
                splashElapsed_ >= kSplashMinimumSeconds && splashLoadComplete())
                ui_.setState(kAfterSplash);
        }

        /* The avatar arrives on a worker; this is the frame it lands on, and
         * the only place it may touch the GPU. */
        if (steamAvatar_.poll() && steamAvatar_.isReady())
            renderer_.uploadSteamAvatar(steamAvatar_.bytes());

        renderer_.render(buildFrameView());

        /* AFTER the swap that render() ends with, which is where the GPU
         * timestamp queries issued this frame become readable. The results
         * belong to a frame or two ago — that latency is inherent to asking
         * the device what it did, not a bug in the reporting.
         *
         * CW_PROFILE_FRAME closes the frame while the "frame" zone above is
         * still in scope; Profiler::endFrame charges open zones up to this
         * point rather than dropping them, which is what makes the whole-frame
         * row exist at all. See the note there. */
        CW_GPU_COLLECT();
        CW_PROFILE_FRAME();

        /* The reveal, once and only once. AFTER render(), because that call
         * ends with EndDrawing() and therefore with a presented frame: reveal
         * before it and the first thing on screen is an uninitialised back
         * buffer that then becomes the splash.
         *
         * A scripted run is never revealed at all. It has nobody watching, and
         * a window that flashes up for two frames in the middle of a build is
         * noise; TakeScreenshot reads the framebuffer, which exists whether or
         * not the window is mapped - verified byte-identical either way. */
        if (!windowShown_ && !scripted) {
            ClearWindowState(FLAG_WINDOW_HIDDEN);
            windowShown_ = true;
        }

        /* What a menu button asked for, applied after the frame that drew it -
         * changing state mid-draw would leave the rest of the frame drawing a
         * screen that is no longer current. */
        const FrameRenderer::UIRequest request = renderer_.takeUIRequest();
        if (request.quit) break;
        if (request.setAmbientOcclusion) renderer_.ao().setEnabled(*request.setAmbientOcclusion);
        if (request.setUseBakedSun)      view_.useBakedSun = *request.setUseBakedSun;
        if (request.setSoftCutaway)      view_.softCutaway = *request.setSoftCutaway;
        if (request.goTo)                ui_.setState(*request.goTo);

        if (options_.screenshotPath && ++frames >= options_.screenshotFrame) {
            TakeScreenshot(options_.screenshotPath->c_str());
            break;
        }
    }

#if XC_HAVE_WEB
    /* BEFORE CloseWindow, in this order. The surface owns a GL texture, so it
     * has to go while the context is still alive; CefShutdown has to come
     * after the browser it would otherwise be waiting on, and WebRuntime::stop
     * pumps the loop enough times to turn "asked to close" into "closed". */
    renderer_.setWebPanel(nullptr);
    if (web_) web_->stop();
    web_.reset();
#endif

    renderer_.shutdownDevView();
    CloseWindow();

    /* AFTER the window, unlike CEF above. The overlay is injected into the
     * graphics context, so tearing Steam down first would pull it out from
     * under a context that is still alive. The destructor would do this anyway;
     * it is written out because the ORDER is the point. */
    steam_.stop();
    return 0;
}

}  // namespace game

