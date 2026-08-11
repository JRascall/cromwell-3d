/* LoadingBar.hpp — pill-shaped progress bar.
 *
 * SINGLE RESPONSIBILITY: draw a thin track with a fill that GLIDES toward the
 * bound value rather than jumping to it.
 *
 * THE GLIDE IS THE FEATURE. Real progress arrives in lumps — a loader finishes
 * three files at once and the number leaps from 20% to 55% — and a bar that
 * teleports makes the lumps the most noticeable thing on screen. Moving at a
 * constant rate toward whatever it was last told smooths that out without ever
 * lying about the destination: `fillAnimationSeconds` is the time for a full
 * 0-to-1 traverse, so a small jump takes proportionally less time and the bar
 * always catches up. Zero snaps, for a bar driven by something already smooth.
 *
 * Constant rate rather than exponential, for the reason theme::interpConstantTo
 * spells out: an exponential approach never actually arrives, and a bar stuck
 * at 99.6% for the rest of the load is worse than one that jumps.
 *
 * STATEFUL, therefore, and the only loader that is. It needs an id so the
 * glide's position survives between frames; see the note on ids in
 * UiContext.hpp.
 *
 * THE FILL NEVER GETS NARROWER THAN ITS OWN HEIGHT when the ends are rounded,
 * so at 1% the leading cap is still a pill rather than a sliver of a lens.
 * Square ends track the true value all the way down, where that concern does
 * not apply.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct LoadingBarSpec {
    /* Fill amount, 0..1. */
    float progress = 0.0f;

    /* Pill caps on track and fill, or square ends. */
    bool roundedEnds = true;

    /* Bar thickness in screen pixels. The bar fills the width it is given and
     * sits centred vertically in it, so a caller can hand it a whole row. */
    float barHeightPx = 5.0f;

    /* Seconds for the fill to glide across the whole bar. 0 snaps. */
    float fillAnimationSeconds = 0.25f;

    float glowStrength = theme::kGlowStrength;
    float glowRadiusPx = theme::kGlowRadiusPx;

    UiColor fillColour = UiColor::white();
    UiColor trackColour = theme::track();
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. The
 * glide TIME is not one: a fill crossing the bar in a quarter of a second
 * should still take a quarter of a second when the bar is twice as wide. */
inline LoadingBarSpec scaled(const LoadingBarSpec& spec, float factor)
{
    LoadingBarSpec out = spec;
    out.barHeightPx *= factor;
    out.glowRadiusPx *= factor;
    return out;
}

void drawLoadingBar(UiContext& context, UiId id, const UiRect& bounds, const LoadingBarSpec& spec);

}  // namespace cromwell::ui
