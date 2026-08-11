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
 *   --select <index>        which unit starts selected
 *   --sprint                force the sprint-hover STATE (both rings, amber solid)
 *   --los                   start with the visibility overlay on
 *   --stairs                the staircase camera preset
 *   --cam px py pz tx ty tz free camera
 *   --dev-view              start with the F1 dev panel open; the only way to
 *                           get it in a --shot, which has no F1 to press
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

#include "cromwell/camera/OrbitCamera.hpp"   /* CameraPreset */
#include "game/lattice/Cell.hpp"
#include "game/lattice/Constants.hpp"

#include <array>
#include <optional>
#include <string>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

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
    int   selectedUnit = 0;

    /* the sprint-hover STATE, which is both rings up with amber solid — not
     * the amber ring alone. TAB still cycles to that as a debug view. */
    bool forceBothRings = false;
    bool losMode        = false;

    CameraPreset        cameraPreset = CameraPreset::Default;
    std::array<float, 6> freeCamera{};   /* px py pz tx ty tz */

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
