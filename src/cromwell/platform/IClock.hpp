/* IClock.hpp — how much time passed, and how much of it the game should believe.
 *
 * SINGLE RESPONSIBILITY: measure the frame's elapsed time and the wall clock,
 * and be the one place a suspicious delta is caught.
 *
 * ==================== WHY THIS IS AN INTERFACE AT ALL ======================
 *
 * Two reasons, and the second is the one that matters.
 *
 * It is a platform service — raylib's GetFrameTime/GetTime are as much a
 * dependency as its window, and a console backend has its own.
 *
 * But mainly: THE FRAME DELTA IS THE MOST DANGEROUS NUMBER IN A GAME LOOP, and
 * it needs one owner. It goes enormous when a machine hitches, when a console
 * suspends to the system menu and resumes, when the window is dragged (Windows
 * blocks inside the modal loop and the next delta is however long the player
 * held the title bar), and when a debugger breakpoint is stepped over. Every
 * one of those produces a frame where the delta is seconds rather than
 * milliseconds, and the symptoms are the same each time: units teleport through
 * walls because a movement step outran the collision sweep, timers fire several
 * times at once, and a physics integrator diverges.
 *
 * Clamping it in the loop is the usual fix, and the usual mistake is that
 * whoever writes the SECOND loop — a tool, a headless sim, a replay — does not
 * know to. Putting the clamp behind the clock means every consumer gets a delta
 * that is already safe to integrate, and the one that genuinely wants the raw
 * measurement has to ask for it by name.
 *
 * ======================= THE THREE CLOCKS ARE DIFFERENT ====================
 *
 * `delta` is game time and is clamped, scaled and pausable — it is what
 * simulation integrates against.
 *
 * `unscaledDelta` ignores pause and time scale, and is what UI animation,
 * loading spinners and the profiler want. A pause menu whose spinner freezes
 * because it integrated game time is the bug this pair prevents.
 *
 * `elapsed` is a monotonic wall clock in seconds since start — never wound back
 * by an NTP correction, which a system clock can be, and which would make a
 * duration measured across it come out negative.
 */
#pragma once

#include <cstdint>

namespace cromwell {

class IClock {
public:
    virtual ~IClock() = default;

    /* Advance to the next frame. Called once, at the top of the loop, by
     * whatever owns it — everything below reports the same values until it is
     * called again, so a frame cannot see time move underneath it. */
    virtual void tick() = 0;

    /* ---- the frame's delta ---------------------------------------------*/

    /* GAME SECONDS: clamped, scaled by timeScale, zero while paused. What
     * simulation integrates. */
    virtual float delta() const = 0;

    /* REAL SECONDS, still clamped but ignoring pause and scale. UI animation,
     * loading indicators, the profiler. */
    virtual float unscaledDelta() const = 0;

    /* THE MEASUREMENT, UNCLAMPED AND UNSCALED. For the profiler and for
     * diagnosing a hitch — this is the only place the true spike survives, and
     * a frame-time graph drawn from `delta` would flatten exactly the events it
     * exists to show. Do not integrate against it. */
    virtual float rawDelta() const = 0;

    /* The ceiling applied above. Its default should be a few frames' worth
     * rather than a whole second: the point is that a hitch produces a slow
     * frame, not a teleport. */
    virtual float maxDelta() const = 0;
    virtual void  setMaxDelta(float seconds) = 0;

    /* True when the last tick was clamped — a hitch happened. Worth logging,
     * and worth suppressing a frame's worth of input-driven movement over. */
    virtual bool hitched() const = 0;

    /* ---- pause and scale ------------------------------------------------*/
    virtual void  setTimeScale(float scale) = 0;
    virtual float timeScale() const = 0;
    virtual void  setPaused(bool paused) = 0;
    virtual bool  paused() const = 0;

    /* ---- absolute time --------------------------------------------------*/

    /* MONOTONIC seconds since the clock started. Never goes backwards, unlike a
     * system clock an NTP correction can wind back mid-session. */
    virtual double elapsed() const = 0;

    /* Ticks since start, for anything that wants to do something every Nth
     * frame without keeping its own counter. */
    virtual uint64_t frameCount() const = 0;

    /* A smoothed frames-per-second, for display. Smoothed because an
     * instantaneous 1/delta jitters too much to read. */
    virtual float framesPerSecond() const = 0;

    /* ---- resuming -------------------------------------------------------
     *
     * DISCARD THE NEXT DELTA ENTIRELY. For the moments where time genuinely did
     * pass but nothing should act on it: returning from a console suspend, from
     * a modal window drag, from a breakpoint, or after a long level load. The
     * clamp would turn a thirty-second gap into one slow frame; this turns it
     * into no frame at all, which is what those cases actually want. */
    virtual void skipNextDelta() = 0;
};

}  // namespace cromwell
