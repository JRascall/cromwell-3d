/* Profiler.hpp — where this frame's time went, live and in the game.
 *
 * SINGLE RESPONSIBILITY: record named CPU and GPU spans, keep the last frame's
 * worth for display, and write a capture when asked. It draws nothing and knows
 * nothing about GL — the GPU numbers are reported INTO it by whoever owns the
 * context.
 *
 * WHY AN IN-ENGINE PROFILER WHEN TRACY EXISTS. Tracy is better at analysis and
 * worse at the question you ask most often, which is "is it slow right now, and
 * which pass". Answering that should not need a second application, a socket,
 * and a build flag — it should be a panel in the dev overlay while you fly the
 * camera around. So the zones feed this always, and Tracy as well when
 * XC_TRACY is on; see Profile.hpp. One set of instrumentation, two consumers,
 * because instrumentation written twice drifts.
 *
 * SPANS, NOT TOTALS. Each zone records a start and an end, so nesting survives.
 * The live panel aggregates by name, but a capture keeps the tree — which is
 * the difference between "the lit pass cost 6 ms" and being able to see what
 * inside it did.
 *
 * NAMES ARE STRING LITERALS AND ARE STORED AS POINTERS. The macros pass
 * __func__ or a literal, both of which outlive the profiler. Nothing here
 * copies or frees a name, which is what keeps a zone down to two timestamps and
 * a push_back. Do not hand it a std::string's c_str().
 *
 * COST. About 40 ns per zone. That is nothing around a render pass and ruinous
 * inside a ray step — a zone in RayCaster's DDA loop would cost more than the
 * loop it measures and would produce tens of millions of events per bake.
 * Instrument the caller, not the inner loop; see CLAUDE.md.
 *
 * THREADING. Single-threaded by design. The frame loop and the render passes
 * are one thread, and that is what this is for. Zones from a worker (SunBaker's
 * pool) would corrupt the event list — leave those to the benchmark, which
 * measures them properly.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cromwell {

class Profiler {
public:
    /* One recorded span. `depth` is nesting level within the frame, which is
     * what lets a capture be drawn as a flame graph. */
    struct Event {
        const char*  name = nullptr;
        std::int64_t startNs = 0;
        /* NEGATIVE means still open. Not zero: zero is a legitimate timestamp
         * in the first instant of the run, and an open zone that looked closed
         * at time zero would be reported as a span of minus-everything. */
        std::int64_t endNs = -1;
        int          depth = 0;
        bool         gpu = false;
    };

    /* One row of the live panel: everything with this name, this frame. */
    struct Row {
        const char* name = nullptr;
        double      cpuMs = 0.0;
        double      gpuMs = 0.0;
        int         calls = 0;
        int         depth = 0;
    };

    static Profiler& instance();

    /* ---- recording ------------------------------------------------------
     * Prefer the macros in Profile.hpp; these are what they call. */
    int  beginZone(const char* name);
    void endZone(int handle);

    /* GPU spans arrive LATE — a timestamp query issued this frame is readable
     * two or three frames later. They are therefore reported by name against
     * the frame they are finally read in, not the frame they were issued in.
     * That is a small lie on the timeline and the honest alternative (holding
     * every frame open until its queries drain) costs more than it is worth for
     * a live readout. A capture records the real issue frame. */
    void reportGpu(const char* name, double milliseconds);

    /* Closes the frame: aggregates rows for the panel, appends to a capture if
     * one is running. Call once, after present. */
    void endFrame();

    /* ---- reading --------------------------------------------------------- */
    const std::vector<Row>& rows() const { return rows_; }

    /* Wall time of the last complete frame, and a smoothed version — a number
     * that jitters every frame cannot be read off a screen. */
    double frameMs() const { return frameMs_; }
    double smoothedFrameMs() const { return smoothedMs_; }

    /* ---- capture ---------------------------------------------------------
     * A capture holds full events for a bounded number of frames, so a long
     * one cannot exhaust memory during a session that was left running. */
    static constexpr int kMaxCaptureFrames = 600;

    void toggleCapture();
    bool capturing() const { return capturing_; }
    int  capturedFrames() const { return capturedFrames_; }

    /* Writes the capture as Chrome Trace Event JSON and clears it. Open the
     * result at ui.perfetto.dev or chrome://tracing — both give a zoomable
     * flame graph for free, which is a great deal more than a bespoke viewer
     * would be worth writing.
     *
     * Returns the path written, or an empty string if there was nothing to
     * write or the file could not be opened. */
    std::string writeCapture(const std::string& path);

    /* Dropped because the capture hit kMaxCaptureFrames. Surfaced so a
     * truncated capture is never mistaken for a complete one. */
    bool captureTruncated() const { return truncated_; }

private:
    Profiler() = default;

    /* NANOSECONDS, not microseconds. A zone around something small — a mask
     * build, a pointer pick — takes well under a microsecond, and at that
     * resolution it reads as 0.00 in the panel, which is indistinguishable
     * from "not measured". The trace format wants microseconds and gets them
     * as a fraction on the way out. */
    std::int64_t nowNs() const;

    std::vector<Event> events_;      /* this frame, reused */
    std::vector<Row>   rows_;        /* last frame, aggregated for the panel */

    int          depth_ = 0;
    std::int64_t frameStartNs_ = 0;
    double       frameMs_ = 0.0;
    double       smoothedMs_ = 0.0;

    /* Captured frames, flattened. `frameStarts_` indexes into it. */
    bool               capturing_ = false;
    bool               truncated_ = false;
    int                capturedFrames_ = 0;
    std::vector<Event> capture_;
    std::vector<int>   frameStarts_;
};

/* RAII span. Prefer CW_PROFILE_ZONE_N in Profile.hpp, which compiles away
 * cleanly and also feeds Tracy. */
class ProfileScope {
public:
    explicit ProfileScope(const char* name)
        : handle_(Profiler::instance().beginZone(name)) {}
    ~ProfileScope() { Profiler::instance().endZone(handle_); }

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    int handle_;
};

}  // namespace cromwell
