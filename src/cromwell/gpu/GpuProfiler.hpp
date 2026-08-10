/* GpuProfiler.hpp — how long a pass took on the DEVICE.
 *
 * SINGLE RESPONSIBILITY: own a pool of timer queries, hand them to named
 * passes, and feed the results into Profiler once the GPU has caught up.
 *
 * WHY IT IS NOT ENOUGH TO TIME THE CPU. A draw call returns when the command is
 * recorded, not when it has run. A frame where the CPU took 2 ms and then
 * waited 18 ms for the GPU looks identical, from the CPU, to a frame where the
 * CPU took 20 ms. Both are "a 20 ms frame". Only a device-side timestamp can
 * tell you which, and which one you have decides whether you go and optimise a
 * shader or a submission loop.
 *
 * THE RESULTS ARE LATE, AND THAT IS INHERENT. A query issued this frame is
 * readable a frame or two later; asking sooner stalls the pipeline, which is
 * the thing being measured. So collect() polls without blocking and reports
 * whatever has landed. The number on screen is two frames old. For a readout
 * you watch while flying the camera, that is invisible; for a capture, the
 * event carries the frame it was issued in.
 *
 * ZONES MUST NOT NEST. GL_TIME_ELAPSED is a single active query per context —
 * beginning a second one inside the first is an error, not a nested result.
 * The render passes here are siblings (shadow, gbuffer, ssao, lit, resolve) so
 * this costs nothing, and a nested begin is IGNORED rather than allowed to
 * corrupt the outer measurement.
 *
 * If per-pass detail inside a pass is ever wanted, that needs
 * glQueryCounter(GL_TIMESTAMP) pairs instead, which do nest. Not worth the
 * bookkeeping until something asks for it.
 *
 *   CW_GPU_ZONE("shadow map");   scoped, in the pass
 *   GpuProfiler::instance().collect();   once a frame, after present
 */
#pragma once

#include "cromwell/gpu/GL.hpp"

namespace cromwell {

class GpuProfiler {
public:
    static GpuProfiler& instance();

    /* No-op when the pool is exhausted or a zone is already open, so an
     * unbalanced or over-deep call site degrades to a missing number rather
     * than to a broken frame. */
    void begin(const char* name);
    void end();

    /* Polls every outstanding query and reports the finished ones into
     * Profiler. Non-blocking. Call once a frame after the buffer swap. */
    void collect();

    /* Queries in flight — a number that only grows means collect() is not
     * being called, which is the failure this exposes. */
    int pending() const;

private:
    GpuProfiler() = default;

    /* Enough for every pass in a frame to be in flight for the two or three
     * frames a result takes to land, with room to spare. Static, so a zone
     * never allocates. */
    static constexpr int kSlots = 64;

    struct Slot {
        const char*   name = nullptr;
        gl::TimerQuery query;
        bool          inFlight = false;
    };

    Slot slots_[kSlots];
    int  active_ = -1;   /* the one open query, or -1 */
};

/* RAII, matching ProfileScope on the CPU side. */
class GpuProfileScope {
public:
    explicit GpuProfileScope(const char* name) { GpuProfiler::instance().begin(name); }
    ~GpuProfileScope() { GpuProfiler::instance().end(); }

    GpuProfileScope(const GpuProfileScope&) = delete;
    GpuProfileScope& operator=(const GpuProfileScope&) = delete;
};

}  // namespace cromwell

/* Paste the line number in so two zones in one scope cannot collide. */
#define CW_GPU_ZONE_JOIN2(a, b) a##b
#define CW_GPU_ZONE_JOIN(a, b)  CW_GPU_ZONE_JOIN2(a, b)

#if XC_HAVE_TRACY
#include <tracy/TracyOpenGL.hpp>
/* Both consumers from one line. Tracy's macro already declares its own scoped
 * object, so this pairs it with ours rather than choosing between them. */
#define CW_GPU_ZONE(name)                                                     \
    ::cromwell::GpuProfileScope CW_GPU_ZONE_JOIN(cwGpuZone_, __LINE__)(name); \
    TracyGpuZone(name)
#define CW_GPU_CONTEXT() TracyGpuContext
#define CW_GPU_COLLECT()                                                      \
    do { ::cromwell::GpuProfiler::instance().collect(); TracyGpuCollect; } while (0)
#else
#define CW_GPU_ZONE(name)                                                     \
    ::cromwell::GpuProfileScope CW_GPU_ZONE_JOIN(cwGpuZone_, __LINE__)(name)
#define CW_GPU_CONTEXT() ((void)0)
#define CW_GPU_COLLECT() ::cromwell::GpuProfiler::instance().collect()
#endif
