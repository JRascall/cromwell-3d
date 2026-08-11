/* Tween.hpp — a number moving to another number over a known time, along a
 * named curve.
 *
 * SINGLE RESPONSIBILITY: hold the clock and the endpoints, and hand back where
 * the value is right now.
 *
 * TWO TYPES, AND THEY ARE FOR DIFFERENT QUESTIONS:
 *
 *   Timeline  — "how far through am I", 0 to 1. Drive anything from it: a
 *               position, a colour, three things at once. Use it when one clock
 *               governs several values, which is most sequences.
 *   TweenFloat — "what is this number now". Owns its own clock and its
 *               endpoints. Use it for a single value that is told where to go
 *               and gets there on its own.
 *
 * WHY THIS SITS BESIDE Easing.hpp RATHER THAN IN AN ANIMATION MODULE. It is
 * twenty lines of arithmetic over a float and a clock, with no dependency on
 * anything — not the renderer, not the UI, not a scene. Skeletal animation, when
 * it arrives, is a different thing entirely (a skeleton, a sampler, a blend
 * tree) and putting these there in anticipation would drag every user of a
 * fade into that header.
 *
 * DURATION-BASED, WHICH IS THE TRADE. A curve only means something against a
 * known length, so these are for animations with an end: a panel arriving, a
 * screen fading up, a counter ticking to a new value. For something with no end
 * time that can reverse at any moment — a hover highlight — a constant RATE
 * behaves better, because reversing halfway takes half as long rather than
 * restarting a curve. That is HoverFade and theme::interpConstantTo. Reaching
 * for the wrong one gives either a fade that jumps when interrupted or one that
 * never quite arrives; see the note at the top of Easing.hpp.
 *
 * NOT TEMPLATED, deliberately. A `Tween<T>` needs a lerp for T, which means a
 * customisation point, which means every user of a fade instantiating machinery
 * to move one float. Vectors and colours animate perfectly well by driving one
 * Timeline and lerping the endpoints at the call site — one line, and it is
 * obvious what it does.
 *
 * CONFIGURED FLUENTLY, STARTED SEPARATELY. The shape is `withX()` setters that
 * chain, then a `start()` that takes nothing:
 *
 *     timeline.withDuration(0.3f).withCurve(Ease::CubicOut).start();
 *     timeline.withDelay(0.15f).withMode(Mode::PingPong).start();
 *
 * The alternative was one `start(duration, curve, mode, delay)`, and the call
 * site read `start(0.3f, Ease::CubicOut, Mode::PingPong, 0.15f)` — four
 * unlabelled values where two are floats in different units, which is the exact
 * shape of argument that gets transposed. Naming them at the call site costs
 * nothing and the compiler no longer accepts a delay in the duration slot.
 *
 * THE CONFIGURATION IS STICKY, which is the other half of the win: a timeline
 * keeps its duration and curve across restarts, so a repeatedly-triggered
 * animation is described once at setup and fired with a bare `start()`.
 */
#pragma once

#include "cromwell/math/Easing.hpp"

namespace cromwell {

/* A clock that runs 0 to 1 over a duration, with an optional delay before it
 * starts and an optional repeat. */
class Timeline {
public:
    enum class Mode {
        /* Runs once and stops at 1. */
        Once,

        /* Snaps back to 0 and runs again. A rotation, a pulsing highlight. */
        Loop,

        /* Runs to 1, then back to 0, then again. The curve applies in both
         * directions, so an ease-out arrival is also an ease-out departure —
         * which is what makes a breathing element look symmetric. */
        PingPong,
    };

    /* ---- fluent configuration --------------------------------------------
     * Set before start(), and they persist across it — see the header. Safe to
     * change while running: the next start() picks them up, and the running
     * animation is left alone rather than being retimed underneath itself. */

    /* Zero completes immediately rather than dividing by it, so "animate this
     * over `n` seconds" with n configurable to 0 means "do it now" without a
     * special case at the call site. Negative is clamped to zero. */
    Timeline& withDuration(float seconds);

    Timeline& withCurve(Ease curve) { curve_ = curve; return *this; }
    Timeline& withMode(Mode mode) { mode_ = mode; return *this; }

    /* Dead time before the clock moves. Applied by start(), skipped by
     * restart() — a stagger should delay the entrance, not every repeat. */
    Timeline& withDelay(float seconds);

    /* Runs it from the beginning with whatever is configured. */
    void start();

    void advance(float deltaSeconds);

    /* Back to the start, still running, and without re-serving the delay. */
    void restart();

    /* Straight to the end and stopped — for skipping an animation without
     * having to know how long it had left. */
    void finish();

    /* Stops where it is. */
    void stop() { running_ = false; }

    /* Raw 0..1, before the curve. What a caller wants when it is applying its
     * own shaping, or reading the progress as a number. */
    float progress() const { return progress_; }

    /* The eased 0..1 — what almost everything wants. */
    float value() const { return ease(curve_, progress_); }

    /* True from start() until a Once timeline reaches its end. Loops and
     * ping-pongs never stop on their own. */
    bool running() const { return running_; }

    /* True once a Once timeline has arrived. Always false for the repeating
     * modes, which have no end to arrive at. */
    bool finished() const { return !running_ && started_; }

    /* True while the start delay is still counting down — the timeline is
     * running but has not begun to move. */
    bool waiting() const { return delayRemaining_ > 0.0f; }

    float duration() const { return duration_; }
    Ease  curve() const { return curve_; }
    Mode  mode() const { return mode_; }
    float delay() const { return delay_; }

private:
    float duration_ = 0.0f;

    /* The configured delay, and how much of it is left. Two fields because the
     * configuration is sticky and the countdown is not — restart() has to be
     * able to skip the wait without forgetting it. */
    float delay_ = 0.0f;
    float delayRemaining_ = 0.0f;

    float progress_ = 0.0f;
    Ease  curve_ = Ease::Linear;
    Mode  mode_ = Mode::Once;
    bool  running_ = false;
    bool  started_ = false;

    /* PingPong only: which way the clock is currently travelling. */
    bool  reversing_ = false;

    /* Everything start() and restart() share; they differ only in what they do
     * with the delay. */
    void runFromStart();
};

/* A float that is told where to go and gets there on its own. */
class TweenFloat {
public:
    TweenFloat() = default;
    explicit TweenFloat(float initial) : from_(initial), to_(initial), value_(initial) {}

    /* ---- fluent configuration --------------------------------------------
     * How the value travels, set once at setup and reused by every moveTo.
     * That split is what makes the immediate-mode case below read well: the
     * per-frame call says only where to go, because how fast is not a per-frame
     * decision. Pass a different duration mid-life by chaining it onto the
     * moveTo that needs it. */

    /* Defaults to zero, which is an instant assignment — the same "duration 0
     * means do it now" rule Timeline documents. A tween that animates is one
     * that was told how long to take. */
    TweenFloat& withDuration(float seconds);
    TweenFloat& withCurve(Ease curve) { curve_ = curve; return *this; }

    /* Jumps, with no animation — for initialising, and for a value that has
     * been changed by something the player did not watch happen. */
    void snapTo(float value);

    /* Animates to `target` over the configured duration and curve.
     *
     * CALLING THIS EVERY FRAME WITH THE SAME TARGET IS SAFE, and that matters
     * more than it sounds: an immediate-mode caller has nowhere natural to put
     * "only when it changes", so a version that restarted unconditionally would
     * be pinned at the first frame of its curve forever and never move. A
     * repeat of the current target is ignored.
     *
     * RETARGETING MID-FLIGHT starts a new move FROM WHERE THE VALUE IS, not
     * from where the last one began, so the value never jumps. The curve
     * restarts, which is the honest trade: a value retargeted every few frames
     * is permanently in the first part of its curve, so for something that
     * changes that often a constant rate is the better model. */
    void moveTo(float target);

    void advance(float deltaSeconds);

    float value() const { return value_; }
    float target() const { return to_; }
    bool  moving() const { return timeline_.running(); }
    float duration() const { return timeline_.duration(); }
    Ease  curve() const { return curve_; }

private:
    float    from_ = 0.0f;
    float    to_ = 0.0f;
    float    value_ = 0.0f;
    Ease     curve_ = Ease::CubicOut;
    Timeline timeline_;
};

}  // namespace cromwell
