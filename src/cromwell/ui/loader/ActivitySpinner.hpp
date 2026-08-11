/* ActivitySpinner.hpp — the iOS-style activity indicator.
 *
 * SINGLE RESPONSIBILITY: draw a ring of rounded spokes whose highlight advances
 * one spoke at a time, with a fade trailing behind it.
 *
 * THE STEP IS THE WHOLE POINT. The bright spoke JUMPS to the next position
 * rather than sweeping smoothly between them, and that discrete tick is what
 * makes this read as an Apple spinner instead of a generic rotating gradient.
 * Nothing else about the shape matters as much — get the step wrong and no
 * amount of tuning the proportions rescues it.
 *
 * The proportions are Apple's, all derived from the radius: a small centre hole
 * at 42% and chunky spokes at 10%, so neighbouring spokes nearly touch. They
 * scale as one shape, so the spinner is specified by a single number.
 *
 * Each spoke is exact capsule geometry with a one-pixel feather (see
 * ui/shape/Shapes.hpp for why the shader route fails at this size — spokes this
 * small are precisely where it turns to mush, and where the rotated ones come
 * out visibly slimmer than the upright ones).
 *
 * STATELESS, like the loading ring: the step comes from the context's clock.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct ActivitySpinnerSpec {
    /* Outer radius of the spoke ring, screen pixels. */
    float radiusPx = 20.0f;

    /* Seconds for the highlight to travel the full ring. */
    float periodSeconds = 0.8f;

    /* Apple uses 8; the classic grey system spinner uses 12. */
    int spokeCount = 8;

    float glowStrength = theme::kGlowStrength;
    float glowRadiusPx = theme::kGlowRadiusPx;

    /* Overall tint. Each spoke's own position in the tail multiplies its
     * alpha. */
    UiColor colour = UiColor::white();
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. The
 * spoke COUNT is not a dimension: a spinner does not grow spokes on a better
 * monitor, it grows in size. */
inline ActivitySpinnerSpec scaled(const ActivitySpinnerSpec& spec, float factor)
{
    ActivitySpinnerSpec out = spec;
    out.radiusPx *= factor;
    out.glowRadiusPx *= factor;
    return out;
}

void drawActivitySpinner(UiContext& context, Vec2 centre, const ActivitySpinnerSpec& spec);

}  // namespace cromwell::ui
