#include "cromwell/platform/FrameClock.hpp"

#include <chrono>

namespace cromwell {
namespace {

/* MONOTONIC BY DEFINITION, which is the property that matters and the reason
 * this is not system_clock. A wall clock can be wound backwards by an NTP
 * correction or a user changing the date, and a duration measured across that
 * comes out negative — which, integrated, moves everything in the world
 * backwards for one frame. */
double steadySeconds()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point origin = Clock::now();

    const std::chrono::duration<double> since = Clock::now() - origin;
    return since.count();
}

/* How quickly the displayed frame rate chases the real one. 0.1 settles in
 * about twenty frames — fast enough to react to a real change, slow enough
 * that the number is readable rather than a blur. */
constexpr float kFpsSmoothing = 0.1f;

}  // namespace

FrameClock::FrameClock() : source_(&steadySeconds) {}

void FrameClock::setTimeSource(TimeSource source)
{
    source_ = source ? std::move(source) : TimeSource(&steadySeconds);

    /* The next tick would otherwise measure the gap between the OLD source's
     * last reading and the NEW source's first, which is a meaningless number
     * and frequently an enormous one. */
    started_ = false;
}

void FrameClock::setMaxDelta(float seconds)
{
    /* A ceiling of zero or less would freeze the simulation permanently while
     * every clock reading still advanced — the hardest kind of bug to see,
     * because everything animates and nothing moves. */
    if (seconds > 0.0f) maxDelta_ = seconds;
}

void FrameClock::setTimeScale(float scale)
{
    /* Negative time is not a feature. Rewinding is a replay concern and it is
     * done by replaying, not by integrating backwards — every system that
     * consumes delta assumes it is going forwards. */
    timeScale_ = scale < 0.0f ? 0.0f : scale;
}

void FrameClock::tick()
{
    const double now = source_();

    /* THE FIRST TICK MEASURES NOTHING. There is no previous sample to subtract,
     * and using the process start would make frame one as long as the load —
     * which, on a cold start with shaders to compile, is the worst possible
     * first delta to hand a simulation. */
    if (!started_) {
        started_    = true;
        lastSample_ = now;
        raw_        = 0.0f;
        clamped_    = 0.0f;
        hitched_    = false;
        frames_++;
        return;
    }

    double measured = now - lastSample_;
    lastSample_ = now;

    /* A non-monotonic source, or a first reading after one was swapped in.
     * Treated as zero rather than trusted; see setTimeSource. */
    if (measured < 0.0) measured = 0.0;

    raw_ = static_cast<float>(measured);

    /* THE RESUME CASE, and it is deliberately handled BEFORE the clamp.
     *
     * Clamping a thirty-second suspend still produces one full-length frame in
     * which everything that was mid-flight advances at once. What a resume
     * wants is no frame at all — the world was not running, so nothing should
     * have happened. */
    if (skipNext_) {
        skipNext_ = false;
        raw_      = 0.0f;
        clamped_  = 0.0f;
        hitched_  = false;
        frames_++;
        return;
    }

    hitched_ = raw_ > maxDelta_;
    clamped_ = hitched_ ? maxDelta_ : raw_;

    /* ELAPSED FOLLOWS THE CLAMPED VALUE, not the raw one. It is the sum of the
     * time the game believes has passed, so that anything driving an animation
     * from elapsed() stays in step with anything integrating delta(). Summing
     * the raw value instead would make the two disagree by exactly the amount
     * of every hitch — a drift that grows all session and is invisible until
     * something compares them. */
    elapsed_ += clamped_;
    frames_++;

    if (clamped_ > 0.0f) {
        const float instant = 1.0f / clamped_;
        smoothedFps_ = smoothedFps_ <= 0.0f
                           ? instant
                           : smoothedFps_ + (instant - smoothedFps_) * kFpsSmoothing;
    }
}

float FrameClock::delta() const
{
    if (paused_) return 0.0f;
    return clamped_ * timeScale_;
}

}  // namespace cromwell
