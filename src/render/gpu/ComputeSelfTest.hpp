/* ComputeSelfTest.hpp — does the compute path actually run.
 *
 * SINGLE RESPONSIBILITY: answer that question before anything depends on the
 * answer, and name the stage that failed when it does.
 *
 * WHY THIS EXISTS. GL.hpp and ComputeShader.hpp compile and link without ever
 * having dispatched anything, and a compute chain has several links that fail
 * silently rather than loudly: a storage buffer bound at the wrong index reads
 * zeroes, a missing barrier returns last frame's contents, a dispatch sized in
 * groups where items were meant covers a fraction of the data. None of those
 * raise a GL error. All of them look like "my pass is wrong."
 *
 * So the infrastructure proves itself once, on demand, with arithmetic simple
 * enough that a mismatch can only mean the plumbing. The first real compute
 * pass then starts from a known-good base instead of debugging this and itself
 * at the same time.
 *
 * Same shape as WebSelfTest and for the same reason: measurement, not
 * judgement. The output is text and nothing here looks at a picture.
 */
#pragma once

#include <string>

namespace xcom {

/* Runs the whole sequence and returns a human-readable report. Needs a live GL
 * context — it creates buffers and a program — so call it after InitWindow.
 *
 * When `logPath` is non-empty the report is also written there, so a headless
 * or windowed-but-unattended run leaves evidence behind.
 *
 * Never throws and never asserts: a broken compute path should report itself
 * and let the app carry on without compute, exactly as a missing shader does. */
std::string runComputeSelfTest(const std::string& logPath = std::string());

/* True when the last runComputeSelfTest() passed every stage. Cheap to query,
 * so a pass that needs compute can gate on it rather than discovering the
 * problem mid-frame. False before the test has run at all. */
bool computeSelfTestPassed();

}  // namespace xcom
