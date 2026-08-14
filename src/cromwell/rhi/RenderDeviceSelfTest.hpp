/* RenderDeviceSelfTest.hpp — does a backend actually do what it says.
 *
 * SINGLE RESPONSIBILITY: run every stage of IRenderDevice against a live
 * device, verify the pixels that came out, and name the stage that failed.
 *
 * ==================== WHY THIS IS NOT A GL TEST ===========================
 *
 * It takes an IRenderDevice, not a OpenGlRenderDevice, so the same suite runs
 * against every backend the engine ever grows. That is the whole point: when a
 * console or Metal backend is written, the question "is it finished" has an
 * answer that is not "it compiles and the screen is not obviously wrong."
 *
 * A backend author runs this first and works down the failures. Nobody has to
 * describe the contract in prose, and nobody has to discover clause by clause
 * that a depth-only pass needs its draw buffer disabled.
 *
 * ============ WHY A SELF-TEST AND NOT A ctest SUITE =======================
 *
 * Because it needs a GPU and a live context, and the headless suites
 * deliberately have neither — see the split argued in CMakeLists.txt. A test
 * binary that has to open a window is a test nobody runs on CI.
 *
 * So it follows the shape ComputeSelfTest and WebSelfTest already established
 * here: a flag on the executable, a text report, an exit code a build script
 * can gate on. Same reason those exist — GL.hpp and ComputeShader.hpp compiled
 * and linked for months without ever having dispatched anything, and a render
 * device is far larger and fails far more quietly.
 *
 * ======================= WHAT IT ACTUALLY PROVES ==========================
 *
 * Only what it can check by READING PIXELS BACK. A pass that runs without a GL
 * error proves nothing — the interesting failures all produce a clean run and
 * a wrong image: a clear that was scissored away by leftover state, a viewport
 * sized to the window instead of the attachment, a framebuffer that silently
 * fell back to the backbuffer, a draw that wrote to the wrong attachment.
 *
 * Every stage below therefore ends in a readback and a comparison against a
 * value that could only have come from the thing being tested.
 *
 * WHAT IT CANNOT PROVE: that the picture looks right. Shading, filtering and
 * blending correctness need eyes or a golden image, and neither belongs here.
 */
#pragma once

#include <string>

namespace cromwell::rhi {

class IRenderDevice;

struct SelfTestResult {
    bool        passed = false;
    int         stagesRun = 0;
    int         stagesFailed = 0;
    std::string report;   /* human-readable, one line per stage */
};

/* Runs every stage against `device` and returns what happened. Needs a live
 * context, so call it after the platform is up.
 *
 * NEVER THROWS AND NEVER ASSERTS. A broken backend should report itself and let
 * the caller decide, exactly as a missing shader does — an abort here would
 * mean the one tool for diagnosing a bad port cannot survive a bad port.
 *
 * Cleans up everything it created, whether or not it passed, so it is safe to
 * run before a normal session rather than only in place of one. */
SelfTestResult runRenderDeviceSelfTest(IRenderDevice& device);

}  // namespace cromwell::rhi
