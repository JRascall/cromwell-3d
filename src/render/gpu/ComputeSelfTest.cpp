#include "render/gpu/ComputeSelfTest.hpp"

#include "render/gpu/ComputeShader.hpp"
#include "render/gpu/GL.hpp"

#include "raylib.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace xcom {
namespace {

bool g_passed = false;

/* Deliberately not a multiple of the shader's local_size_x of 64. The partial
 * last group is the case a dispatch sized in groups rather than items gets
 * wrong, and a count of 1000 leaves 40 invocations past the end of the data —
 * so a missing bounds check in the shader, or a missing round-up here, both
 * show up as a mismatch rather than passing by luck. */
constexpr unsigned int kItemCount = 1000;

void append(std::string& report, const char* line)
{
    report += line;
    report += '\n';
}

void appendf(std::string& report, const char* format, ...)
{
    char buffer[512];

    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    append(report, buffer);
}

}  // namespace

std::string runComputeSelfTest(const std::string& logPath)
{
    g_passed = false;

    std::string report;
    append(report, "compute self-test");
    append(report, "-----------------");

    /* ---- stage 1: does this context have compute at all ------------------ */

    if (!gl::computeAvailable()) {
        append(report, "FAIL  context has no compute support");
        append(report, "      expected a 4.3 core context; see OPENGL_VERSION in CMakeLists.txt");
        if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
        return report;
    }
    append(report, "ok    context reports compute support");

    /* ---- stage 2: load, compile, link ------------------------------------ */

    ComputeShader shader;
    if (!shader.load("compute_selftest.comp.glsl")) {
        append(report, "FAIL  compute_selftest.comp.glsl did not load");
        append(report, "      see the log above for the driver's compile output");
        if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
        return report;
    }
    appendf(report, "ok    program loaded and linked (id %u)", shader.program());

    /* ---- stage 3: buffers ------------------------------------------------ */

    std::vector<unsigned int> input(kItemCount);
    for (unsigned int i = 0; i < kItemCount; i++) input[i] = i;

    ShaderStorageBuffer inputBuffer;
    ShaderStorageBuffer outputBuffer;

    const std::size_t bytes = kItemCount * sizeof(unsigned int);

    if (!inputBuffer.create(bytes, input.data(), /*dynamic=*/false) ||
        !outputBuffer.create(bytes, nullptr, /*dynamic=*/true)) {
        append(report, "FAIL  storage buffers could not be created");
        if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
        return report;
    }
    appendf(report, "ok    two storage buffers of %u bytes", static_cast<unsigned>(bytes));

    /* ---- stage 4: dispatch ----------------------------------------------- */

    gl::checkErrors("before compute self-test dispatch");

    {
        /* BarrierBufferUpdate because the consumer here is a CPU readback.
         * A pass feeding a draw would want BarrierShaderStorage, and one
         * feeding an indirect draw BarrierCommand — see GL.hpp. */
        ComputePass pass(shader, gl::BarrierBufferUpdate);

        if (!pass.valid()) {
            append(report, "FAIL  compute pass would not start");
            if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
            return report;
        }

        shader.setUInt("uCount", kItemCount);
        inputBuffer.bind(0);
        outputBuffer.bind(1);

        pass.dispatchItems(kItemCount, 64);
    }  /* barrier fires here */

    if (!gl::checkErrors("compute self-test dispatch")) {
        append(report, "FAIL  the dispatch raised a GL error - see the log");
        if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
        return report;
    }
    appendf(report, "ok    dispatched %u items over %u groups of 64",
            kItemCount, (kItemCount + 63) / 64);

    /* ---- stage 5: did it compute the right thing ------------------------- */

    std::vector<unsigned int> output(kItemCount, 0xFFFFFFFFu);
    outputBuffer.read(output.data(), bytes);

    unsigned int mismatches = 0;
    unsigned int firstBad   = 0;

    for (unsigned int i = 0; i < kItemCount; i++) {
        const unsigned int expected = input[i] * 2u + 1u;
        if (output[i] != expected) {
            if (mismatches == 0) firstBad = i;
            mismatches++;
        }
    }

    if (mismatches != 0) {
        appendf(report, "FAIL  %u of %u results wrong", mismatches, kItemCount);
        appendf(report, "      first at index %u: expected %u, got %u",
                firstBad, input[firstBad] * 2u + 1u, output[firstBad]);

        /* The two failures worth telling apart by eye, because they point at
         * completely different bugs. */
        if (mismatches == kItemCount) {
            append(report, "      EVERY result wrong - the dispatch did not run, or the "
                           "buffers bound at the wrong indices");
        } else if (firstBad >= (kItemCount / 64) * 64) {
            append(report, "      only the tail is wrong - the dispatch did not cover the "
                           "partial last group");
        }

        if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
        return report;
    }

    appendf(report, "ok    all %u results correct", kItemCount);
    append(report, "");
    append(report, "PASS  compute is usable: program, storage buffers, dispatch, barrier, readback");

    g_passed = true;

    if (!logPath.empty()) SaveFileText(logPath.c_str(), report.data());
    return report;
}

bool computeSelfTestPassed()
{
    return g_passed;
}

}  // namespace xcom
