/* FrameClock.hpp — the frame clock, once, for every platform.
 *
 * SINGLE RESPONSIBILITY: implement IClock — measure the frame, clamp it, scale
 * it, and be the one place a suspicious delta is caught.
 *
 * ================== WHY THIS IS NOT A PER-BACKEND CLASS ===================
 *
 * Because almost none of it is platform-specific, and the part that is has a
 * portable answer.
 *
 * Everything a clock does that MATTERS — clamping a hitch, discarding a resume
 * gap, separating game time from real time, smoothing a frame rate for display
 * — is arithmetic over a number of seconds. Writing that per backend would mean
 * four copies of the clamp, and the whole reason IClock exists is that a
 * missing clamp is a unit teleporting through a wall. Four copies is four
 * chances to leave it out, in the exact place a port is least tested.
 *
 * And the one genuinely platform question — "what time is it" — is answered
 * portably by std::chrono::steady_clock, which is monotonic by definition and
 * available on every target here including the consoles. raylib's GetTime()
 * offers nothing over it.
 *
 * SO THERE IS NO RaylibClock, and there should never be a console one either. A
 * platform that needs its own time source (a fixed-tick certification harness,
 * a replay driving time from a file) supplies one through `setTimeSource`
 * rather than reimplementing the class.
 *
 * ============= WHY THE HITCH RULES ARE WORTH THIS MUCH TROUBLE ============
 *
 * The failures this prevents all look like other bugs, which is what makes
 * them expensive:
 *
 *   - A window dragged by its title bar blocks inside the OS modal loop. The
 *     next delta is however long the player held it — often seconds. Movement
 *     integrated over that passes through walls, and it gets reported as a
 *     collision bug.
 *   - A console suspended to the system menu and resumed produces a delta of
 *     minutes. Clamping alone still yields one full-length frame of everything
 *     happening at once, so resume wants skipNextDelta and not the clamp.
 *   - A breakpoint stepped over does the same thing, which is why it usually
 *     gets discovered while debugging something else entirely.
 *
 * The raw measurement survives on rawDelta() so a profiler still sees the
 * spike — a frame-time graph drawn from the clamped value would flatten exactly
 * the events it exists to show.
 */
#pragma once

#include "cromwell/platform/IClock.hpp"

#include <functional>

namespace cromwell {

class FrameClock final : public IClock {
public:
    FrameClock();

    /* WHERE SECONDS COME FROM. Defaults to std::chrono::steady_clock, which is
     * what every real platform should use. Replaceable so a test can drive time
     * deterministically and a replay can drive it from a file — both of which
     * want the clamp and the scaling to behave exactly as they do in the game,
     * which is only true if they run through this same class.
     *
     * Must be monotonic. A source that can go backwards produces a negative
     * delta, which is clamped to zero here rather than trusted. */
    using TimeSource = std::function<double()>;
    void setTimeSource(TimeSource source);

    void tick() override;

    float delta() const override;
    float unscaledDelta() const override { return clamped_; }
    float rawDelta() const override { return raw_; }

    float maxDelta() const override { return maxDelta_; }
    void  setMaxDelta(float seconds) override;
    bool  hitched() const override { return hitched_; }

    void  setTimeScale(float scale) override;
    float timeScale() const override { return timeScale_; }
    void  setPaused(bool paused) override { paused_ = paused; }
    bool  paused() const override { return paused_; }

    double   elapsed() const override { return elapsed_; }
    uint64_t frameCount() const override { return frames_; }
    float    framesPerSecond() const override { return smoothedFps_; }

    void skipNextDelta() override { skipNext_ = true; }

private:
    TimeSource source_;
    double     lastSample_ = 0.0;
    bool       started_ = false;

    float raw_     = 0.0f;
    float clamped_ = 0.0f;

    /* A TENTH OF A SECOND, not a whole one. The point of the ceiling is that a
     * hitch becomes a SLOW FRAME rather than a teleport, and a one-second
     * clamp still lets a unit cross a room between two frames. Ten frames'
     * worth at 100 Hz is enough that an ordinary stutter passes through
     * untouched and a stall does not. */
    float maxDelta_ = 0.1f;

    float timeScale_ = 1.0f;
    bool  paused_    = false;
    bool  hitched_   = false;
    bool  skipNext_  = false;

    double   elapsed_ = 0.0;
    uint64_t frames_  = 0;

    /* Exponentially smoothed, because an instantaneous 1/delta jitters far too
     * much to read off a HUD. */
    float smoothedFps_ = 0.0f;
};

}  // namespace cromwell
