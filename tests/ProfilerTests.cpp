/* ProfilerTests.cpp — the capture must be a file something else can open.
 *
 * THE FAILURE THIS EXISTS TO CATCH is not a wrong number. It is a trace that
 * Perfetto refuses to load — a trailing comma, an unescaped name, an empty
 * event array — which is discovered at the end of a profiling session, after
 * the thing being investigated has stopped happening.
 *
 * So this records synthetic frames, writes a capture, and checks the JSON is
 * structurally sound: balanced braces and brackets outside strings, the keys
 * the format requires, one event per zone, and both lanes present. It does not
 * validate against the full Trace Event spec — it validates the shape this
 * writer produces, which is what can regress.
 */
#include "cromwell/diag/Profiler.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

std::string readAll(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};

    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, got);

    std::fclose(file);
    return text;
}

/* Brace/bracket balance, ignoring anything inside a string. Enough to catch a
 * truncated or comma-mangled file without linking a JSON parser. */
bool balanced(const std::string& text)
{
    int braces = 0, brackets = 0;
    bool inString = false, escaped = false;

    for (char c : text) {
        if (inString) {
            if (escaped)         escaped = false;
            else if (c == '\\')  escaped = true;
            else if (c == '"')   inString = false;
            continue;
        }
        switch (c) {
            case '"': inString = true; break;
            case '{': braces++;   break;
            case '}': braces--;   break;
            case '[': brackets++; break;
            case ']': brackets--; break;
            default: break;
        }
        if (braces < 0 || brackets < 0) return false;
    }
    return braces == 0 && brackets == 0 && !inString;
}

int countOccurrences(const std::string& text, const std::string& needle)
{
    int count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        count++;
    return count;
}

void testCaptureWritesLoadableJson()
{
    std::printf("== profiler capture ==\n");

    Profiler& profiler = Profiler::instance();

    /* Three frames, two CPU zones each (one nested) and one GPU report. */
    profiler.toggleCapture();
    CHECK(profiler.capturing(), "capture did not start");

    constexpr int kFrames = 3;
    for (int frame = 0; frame < kFrames; frame++) {
        {
            ProfileScope outer("outer");
            {
                ProfileScope inner("inner");
            }
        }
        profiler.reportGpu("gpu pass", 1.25);
        profiler.endFrame();
    }

    CHECK(profiler.capturedFrames() == kFrames,
          "captured %d frames, expected %d", profiler.capturedFrames(), kFrames);

    profiler.toggleCapture();
    CHECK(!profiler.capturing(), "capture did not stop");

    const std::string path = profiler.writeCapture("profiler_test_capture.json");
    CHECK(!path.empty(), "writeCapture returned no path");
    if (path.empty()) return;

    const std::string json = readAll(path);
    CHECK(!json.empty(), "capture file is empty");

    CHECK(balanced(json), "capture JSON has unbalanced braces or brackets");

    /* The keys the format needs, and the two lanes. */
    CHECK(json.find("\"traceEvents\"") != std::string::npos, "no traceEvents array");
    CHECK(json.find("\"displayTimeUnit\"") != std::string::npos, "no displayTimeUnit");
    CHECK(json.find("\"name\":\"CPU\"") != std::string::npos, "no CPU lane");
    CHECK(json.find("\"name\":\"GPU\"") != std::string::npos, "no GPU lane");

    /* One complete event per zone recorded, in each frame. */
    CHECK(countOccurrences(json, "\"name\":\"outer\"") == kFrames,
          "expected %d outer events, found %d",
          kFrames, countOccurrences(json, "\"name\":\"outer\""));
    CHECK(countOccurrences(json, "\"name\":\"inner\"") == kFrames,
          "expected %d inner events, found %d",
          kFrames, countOccurrences(json, "\"name\":\"inner\""));
    CHECK(countOccurrences(json, "\"name\":\"gpu pass\"") == kFrames,
          "expected %d gpu events, found %d",
          kFrames, countOccurrences(json, "\"name\":\"gpu pass\""));

    /* GPU events go in lane 2, CPU in lane 1 — if they shared a lane the
     * viewer would draw them as impossible nesting. */
    CHECK(json.find("\"tid\":2") != std::string::npos, "no events on the GPU lane");

    /* Writing clears the capture, so a second F9 does not re-emit the first. */
    CHECK(profiler.capturedFrames() == 0, "capture was not cleared after writing");
    CHECK(profiler.writeCapture("profiler_test_should_not_exist.json").empty(),
          "an emptied capture still wrote a file");

    std::printf("   %d frames -> %zu bytes of valid trace JSON\n", kFrames, json.size());
    std::remove(path.c_str());
}

/* THE OUTERMOST ZONE IS STILL OPEN WHEN THE FRAME ENDS, always.
 *
 * A frame zone declared at the top of the loop body is in scope when the end of
 * that body calls endFrame(), so its destructor has not run. Dropping unclosed
 * events therefore lost the whole-frame CPU row — the single most useful line
 * in the panel — on every frame, which is exactly how it was found: the panel
 * showed per-pass GPU numbers and no CPU total.
 *
 * So this is the regression test for the real bug, not a robustness curiosity. */
void testOpenZoneIsStillReported()
{
    std::printf("== profiler open zones ==\n");

    Profiler& profiler = Profiler::instance();

    {
        ProfileScope frame("frame");        /* still open below */
        {
            ProfileScope inner("inner");
        }
        profiler.endFrame();                /* frame ends INSIDE "frame" */
    }

    bool sawFrame = false, sawInner = false;
    for (const Profiler::Row& row : profiler.rows()) {
        if (std::string(row.name) == "frame") { sawFrame = true;
            CHECK(row.cpuMs >= 0.0, "open zone reported a negative time"); }
        if (std::string(row.name) == "inner") sawInner = true;
    }

    CHECK(sawFrame, "the still-open frame zone was dropped instead of closed");
    CHECK(sawInner, "the closed inner zone is missing");

    profiler.endFrame();
    CHECK(profiler.frameMs() >= 0.0, "frame time went negative");
    std::printf("   an open zone is charged up to the frame end, not discarded\n");
}

/* A CPU zone and a GPU report of the SAME NAME must land on one row. They are
 * written as two separate string literals, and whether the compiler pools those
 * into one address is not something to depend on — unpooled, the pass shows up
 * twice, once with no GPU time and once with no CPU time, which reads as a
 * broken measurement. */
void testCpuAndGpuMergeByName()
{
    std::printf("== profiler row merging ==\n");

    Profiler& profiler = Profiler::instance();

    /* Deliberately built at runtime so it cannot share an address with the
     * literal below, which is the case the pointer compare got wrong. */
    std::string built = "shadow";
    built += " map";

    {
        ProfileScope pass(built.c_str());
    }
    profiler.reportGpu("shadow map", 2.5);
    profiler.endFrame();

    int matches = 0;
    for (const Profiler::Row& row : profiler.rows()) {
        if (std::string(row.name) != "shadow map") continue;
        matches++;
        /* `calls` counts CPU events only, so a non-zero call count is what
         * proves the CPU side merged in. Asserting on cpuMs would be a timing
         * assertion — an empty scope legitimately takes near zero, and a test
         * that fails when the machine is fast is a test that gets deleted. */
        CHECK(row.calls == 1, "merged row lost its CPU event (calls=%d)", row.calls);
        CHECK(row.cpuMs >= 0.0, "merged row has a negative CPU time");
        CHECK(row.gpuMs > 2.0, "merged row lost its GPU time (%.2f)", row.gpuMs);
    }
    CHECK(matches == 1, "expected 1 merged row for 'shadow map', found %d", matches);

    std::printf("   cpu and gpu with the same name share one row\n");
}

}  // namespace

int main()
{
    testCaptureWritesLoadableJson();
    testOpenZoneIsStillReported();
    testCpuAndGpuMergeByName();

    if (g_failures) std::printf("\n%d FAILURE(S)\n", g_failures);
    else            std::printf("\nall profiler checks passed\n");
    return g_failures ? 1 : 0;
}
