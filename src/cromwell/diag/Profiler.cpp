#include "cromwell/diag/Profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace cromwell {

namespace {

/* How hard the displayed frame time is smoothed. A raw per-frame number is
 * unreadable — it jitters by whole milliseconds — and a heavily damped one
 * hides the spike you are hunting. This is the usual compromise. */
constexpr double kSmoothing = 0.9;

using Clock = std::chrono::steady_clock;

const Clock::time_point kEpoch = Clock::now();

}  // namespace

Profiler& Profiler::instance()
{
    /* Function-local static: constructed on first use, which is before any
     * zone can run, and destroyed after main. No initialisation order to get
     * wrong. */
    static Profiler profiler;
    return profiler;
}

std::int64_t Profiler::nowNs() const
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now() - kEpoch).count();
}

int Profiler::beginZone(const char* name)
{
    Event event;
    event.name    = name;
    event.startNs = nowNs();
    event.depth   = depth_++;

    events_.push_back(event);
    return static_cast<int>(events_.size()) - 1;
}

void Profiler::endZone(int handle)
{
    if (handle < 0 || handle >= static_cast<int>(events_.size())) return;
    events_[static_cast<std::size_t>(handle)].endNs = nowNs();
    if (depth_ > 0) depth_--;
}

void Profiler::reportGpu(const char* name, double milliseconds)
{
    /* GPU spans have no meaningful position on the CPU timeline — they are a
     * duration measured on another device. Recorded at the frame's start so a
     * capture puts them in a lane of their own rather than interleaved with
     * CPU work they did not run alongside. */
    Event event;
    event.name    = name;
    event.startNs = frameStartNs_;
    event.endNs   = frameStartNs_ + static_cast<std::int64_t>(milliseconds * 1.0e6);
    event.depth   = 0;
    event.gpu     = true;
    events_.push_back(event);
}

void Profiler::endFrame()
{
    const std::int64_t endNs = nowNs();
    frameMs_ = static_cast<double>(endNs - frameStartNs_) / 1.0e6;

    /* CLOSE ANYTHING STILL OPEN, rather than discarding it.
     *
     * This is not a rare edge case, it is the NORMAL case for the outermost
     * zone. A frame zone declared at the top of the loop body is still in
     * scope when the end of that body calls endFrame(), so its destructor has
     * not run yet — and dropping unclosed events meant the whole-frame CPU row,
     * the single most useful line in the panel, never appeared at all.
     *
     * Charging it up to here is also the right answer for a zone left open by
     * an early return: "at least this long" beats vanishing. */
    for (Event& event : events_)
        if (event.endNs < 0) event.endNs = endNs;

    /* Seed rather than ease on the first frame, or the readout spends its
     * first second climbing from zero. */
    smoothedMs_ = (smoothedMs_ <= 0.0)
        ? frameMs_
        : smoothedMs_ * kSmoothing + frameMs_ * (1.0 - kSmoothing);

    /* ---- aggregate for the live panel ----------------------------------
     * By name, keeping the shallowest depth seen so the panel can indent.
     * Linear scan per event: the row count is a couple of dozen, and a map
     * would allocate every frame to save nothing. */
    rows_.clear();
    for (const Event& event : events_) {
        if (event.endNs < event.startNs || !event.name) continue;

        const double ms = static_cast<double>(event.endNs - event.startNs) / 1.0e6;

        /* MATCHED BY TEXT, NOT BY POINTER. A CPU zone and its GPU counterpart
         * are written as two separate "shadow map" literals; whether the
         * compiler pools those into one address is its business, not something
         * to depend on. Without this the same pass can appear as two rows —
         * one with only a CPU time and one with only a GPU time — which reads
         * as a missing measurement rather than as a duplicate.
         *
         * strcmp over a couple of dozen rows, once a frame. A map would
         * allocate every frame to save nothing. */
        Row* row = nullptr;
        for (Row& candidate : rows_)
            if (candidate.name == event.name ||
                (candidate.name && std::strcmp(candidate.name, event.name) == 0)) {
                row = &candidate;
                break;
            }

        if (!row) {
            rows_.push_back(Row{ event.name, 0.0, 0.0, 0, event.depth });
            row = &rows_.back();
        }

        if (event.gpu) row->gpuMs += ms;
        else         { row->cpuMs += ms; row->calls++; }
        row->depth = std::min(row->depth, event.depth);
    }

    /* ---- append to a running capture ------------------------------------ */
    if (capturing_) {
        if (capturedFrames_ < kMaxCaptureFrames) {
            frameStarts_.push_back(static_cast<int>(capture_.size()));
            capture_.insert(capture_.end(), events_.begin(), events_.end());
            capturedFrames_++;
        } else {
            truncated_ = true;
        }
    }

    events_.clear();
    depth_ = 0;
    frameStartNs_ = endNs;
}

void Profiler::toggleCapture()
{
    if (capturing_) {
        capturing_ = false;
        return;
    }

    capture_.clear();
    frameStarts_.clear();
    capturedFrames_ = 0;
    truncated_ = false;
    capturing_ = true;
}

std::string Profiler::writeCapture(const std::string& path)
{
    if (capture_.empty()) return {};

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return {};

    /* CHROME TRACE EVENT FORMAT, because it is trivially writable and opens in
     * ui.perfetto.dev and chrome://tracing with zooming, flame graphs and
     * search already built. Writing a viewer would be the expensive half of a
     * profiler, and this skips it entirely.
     *
     * "ph":"X" is a complete event: one record with a start and a duration,
     * rather than a begin/end pair. "ts"/"dur" are MICROSECONDS.
     *
     * pid 1 throughout; tid separates the lanes — 1 for CPU, 2 for GPU, so the
     * two appear as separate rows rather than as impossible nesting. */
    std::fprintf(file, "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n");
    std::fprintf(file,
        "{\"ph\":\"M\",\"pid\":1,\"tid\":1,\"name\":\"thread_name\","
        "\"args\":{\"name\":\"CPU\"}},\n");
    std::fprintf(file,
        "{\"ph\":\"M\",\"pid\":1,\"tid\":2,\"name\":\"thread_name\","
        "\"args\":{\"name\":\"GPU\"}}");

    for (const Event& event : capture_) {
        if (event.endNs < event.startNs || !event.name) continue;

        std::fprintf(file,
            ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":%d,\"name\":\"%s\","
            "\"ts\":%.3f,\"dur\":%.3f}",
            event.gpu ? 2 : 1,
            event.name,
            static_cast<double>(event.startNs) / 1000.0,
            static_cast<double>(event.endNs - event.startNs) / 1000.0);
    }

    std::fprintf(file, "\n]}\n");
    std::fclose(file);

    capture_.clear();
    frameStarts_.clear();
    capturedFrames_ = 0;
    return path;
}

}  // namespace cromwell
