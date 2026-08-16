/* CliOptions.hpp — the command line.
 *
 * SINGLE RESPONSIBILITY: parse argv into a struct. Nothing acts on it here.
 *
 *   --shot <file.png>       render one frame, screenshot and exit
 *                           (raylib resolves that path against the cwd)
 *   --shot-frame <n>        capture on frame n instead of 3 — the FPS readout
 *                           is meaningless on frame 3, so a perf check wants
 *                           a couple of hundred
 *   --no-ao                 start with ambient occlusion off, for A/B shots
 *   --no-steam              do not connect to Steam. Scripted runs (--shot and
 *                           the self-tests) skip it anyway; this is the switch
 *                           for an interactive one
 *   --decals                scatter procedural demo decals over the board.
 *                           Nothing in the game places decals yet, so this is
 *                           the only way to see the projector pass do anything
 *                           — see DecalDemo.hpp
 *   --bake-benchmark        time the static sun bake and exit; no window
 *   --budget <tiles>        movement budget
 *   --iso <storey>          floor isolation ceiling
 *   --outline-ss <n>        selection-outline stencil supersample: 2, 4 or 8.
 *                           The quality/memory dial for the silhouette's edge —
 *                           omit it to keep the engine's default
 *   --select <index>        which unit starts selected
 *   --sprint                force the sprint-hover STATE (both rings, amber solid)
 *   --los                   start with the visibility overlay on
 *   --stairs                the staircase camera preset
 *   --cam px py pz tx ty tz free camera
 *   --no-cutaway            keep every wall - do not strip facings by angle
 *   --fov degrees           its vertical field of view (perspective)
 *   --ortho worldHeight     or make it orthographic, spanning this height
 *   --dev-view              start with the F1 dev panel open; the only way to
 *                           get it in a --shot, which has no F1 to press
 *   --minimap-realtime      capture the minimap every frame instead of five
 *                           times a second. A test switch — see the field
 *   --splash                stay on the splash screen: it does not time out,
 *                           and a scripted run opens on it rather than skipping
 *                           straight to the board. How you sit and look at the
 *                           thing while tuning its shader — F5 reloads it live
 *   --no-splash             skip the splash and its six-second minimum, and go
 *                           straight to the board
 *   --mouse <x> <y>         deterministic hover, for reproducible screenshots
 *   --boom <x> <y> <z>      detonate at startup, headless
 *   --log <file>            write the log here instead of xcom.log beside the
 *                           executable
 *   --log-level <name>      trace|debug|info|warn|error|fatal|off — the floor,
 *                           info by default. debug is the verbose setting: it
 *                           adds raylib's narration of every texture, shader
 *                           and framebuffer it loads. raylib's WARNINGS and
 *                           ERRORS are in the log at every level.
 */
#pragma once

#include "game/lattice/Cell.hpp"
#include "game/lattice/Constants.hpp"

#include <array>
#include <optional>
#include <string>

namespace game {

/* No `using namespace cromwell` here any more — nothing in this header names
 * the engine. That is worth keeping true: the command line describes a GAME
 * run, and the day it needs an engine type is the day to ask why. */

/* Which viewpoint the camera starts on. THIS GAME'S vocabulary — Staircase is
 * a framing of the demo map's stairwell — so it is declared here, where
 * --stairs and --cam are parsed, and Application translates it into
 * camera().at().lookingAt() calls. It lived in the engine's OrbitCamera for a
 * while, which put one game's demo-map coordinates in an engine header; the
 * engine's rig now knows nothing about presets, the same eviction ViewLayers
 * went through when it shipped a `units` switch. */
enum class CameraPreset : int { Default = 0, Staircase, Free };

struct CliOptions {
    std::optional<std::string> screenshotPath;
    int   screenshotFrame = 3;

    /* --web-selftest <log>: drive the browser surface through a scripted
       click-and-type and write what happened. Opens a window because the
       surface owns a GL texture, but never draws a frame of the game. */
    std::optional<std::string> webSelfTestPath;

    /* --web-url <url>: with --web-selftest, load this instead of running the
       scripted click-and-type, and simply survive it for a while. How a
       crash-on-navigation gets reproduced without a person driving. */
    std::optional<std::string> webSelfTestUrl;

    /* --web-type <text>: during a soak, focus the page's search field, type
       this and press enter. Reproduces "it crashed when I hit enter" without
       anybody having to hit enter. */
    std::optional<std::string> webSelfTestType;

    /* --compute-selftest [<log>]: dispatch one trivial compute shader and
       verify the results, proving the GL.hpp / ComputeShader.hpp path before
       any pass depends on it. Opens a window because compute needs a context,
       then exits without drawing a frame of the game. An empty string means
       report to the log only. See render/gpu/ComputeSelfTest.hpp. */
    std::optional<std::string> computeSelfTestPath;

    /* --device-selftest [<log>]: run the rhi backend conformance suite —
       resources, handle generations, clears, a depth-only pass, a shader and a
       fullscreen draw, each verified by reading the pixels back. Opens a window
       because a device needs a context, then exits without drawing a frame of
       the game.

       THE SAME SUITE RUNS AGAINST EVERY BACKEND, which is the point: it takes
       an IRenderDevice rather than the GL one, so a console or Metal port has a
       concrete list of failures to work down instead of "it compiles and the
       screen looks wrong". See cromwell/rhi/RenderDeviceSelfTest.hpp. */
    std::optional<std::string> deviceSelfTestPath;

    /* --renderer rhi: draw through rhi::IRenderDevice instead of raylib.

       TWO RENDERERS EXIST DURING THE PORT and this chooses between them. The
       rhi path is being built one pass at a time and is nowhere near parity;
       running it today gets a cleared backbuffer. It is selectable anyway,
       because a converted pass that cannot be LOOKED at beside the original is
       a pass nobody can review. See game/render/rhi/RhiFrameRenderer.hpp. */
    bool useRhiRenderer = false;

    bool  ambientOcclusion = true;

    /* --no-steam clears this. ON by default because a Steam build that only
     * connects when asked is a Steam build nobody notices is broken; a failure
     * to connect already costs nothing. */
    bool  steam = true;

    /* --decals: place the procedural demo marks. Scaffolding, and off by
     * default — the decal SYSTEM is always built, this only gives it content
     * to project until something in the game does. See DecalDemo.hpp. */
    bool  decalDemo = false;

    bool  bakeBenchmark = false;   /* headless; never opens a window */
    bool  forceDevView  = false;   /* open the dev panel at startup   */

    /* --outline-ss: how finely the selection outline's stencil is rasterised, as
     * a multiple of the window. 2, 4 or 8; the engine snaps anything else and
     * refuses to go below the scene's own supersample, because a stencil coarser
     * than the depth it is compared against cannot be aligned at all.
     *
     * ZERO MEANS "DO NOT ASK", which is not the same as a low setting: the engine
     * keeps its own default, so this flag is for A/B-ing the dial rather than
     * for expressing a preference. The place a preference will eventually live is
     * the user settings bag, not argv. */
    int   outlineSupersample = 0;

    /* --minimap-realtime: run the plan-view capture every frame instead of on
     * its 0.2 s interval. The interval is the shipped answer — see
     * CaptureSchedule.hpp for why — and this exists so a test can rule the
     * schedule out (is the marker lagging, or is the picture just a fifth of a
     * second old?) without editing the schedule and forgetting to put it
     * back. */
    bool  minimapRealtime = false;
    /* Open the widget gallery at startup, the same way --dev-view opens the
     * panel. Exists so the --shot path can capture it: the gallery is the only
     * place several widgets are drawn at all, so without this they can only be
     * looked at by hand and never in a scripted frame. */
    bool  forceUiGallery = false;

    /* Stay on the splash screen. Two separate effects, one intent — "I am
     * looking at the splash, not at the game":
     *
     *   - it does not time out after Application::kSplashMinimumSeconds, so an
     *     interactive run sits on it until the window is closed;
     *   - a SCRIPTED run opens on it rather than going straight to the board.
     *     Scripted runs skip the front end by default (see the `scripted` flag
     *     in Application::run) because a screenshot of a splash instead of the
     *     game is a silently useless artefact — which left the splash with no
     *     way to be captured at all.
     *
     * Both exist because the splash became an animated shader rather than a
     * line of text, and neither watching two seconds go past nor guessing from
     * the source is a way to tune one.
     *
     *     xcom --splash --no-steam                     sit and look at it
     *     xcom --shot s.png --shot-frame 40 --splash   capture one frame
     *
     * Frame 40 of 60 is two thirds of a second in, past the effect ramp.
     *
     * It was briefly defaulted ON while the splash was being art-directed, so
     * that every launch held on it whatever it was started from. That is what
     * `--no-splash` exists for, and why the pair reads oddly symmetrical for a
     * flag that is off by default. */
    bool  forceSplash   = false;

    /* --no-splash: start on the board and never show the splash at all.
     *
     * The splash now holds for a six-second MINIMUM, which is right for anyone
     * starting the game and wrong for the fiftieth build of an afternoon spent
     * on something the splash is in front of. This skips it outright rather
     * than shortening it — a developer switch, so the shipped timing is never
     * quietly tuned to suit whoever is iterating. */
    bool  skipSplash    = false;

    /* Which F-cycle debug view to start in — the only way to get one into a
     * --shot, which has no F to press. Same numbering as PbrShader::
     * setDebugView: 1 geometry, 2 probes, 3 rooms, 4 roughness, 5 occlusion. */
    int   debugView    = 0;
    float moveBudget   = 6.0f;
    int   isoLevel     = kDefaultStoreyCount - 1;

    /* KEEP EVERY WALL. The cutaway defaults to Dynamic, which removes the wall
     * facings the camera is looking through so the player can see into a room.
     * That is right for play and wrong for a reproduction: a screenshot taken
     * to show "it looks wrong here" comes back with the walls of the room in
     * question missing, and the thing being described is not in the picture.
     *
     * It is also the reason --cam alone was not enough to reproduce a view even
     * once the lens travelled with it. Sets CutawayMode::Manual at startup,
     * which keeps all facings; the storey cut is --iso and stays separate,
     * because hiding a FLOOR and hiding a WALL are different questions. */
    bool  keepWalls    = false;
    int   selectedUnit = 0;

    /* the sprint-hover STATE, which is both rings up with amber solid — not
     * the amber ring alone. TAB still cycles to that as a debug view. */
    bool forceBothRings = false;
    bool losMode        = false;

    CameraPreset        cameraPreset = CameraPreset::Default;
    std::array<float, 6> freeCamera{};   /* px py pz tx ty tz */

    /* THE LENS, WITHOUT WHICH A POSE DOES NOT REPRODUCE A PICTURE.
     *
     * A camera is not just where it is and what it looks at. `--cam` restored
     * the pose and left the lens at whatever the pawn's rig was built with, so
     * pasting the dev panel's line back in reproduced a DIFFERENT framing
     * whenever the view being described was not at that default — and the error
     * reads as the camera standing too far forward or too far back, which sends
     * you looking at the position it got right.
     *
     * Zero means "leave the rig's own lens alone", so a `--cam` from before
     * this existed still behaves exactly as it did.
     *
     * TWO FIELDS BECAUSE THERE ARE TWO KINDS OF LENS, and conflating them is
     * the trap Camera.hpp's header calls "the fovy trap": under perspective the
     * number is a vertical ANGLE, under orthographic it is a visible HEIGHT in
     * world units. One float carrying either would be raylib's Camera3D::fovy
     * again, reintroduced at the command line. */
    float cameraFov = 0.0f;         /* vertical degrees, perspective */
    float cameraOrthoHeight = 0.0f; /* world units, switches to orthographic */

    std::optional<int> mouseX;
    std::optional<int> mouseY;
    std::optional<Cell> detonateAt;

    /* Diagnostics. Empty path means "xcom.log beside the executable", which is
     * what you want every time except when two runs must not overwrite each
     * other's log. */
    std::string logPath;
    std::string logLevel = "info";

    static CliOptions parse(int argc, char** argv);
};

}  // namespace game
