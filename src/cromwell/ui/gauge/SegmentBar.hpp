/* SegmentBar.hpp — slanted segmented value bar.
 *
 * SINGLE RESPONSIBILITY: draw a row of parallelogram chips, the first `value`
 * of them solid and the rest hollow, and report what the cursor is pointing at.
 *
 * "2 of 6" reads at a glance in a way a proportional bar never does, which is
 * why every game that has a small integer quantity — squad size, ammo, ability
 * charges, difficulty steps — ends up drawing one of these.
 *
 * THE CURSOR PREVIEW IS OPTIONAL AND IS A DISPLAY, NOT AN INPUT. With it on,
 * chips fill up to the chip under the cursor and the whole bar blends toward
 * the highlight colour, so a settings row shows what clicking would select. The
 * bar itself never commits anything: it returns what it previewed and the
 * caller decides whether a click means anything. That split is deliberate —
 * the same widget serves a read-only HUD gauge and an interactive stepper
 * without carrying a mode flag.
 *
 * The slanted edges are why this is exact geometry with a one-pixel feather
 * rather than four rectangles: a sheared edge is the case a rounded-box shader
 * renders worst (see ui/shape/Shapes.hpp), and the shear is most of the look.
 *
 * STATEFUL, for the highlight fade — see the note on ids in UiContext.hpp.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct SegmentBarSpec {
    /* Total chips, and how many of them are solid. */
    int segmentCount = 6;
    int value = 0;

    /* Fill chips up to the one under the cursor and tint the bar while
     * hovered. */
    bool hoverPreview = false;

    /* One chip's size, the horizontal shear of its top edge, the gap between
     * chips, and the stroke width of the hollow ones — all screen pixels. */
    Vec2  segmentSize{ 16.0f, 40.0f };
    float slantPx = 20.0f;
    float spacingPx = 5.0f;
    float outlineThicknessPx = 1.5f;

    float glowStrength = theme::kGlowStrength;
    float glowRadiusPx = theme::kGlowRadiusPx;

    HorizontalAlign horizontalAlign = HorizontalAlign::Centre;
    VerticalAlign   verticalAlign = VerticalAlign::Middle;

    UiColor fillColour = UiColor::white();
    UiColor emptyColour = UiColor::white().withAlpha(0.4f);
    UiColor highlightColour = UiColor::white();

    float fadeInSeconds = theme::kFadeInSeconds;
    float fadeOutSeconds = theme::kFadeOutSeconds;
    float fadeEase = theme::kFadeEase;
};

/* What the bar showed this frame. One-shot carrier. */
struct SegmentBarResult {
    bool hovered = false;

    /* The value the cursor is previewing, 1-based like the chip count; equal to
     * the spec's value when nothing is hovered. This is what a caller commits
     * on a click. */
    int previewValue = 0;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`.
 * Counts and the value are data, not geometry. */
inline SegmentBarSpec scaled(const SegmentBarSpec& spec, float factor)
{
    SegmentBarSpec out = spec;
    out.segmentSize = out.segmentSize * factor;
    out.slantPx *= factor;
    out.spacingPx *= factor;
    out.outlineThicknessPx *= factor;
    out.glowRadiusPx *= factor;
    return out;
}

SegmentBarResult drawSegmentBar(UiContext& context, UiId id, const UiRect& bounds,
                                const SegmentBarSpec& spec);

}  // namespace cromwell::ui
