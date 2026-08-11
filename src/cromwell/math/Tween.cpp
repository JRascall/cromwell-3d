#include "cromwell/math/Tween.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {

Timeline& Timeline::withDuration(float seconds)
{
    duration_ = std::max(seconds, 0.0f);
    return *this;
}

Timeline& Timeline::withDelay(float seconds)
{
    delay_ = std::max(seconds, 0.0f);
    return *this;
}

void Timeline::start()
{
    delayRemaining_ = delay_;
    runFromStart();
}

void Timeline::restart()
{
    /* No delay on a repeat — see withDelay. */
    delayRemaining_ = 0.0f;
    runFromStart();
}

void Timeline::runFromStart()
{
    progress_ = 0.0f;
    reversing_ = false;
    started_ = true;

    /* A zero duration is "already there" rather than a division by zero. It
     * still counts as having run, so finished() is true and a caller waiting on
     * it is released on the same frame. */
    running_ = duration_ > 0.0f || delayRemaining_ > 0.0f;
    if (!running_) {
        progress_ = 1.0f;
    }
}

void Timeline::finish()
{
    progress_ = 1.0f;
    delayRemaining_ = 0.0f;
    running_ = false;
    started_ = true;
}

void Timeline::advance(float deltaSeconds)
{
    if (!running_ || deltaSeconds <= 0.0f) {
        return;
    }

    /* The delay eats the frame's time first, and any left over is spent on the
     * animation — so a delay that expires mid-frame does not cost the animation
     * that frame. At 60 Hz it is a sixtieth of a second; over a staggered list
     * of twenty items it is a third of a second of drift. */
    if (delayRemaining_ > 0.0f) {
        const float consumed = std::min(delayRemaining_, deltaSeconds);
        delayRemaining_ -= consumed;
        deltaSeconds -= consumed;
        if (deltaSeconds <= 0.0f) {
            return;
        }
    }

    if (duration_ <= 0.0f) {
        progress_ = 1.0f;
        running_ = false;
        return;
    }

    const float step = deltaSeconds / duration_;

    switch (mode_) {
    case Mode::Once:
        progress_ += step;
        if (progress_ >= 1.0f) {
            progress_ = 1.0f;
            running_ = false;
        }
        break;

    case Mode::Loop:
        progress_ += step;
        /* fmod rather than a subtract, so a frame long enough to span several
         * cycles — a debugger pause, a stalled load — lands in the right place
         * instead of somewhere a single subtraction left it. */
        progress_ = std::fmod(progress_, 1.0f);
        if (progress_ < 0.0f) {
            progress_ += 1.0f;
        }
        break;

    case Mode::PingPong:
        progress_ += reversing_ ? -step : step;
        /* Reflect repeatedly rather than once, for the same long-frame reason.
         * Each reflection flips the direction, so an overrun of two and a half
         * cycles ends up travelling the correct way. */
        while (progress_ > 1.0f || progress_ < 0.0f) {
            if (progress_ > 1.0f) {
                progress_ = 2.0f - progress_;
                reversing_ = true;
            } else {
                progress_ = -progress_;
                reversing_ = false;
            }
        }
        break;
    }
}

void TweenFloat::snapTo(float value)
{
    from_ = value;
    to_ = value;
    value_ = value;
    timeline_.finish();
}

TweenFloat& TweenFloat::withDuration(float seconds)
{
    timeline_.withDuration(seconds);
    return *this;
}

void TweenFloat::moveTo(float target)
{
    /* Already going there — leave the animation alone. See the header: without
     * this, an immediate-mode caller restating its target every frame would
     * hold the tween on the first frame of its curve forever. */
    if (timeline_.running() && target == to_) {
        return;
    }
    if (!timeline_.running() && target == value_) {
        return;
    }

    /* FROM WHERE IT IS, not from where the last move started — that is what
     * stops a retarget mid-flight from jumping backwards. */
    from_ = value_;
    to_ = target;
    timeline_.withCurve(curve_).start();

    if (!timeline_.running()) {
        /* Zero duration: already arrived. */
        value_ = to_;
    }
}

void TweenFloat::advance(float deltaSeconds)
{
    if (!timeline_.running()) {
        return;
    }
    timeline_.advance(deltaSeconds);
    value_ = from_ + (to_ - from_) * timeline_.value();

    /* Pinned exactly on arrival. The curves that overshoot do not return
     * precisely 1 at the end in floating point, and a value that settles at
     * 99.9997% of its target is the kind of thing that shows up much later as a
     * bar that never quite fills. */
    if (!timeline_.running()) {
        value_ = to_;
    }
}

}  // namespace cromwell
