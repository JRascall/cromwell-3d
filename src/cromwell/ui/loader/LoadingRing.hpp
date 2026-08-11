/* LoadingRing.hpp — arc spinner and progress ring.
 *
 * SINGLE RESPONSIBILITY: draw a dim full-circle track with a bright arc on it,
 * animated one of three ways.
 *
 * A fixed-length arc circling at a constant rate (Spin) is the indeterminate
 * spinner; an arc growing from empty to full and restarting (Fill) is the
 * "something is happening and we cannot say how much longer" loop; an arc
 * showing a bound value (Progress) is a determinate ring.
 *
 * Everything is exact vertex geometry with a one-pixel feather — see
 * ui/shape/Shapes.hpp for why the alternative does not survive contact with
 * small UI — so the track and the arc stay crisp and uniformly thick at every
 * angle, and the rounded ends are real half-discs rather than a shader's
 * approximation of them.
 *
 * STATELESS. It reads the context's clock and derives everything else from its
 * spec, so it needs no id and no per-widget state. Two rings with the same
 * period are in phase, which is what a row of them should look like.
 *
 * ANGLES ANCHOR AT 12 O'CLOCK AND TURN CLOCKWISE, in screen space with y down.
 * Every rotating thing in the kit agrees on that; a spinner that ran
 * anticlockwise next to one that did not would look like a bug even to someone
 * who could not say why.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

namespace cromwell::ui {

enum class LoadingRingStyle {
    /* A fixed-length arc endlessly circling the track — the classic
     * indeterminate spinner, with a constant gap behind it. */
    Spin,

    /* The arc grows from empty to a full circle over one period, then restarts
     * from empty. */
    Fill,

    /* The arc length shows the spec's progress value. */
    Progress,
};

/* ONE-SHOT DATA CARRIER (see the encapsulation note in UiColor.hpp): built by
 * the caller, read by one draw call, gone at the end of the statement. */
struct LoadingRingSpec {
    LoadingRingStyle style = LoadingRingStyle::Spin;

    /* Outer radius and band thickness, in screen pixels. */
    float radiusPx = 16.0f;
    float thicknessPx = 4.0f;

    /* Seconds for one revolution (Spin) or one empty-to-full loop (Fill). */
    float periodSeconds = 1.0f;

    /* Arc length — Spin only. Anything at or above 360 closes the ring, which
     * makes the caps meaningless, so they are dropped. */
    float sweepDegrees = 270.0f;

    /* Fill amount 0..1 — Progress only. */
    float progress = 0.0f;

    float glowStrength = theme::kGlowStrength;
    float glowRadiusPx = theme::kGlowRadiusPx;

    UiColor arcColour = UiColor::white();
    UiColor trackColour = theme::track();
};

/* Every pixel dimension multiplied by `factor`, for the display-scale contract
 * in UiContext.hpp. Times, angles, the progress value and the colours are
 * deliberately untouched — a ring does not spin faster on a better monitor.
 *
 * These live beside their spec rather than in one central file so that adding a
 * field and forgetting to scale it is visible in the same screenful of code. */
inline LoadingRingSpec scaled(const LoadingRingSpec& spec, float factor)
{
    LoadingRingSpec out = spec;
    out.radiusPx *= factor;
    out.thicknessPx *= factor;
    out.glowRadiusPx *= factor;
    return out;
}

/* Draws the ring centred on `centre`. The spec's radius is the OUTER radius, so
 * the shape occupies a box of 2 * radiusPx on a side. */
void drawLoadingRing(UiContext& context, Vec2 centre, const LoadingRingSpec& spec);

}  // namespace cromwell::ui
