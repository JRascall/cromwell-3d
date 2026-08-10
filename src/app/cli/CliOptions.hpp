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

#include "core/lattice/Cell.hpp"
#include "core/lattice/Constants.hpp"

#include <array>
#include <optional>
#include <string>

namespace xcom {

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

    bool  ambientOcclusion = true;

    /* --decals: place the procedural demo marks. Scaffolding, and off by
     * default — the decal SYSTEM is always built, this only gives it content
     * to project until something in the game does. See DecalDemo.hpp. */
    bool  decalDemo = false;

    bool  bakeBenchmark = false;   /* headless; never opens a window */
    bool  forceDevView  = false;   /* open the dev panel at startup   */

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

}  // namespace xcom
