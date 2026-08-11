/* CaptureSchedule.hpp — how often a second camera actually has to draw.
 *
 * SINGLE RESPONSIBILITY: answer "should this capture render on this frame".
 * It owns no target, no camera and no GL — which is why it is header-only and
 * testable, and why the awkward rule below can be pinned down rather than
 * hoped for.
 *
 * ============== THIS IS THE MOST IMPORTANT PART OF SCENE CAPTURE ===========
 *
 * A SECOND CAMERA IS A SECOND SCENE PASS, and the naive version of that costs
 * you the frame twice. Unreal's USceneCaptureComponent2D defaults
 * bCaptureEveryFrame to true and it is the single most common cause of "why did
 * adding a minimap halve my framerate" — the capture is cheap to ADD and
 * expensive to RUN, and nothing about the API says so.
 *
 * So the schedule is a required, visible part of setting one up rather than a
 * flag hidden behind a default:
 *
 *   EVERY FRAME   — a mirror, a scope, a security feed with something moving in
 *                   it. Genuinely costs a second pass. Choose it deliberately.
 *   INTERVAL      — a minimap. Nothing on a minimap moves fast enough to need
 *                   sixty updates a second; four or five is indistinguishable
 *                   and costs a fifteenth as much.
 *   ON DEMAND     — a photo. A loading screen's backdrop, a saved-game
 *                   thumbnail, a planning-mode overview. Renders when asked and
 *                   never again.
 *
 * ==================== THE FIRST CAPTURE ALWAYS HAPPENS =====================
 *
 * AN ON-DEMAND CAPTURE THAT HAS NEVER RUN IS A BLACK RECTANGLE, and that reads
 * as "scene capture is broken" rather than as "you have not asked for one yet".
 * So the first tick always returns true whatever the mode, and every mode is
 * therefore showing something real from the frame it appears.
 *
 * It matters most for the interval mode, where the alternative is a minimap
 * that is black for the first fifth of a second every time it opens — brief,
 * ugly, and exactly the sort of thing that gets blamed on the renderer.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace cromwell {

class CaptureSchedule {
public:
    enum class Mode { EveryFrame, Interval, OnDemand };

    /* A second full scene pass, every frame. See the header before choosing
     * this. */
    static CaptureSchedule everyFrame() { return CaptureSchedule{ Mode::EveryFrame, 0.0f }; }

    /* At most once per `seconds`. The right answer for a minimap; 0.2 (five a
     * second) is imperceptible on one and costs a twelfth of every-frame.
     *
     * `phaseSeconds` OFFSETS THE TIMER, AND MATTERS THE MOMENT THERE ARE TWO
     * CAPTURES. Four captures on a 0.2 s interval, all started on the same
     * frame, all fire on the same frame forever after — so four frames in five
     * are free and the fifth carries four extra scene passes. The average is
     * fine and the frame time has a spike in it every fifth of a second, which
     * is exactly the kind of hitch that gets blamed on the garbage collector in
     * languages that have one.
     *
     * Spread them: `interval(0.2f, 0.0f)`, `interval(0.2f, 0.05f)`,
     * `interval(0.2f, 0.10f)`, `interval(0.2f, 0.15f)` and at most one runs on
     * any frame. The phase only shifts WHEN, never how often. */
    static CaptureSchedule interval(float seconds, float phaseSeconds = 0.0f)
    {
        CaptureSchedule schedule{ Mode::Interval, std::max(seconds, 0.0f) };

        /* WRAPPED INTO THE INTERVAL. An oversized phase would otherwise start
         * the timer already past its threshold, so the schedule would fire on
         * every frame — the exact opposite of what staggering is for. */
        const float phase = std::max(phaseSeconds, 0.0f);
        schedule.phase_ = schedule.interval_ > 0.0f
                              ? std::fmod(phase, schedule.interval_)
                              : 0.0f;
        return schedule;
    }

    /* Only when asked — plus the first frame, so it is never a black hole. */
    static CaptureSchedule onDemand() { return CaptureSchedule{ Mode::OnDemand, 0.0f }; }

    Mode mode() const { return mode_; }
    float intervalSeconds() const { return interval_; }

    /* Re-phases an existing schedule. For a collection that spreads its members
     * out automatically — the caller stated a rate, and something else decides
     * where in the cycle this one sits. See camera/CameraSet.hpp. Wrapped into the
     * interval for the same reason the constructor wraps it. */
    void setPhase(float phaseSeconds)
    {
        phase_ = interval_ > 0.0f ? std::fmod(std::max(phaseSeconds, 0.0f), interval_) : 0.0f;
    }
    float phase() const { return phase_; }

    /* Forces the next tick to capture, whatever the mode. The escape hatch for
     * "something changed and the picture is now wrong" — a wall was demolished,
     * the squad teleported — which no timer can know about. */
    void request() { requested_ = true; }

    /* True on the frames that should render. Call once per frame, and only
     * once: it advances the timer.
     *
     * ORDERED SO THAT AN EXPLICIT REQUEST ALWAYS WINS. A request arriving
     * halfway through an interval must not be swallowed by the timer, or
     * "redraw now, the world changed" would silently mean "redraw within the
     * next fifth of a second", which is the same bug as no request at all on
     * the frame it matters. */
    bool tick(float deltaSeconds)
    {
        if (!captured_) {
            captured_ = true;
            requested_ = false;

            /* THE PHASE IS APPLIED HERE, NOT AT CONSTRUCTION, and that is the
             * whole of the staggering. Every schedule takes its free first
             * capture on the same frame — unavoidable, and harmless once — so
             * resetting the timer to zero here would leave them all in step
             * forever and the stagger would silently do nothing. Starting the
             * timer AT the phase is what pushes them apart from the second
             * capture onward. */
            sinceCapture_ = phase_;
            return true;
        }

        if (requested_) {
            requested_ = false;
            sinceCapture_ = 0.0f;
            return true;
        }

        if (mode_ == Mode::EveryFrame) {
            sinceCapture_ = 0.0f;
            return true;
        }

        if (mode_ == Mode::OnDemand) return false;

        sinceCapture_ += deltaSeconds;
        if (sinceCapture_ < interval_) return false;

        /* Reset rather than subtract the interval. Subtracting would let a
         * frame spike bank credit and fire several captures in a row to "catch
         * up" — which is the worst possible response to already being late. */
        sinceCapture_ = 0.0f;
        return true;
    }

    /* False until the first capture has happened. For a caller deciding whether
     * its texture holds anything worth showing. */
    bool everCaptured() const { return captured_; }

    /* Forgets that anything was captured, so the next tick renders. For a
     * target that was just resized or recreated and whose contents are now
     * meaningless. */
    void invalidate()
    {
        captured_ = false;
        sinceCapture_ = 0.0f;
        /* phase_ survives: the schedule is being made to redraw, not being
         * re-phased, and losing the offset here would quietly put it back in
         * step with its neighbours after every resize. */
    }

private:
    CaptureSchedule(Mode mode, float interval) : interval_(interval), mode_(mode) {}

    float interval_ = 0.0f;
    float sinceCapture_ = 0.0f;

    /* Where the timer starts after the first capture, so several schedules on
     * the same interval do not stay in step. See tick(). */
    float phase_ = 0.0f;
    Mode  mode_ = Mode::EveryFrame;
    bool  requested_ = false;
    bool  captured_ = false;
};

}  // namespace cromwell
