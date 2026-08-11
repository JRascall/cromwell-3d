/* HoverFade.hpp — a highlight that arrives and leaves over time.
 *
 * SINGLE RESPONSIBILITY: hold one 0..1 blend and move it toward whatever the
 * caller says the current state is, at the authored rate.
 *
 * WHY A CLOCK RATHER THAN A DELTA. advance() takes the current TIME, not the
 * frame's elapsed seconds, and works out the delta itself. That makes it
 * idempotent within a frame: a compound control whose label, plate and outline
 * each ask for the same fade gets one advance and three identical answers,
 * rather than three advances and a highlight that arrives three times too fast.
 * Slate's polled colour attributes hit exactly this and the PO widgets solved it
 * the same way; the immediate-mode kit hits it whenever a widget reads its own
 * fade from more than one place, which most of them do.
 *
 * CONSTANT RATE, NOT EXPONENTIAL. See theme::interpConstantTo — a fade that
 * approaches asymptotically never reaches full accent, so a "highlighted"
 * control sits permanently at 99% of the colour it was meant to be.
 *
 * A fade time of zero snaps, which is the documented way to turn the animation
 * off without a second flag.
 */
#pragma once

#include "cromwell/ui/core/UiTheme.hpp"

namespace cromwell::ui {

class HoverFade {
public:
    /* Moves toward highlighted (1) or idle (0) and returns the new alpha.
     * `now` is the context's monotonic clock in seconds. */
    float advance(bool highlighted, float fadeInSeconds, float fadeOutSeconds, double now)
    {
        /* First call establishes the clock without advancing — otherwise a
         * widget created mid-session sees a delta of "seconds since the process
         * started" and snaps to its target on the frame it appears. */
        const float deltaSeconds = (lastTime_ > 0.0 && now > lastTime_)
            ? static_cast<float>(now - lastTime_)
            : 0.0f;
        lastTime_ = now;

        const float target = highlighted ? 1.0f : 0.0f;
        const float fadeTime = highlighted ? fadeInSeconds : fadeOutSeconds;
        alpha_ = fadeTime <= 0.0f
            ? target
            : theme::interpConstantTo(alpha_, target, deltaSeconds, 1.0f / fadeTime);
        return alpha_;
    }

    float alpha() const { return alpha_; }

    /* Jump straight to a state, for a control that is being shown already
     * highlighted and should not animate into it. */
    void snapTo(float alpha) { alpha_ = alpha; }

private:
    float  alpha_ = 0.0f;
    double lastTime_ = 0.0;
};

}  // namespace cromwell::ui
