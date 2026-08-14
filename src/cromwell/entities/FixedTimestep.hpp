/* FixedTimestep.hpp — the clock the simulation runs on, decoupled from the
 * clock the screen runs on.
 *
 * SINGLE RESPONSIBILITY: turn "this much real time passed" into "run this many
 * simulation steps, and you are this far into the next one". It runs nothing
 * and owns nothing else.
 *
 * WHY A FIXED STEP AT ALL. Feed real frame time into the simulation and the
 * simulation becomes a function of the frame rate: a machine drawing at 144 Hz
 * integrates movement in smaller slices than one at 60, gets slightly different
 * answers, and diverges. That costs three things — replays and saves that
 * cannot reproduce, shared-simulation multiplayer that cannot exist at all, and
 * bugs that are not repeatable, which is the one that hurts daily. A step of
 * fixed SIZE, run however many times the elapsed real time is worth, gives the
 * same answer everywhere.
 *
 * THE FAILURE THIS AVOIDS IS NOT THE OBVIOUS ONE. Bethesda's engine steps
 * physics once per frame with a fixed amount of time per step, so at 120 fps it
 * simulates twice as much game per second and items fly off shelves. They had
 * the fixed size and not the accumulator. **The count of steps must come from
 * measured elapsed time, never from the count of frames** — which is the whole
 * job of this class.
 *
 * SPEED CONTROL IS THE CALLER'S, AND IT SCALES THE INPUT, NOT THE STEP. Pause
 * by passing 0, run at double speed by passing twice the elapsed time, at half
 * by passing half. The step keeps its size and only the number of them changes,
 * so a save made while paused, fast-forwarded or slowed replays identically.
 * Changing the step SIZE instead would also change speed — and would change the
 * answers, because integrating over a bigger slice is not the same arithmetic.
 * That is why withRate() is a per-project decision made once at startup and not
 * a speed control. (OpenRA does drive its game speeds by resizing the step,
 * which is why its speeds are not interchangeable mid-match; see
 * study/games/strategy/openra.md §1.)
 *
 * THE RATE IS THE GAME'S CHOICE AND THE ENGINE MUST NOT ASSUME ONE. A lockstep
 * RTS wants 10-25 Hz because it is networking orders and unit movement needs no
 * more; a shooter wants 60-128 because player movement and hit registration do;
 * a turn-based game barely cares because its simulation is event-driven and the
 * continuous part is animation, which is presentation and runs per frame.
 * Baking a number in here would serve one genre and be wrong for the other two.
 *
 * CATCH-UP IS CAPPED, and the cap is not optional. If a frame takes far longer
 * than a step — a load, a breakpoint, a dragged window — the accumulator holds
 * enough time to demand hundreds of steps, each of which makes the next frame
 * later still. Past the cap the surplus is DISCARDED: the simulation loses that
 * time rather than trying to relive it. The game briefly runs slow, which is
 * survivable; a spiral into the swap file is not.
 */
#pragma once

namespace cromwell {

class FixedTimestep {
public:
    /* ---- configuration, once, at startup -------------------------------- */

    /* Steps per second of simulated time. See the header note on why this is a
     * per-project constant rather than a speed control. Non-positive rates are
     * refused rather than stored — a zero rate would divide by zero on the
     * first advance, and that is a crash a long way from the mistake. */
    FixedTimestep& withRate(int stepsPerSecond)
    {
        if (stepsPerSecond > 0) {
            rate_ = stepsPerSecond;
            stepSeconds_ = 1.0f / static_cast<float>(stepsPerSecond);
        }
        return *this;
    }

    /* How much simulated time may be owed before the surplus is thrown away.
     * The default is a quarter second, which is OpenRA's number and is roughly
     * where "the game hitched" turns into "the game is broken". */
    FixedTimestep& withMaxCatchUp(float seconds)
    {
        if (seconds > 0.0f) maxCatchUp_ = seconds;
        return *this;
    }

    int   rate() const { return rate_; }
    float stepSeconds() const { return stepSeconds_; }
    float maxCatchUp() const { return maxCatchUp_; }

    /* ---- the frame ------------------------------------------------------ */

    /* Bank the elapsed time and report how many steps are owed. Not a chaining
     * setter: the count is the whole point of the call, so it is returned
     * rather than thrown away (see CLAUDE.md on when not to chain).
     *
     * `elapsedSeconds` is REAL time already scaled by whatever speed control
     * the caller has — see the header note. Negative values are ignored rather
     * than banked, because a clock that went backwards is a bug elsewhere and
     * running the simulation backwards is not the repair. */
    int advance(float elapsedSeconds)
    {
        if (elapsedSeconds > 0.0f) accumulator_ += elapsedSeconds;

        /* Clamp before counting, so the discarded surplus never becomes steps.
         * Clamping after would have already committed to running them. */
        if (accumulator_ > maxCatchUp_) accumulator_ = maxCatchUp_;

        int steps = 0;
        while (accumulator_ >= stepSeconds_) {
            accumulator_ -= stepSeconds_;
            steps++;
        }
        return steps;
    }

    /* How far into the NEXT step we are, 0 to 1. This is what presentation
     * blends with: at 30 steps a second and 144 frames, five frames in a row
     * would otherwise draw the same positions and the sixth would jump, so
     * anything drawn from simulated state shows a point this far between its
     * previous and current values.
     *
     * Only meaningful after advance(). Nothing is forced to use it — a game
     * whose visible motion comes from an animator rather than from simulated
     * positions has nothing to blend, which is the turn-based case. */
    float blend() const { return accumulator_ / stepSeconds_; }

    /* Drop the banked time. For a load, a level change, or anything else after
     * which the elapsed wall time says nothing about how much game should have
     * happened. */
    void reset() { accumulator_ = 0.0f; }

private:
    int   rate_ = 60;
    float stepSeconds_ = 1.0f / 60.0f;
    float maxCatchUp_ = 0.25f;
    float accumulator_ = 0.0f;
};

}  // namespace cromwell
