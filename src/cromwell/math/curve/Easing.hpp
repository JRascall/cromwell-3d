/* Easing.hpp — the shapes a value can take on its way from one number to
 * another.
 *
 * SINGLE RESPONSIBILITY: map a 0..1 progress to a 0..1 eased progress. Pure
 * functions, no state, no clock, no opinion about what is being animated.
 *
 * WHY THIS IS WORTH A FILE. Linear motion is the one thing nothing in the
 * physical world does, and UI that moves linearly reads as cheap in a way
 * people notice without being able to name. The fix is a curve, and the curves
 * that work are a small, well-known set that everyone reinvents badly — usually
 * as an ad-hoc `t*t` at one call site and a `1-(1-t)*(1-t)` at another, with no
 * way to compare them or swap one for the other.
 *
 * NAMED, NOT SPELLED OUT, so that "how does this arrive" is a value that can be
 * stored in a spec, tuned in a dev panel, and read back in a log. That is the
 * whole reason for the enum: a curve you can pass around is a curve you can
 * change without touching the thing being animated.
 *
 * IN, OUT, IN-OUT — the convention, since it is the one thing people get
 * backwards:
 *   - IN eases the START. It begins slowly and finishes fast. Use it for
 *     something LEAVING: an element accelerating off screen.
 *   - OUT eases the END. It begins fast and settles gently. Use it for
 *     something ARRIVING — which is most UI, because the eye wants to see the
 *     thing appear immediately and then come to rest.
 *   - IN-OUT eases both. Use it for a move BETWEEN two resting states, where
 *     there is no arrival or departure, only travel.
 *
 * WHEN NOT TO USE ANY OF THIS. A curve needs a known duration to be a curve at
 * all — it maps "how far through the animation" to "how far along". For
 * something interruptible with no end time, like a hover highlight that can
 * reverse halfway, a constant RATE is the right model instead, because it
 * behaves sensibly when the target changes mid-flight. That is what
 * HoverFade and theme::interpConstantTo are for, and mixing them up produces
 * either a fade that snaps when reversed or one that never finishes. See
 * Tween.hpp, which pairs a duration with a curve for the case where the
 * duration IS known.
 *
 * NOTE ON theme::easeInOut. The UI kit carries a separate ease-in-out taking a
 * continuous EXPONENT rather than a named curve, because the widgets were
 * ported from a codebase using Unreal's convention and their specs are tuned in
 * those numbers. Exponent 2 is exactly Ease::QuadInOut, exponent 3 is
 * Ease::CubicInOut. Both exist on purpose; use this one for anything new.
 */
#pragma once

namespace cromwell {

enum class Ease {
    /* No curve. Correct for a genuinely uniform process — a spinner's rotation,
     * a scrolling credits list — and wrong for almost everything else. */
    Linear,

    /* The workhorse pair. Gentle at both ends, no overshoot, nothing to think
     * about. SmootherStep has zero curvature as well as zero slope at the ends,
     * so it is the one to reach for when a linear-looking seam is visible where
     * the motion starts. */
    SmoothStep,
    SmootherStep,

    /* Polynomial families, mildest first. Quad is a suggestion of a curve;
     * Cubic is the default for UI that should feel deliberate; Quint is
     * dramatic and starts to read as slow. */
    QuadIn, QuadOut, QuadInOut,
    CubicIn, CubicOut, CubicInOut,
    QuintIn, QuintOut, QuintInOut,

    /* Sine is the softest useful curve — barely eased, but never linear. Good
     * for something large moving a short distance. */
    SineIn, SineOut, SineInOut,

    /* Exponential is the most aggressive of the ordinary curves. ExpoOut is the
     * "snap into place" arrival: most of the distance is covered almost
     * immediately and the rest settles. */
    ExpoIn, ExpoOut, ExpoInOut,

    /* Circular. Sharper than Quad at the eased end and rounder than Expo —
     * useful when Expo reads as too abrupt but Cubic as too soft. */
    CircIn, CircOut, CircInOut,

    /* OVERSHOOTS ITS TARGET and comes back. This is what makes something feel
     * physical rather than driven, and it is the single most effective curve
     * for a panel or badge appearing. It leaves the 0..1 range, so anything
     * clamping its own output — an alpha, a progress bar — will flatten the
     * overshoot and lose the effect. */
    BackIn, BackOut, BackInOut,

    /* Overshoots repeatedly, decaying. Playful, and tiring quickly on anything
     * the player sees often. */
    ElasticOut,

    /* Settles by bouncing, decaying. Same warning as Elastic. */
    BounceOut,
};

/* The curve applied to `t`, which is CLAMPED to 0..1 first — so a caller that
 * has not normalised its clock gets the endpoint rather than an extrapolation
 * off the end of the curve.
 *
 * The RESULT is not clamped: Back and Elastic deliberately leave the range, and
 * flattening them here would silently remove the only reason to choose them. */
float ease(Ease curve, float t);

/* `a` to `b` along the curve. The form most call sites actually want, and worth
 * having so that `a + (b - a) * ease(...)` is not written out at each one. */
inline float easeBetween(Ease curve, float a, float b, float t)
{
    return a + (b - a) * ease(curve, t);
}

/* The curve's name, for logs and dev panels. Never localise or reformat these —
 * they are how a curve is identified in a settings file. */
const char* toString(Ease curve);

}  // namespace cromwell
