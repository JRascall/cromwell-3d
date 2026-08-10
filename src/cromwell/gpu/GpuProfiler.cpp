#include "cromwell/gpu/GpuProfiler.hpp"

#include "cromwell/diag/Profiler.hpp"

namespace cromwell {

GpuProfiler& GpuProfiler::instance()
{
    static GpuProfiler profiler;
    return profiler;
}

void GpuProfiler::begin(const char* name)
{
    /* GL_TIME_ELAPSED allows one active query per context. A nested begin is
     * a caller mistake; swallowing it costs one missing number, while passing
     * it to GL would invalidate the enclosing measurement too. */
    if (active_ >= 0) return;

    for (int i = 0; i < kSlots; i++) {
        Slot& slot = slots_[i];
        if (slot.inFlight) continue;

        if (!slot.query.valid()) {
            slot.query.create();
            /* No context, or a driver without the extension. Give up quietly
             * rather than every frame forever — the CPU zones still work. */
            if (!slot.query.valid()) return;
        }

        slot.name     = name;
        slot.inFlight = true;
        slot.query.begin();
        active_ = i;
        return;
    }

    /* Pool exhausted, which means results are not being collected. Nothing to
     * do here but decline; pending() is how that gets noticed. */
}

void GpuProfiler::end()
{
    if (active_ < 0) return;
    slots_[active_].query.end();
    active_ = -1;
}

void GpuProfiler::collect()
{
    Profiler& profiler = Profiler::instance();

    for (int i = 0; i < kSlots; i++) {
        Slot& slot = slots_[i];
        if (!slot.inFlight || i == active_) continue;

        /* Non-blocking on purpose: a query issued this frame is normally NOT
         * ready, and waiting for it would stall the pipeline this class exists
         * to measure. It lands in a frame or two. */
        if (!slot.query.resultReady()) continue;

        profiler.reportGpu(slot.name, slot.query.milliseconds());
        slot.inFlight = false;
        slot.name     = nullptr;
    }
}

int GpuProfiler::pending() const
{
    int count = 0;
    for (int i = 0; i < kSlots; i++)
        if (slots_[i].inFlight) count++;
    return count;
}

}  // namespace cromwell
