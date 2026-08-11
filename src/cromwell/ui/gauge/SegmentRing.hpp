/* SegmentRing.hpp — segmented radial gauge.
 *
 * SINGLE RESPONSIBILITY: draw a progress band cut into N chips by straight
 * constant-width slots, over an optional backing disc.
 *
 * PROGRESS IS CONTINUOUS, NOT PER CHIP. Eight segments at 10% part-fills the
 * first chip; four segments at 89% shows three solid chips and most of the
 * fourth. Boundaries land exactly at k/N, so the fill edge crosses a slot
 * invisibly and the gauge reads as one quantity rather than as a counter.
 *
 * THE SLOTS ARE STRAIGHT CUTS, AND THIS IS THE PART THAT TOOK THE WORK. Each
 * chip is cut back from its two boundary lines by half the gap, along a line
 * PARALLEL to the boundary's radial line. The obvious alternative — a plain
 * radial wedge cut — puts the slot's two sides at visibly different angles at
 * the sizes a gauge is drawn: a chip would start at 90 degrees and end at 80,
 * and the eye reads the whole ring as sloppy without being able to say why. The
 * cost is an asin per vertex, which is nothing for a shape drawn once a frame.
 *
 * THE SLOTS ARE GENUINELY EMPTY. Whatever is behind shows through — the backing
 * disc when it is on, the scene when it is not. The disc spans the WHOLE gauge
 * rather than just the middle, so one colour covers the centre and the gaps,
 * and turning it off leaves both transparent.
 *
 * A NOTE ON THE CENTRE ICON. The Slate original took an image brush. This draw
 * list is untextured by construction (see UiDrawList.hpp), so the centre takes
 * a text run instead — a glyph from an icon font is the direct equivalent and
 * needs no texture path. Sampled images in the UI would mean a textured command
 * kind and a handle the headless half cannot name; worth adding when something
 * actually needs a photograph in a widget, not before.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiText.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

#include <string>

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct SegmentRingSpec {
    /* Outer radius and band thickness, screen pixels. */
    float radiusPx = 24.0f;
    float thicknessPx = 4.0f;

    /* Width of the slot cut between chips, screen pixels. Clamped so that the
     * slots can never eat a whole chip however many segments there are. */
    float gapPx = 4.0f;

    /* Rotates the whole chip layout. 0 puts the first chip's leading edge at 12
     * o'clock. */
    float startAngleDegrees = 0.0f;

    /* How many chips the band is split into. 1 is a continuous ring. */
    int segmentCount = 8;

    /* Fill amount 0..1, poured through the chips clockwise. */
    float progress = 0.0f;

    float glowStrength = theme::kGlowStrength;
    float glowRadiusPx = theme::kGlowRadiusPx;

    UiColor fillColour = UiColor::white();
    UiColor trackColour = theme::track();

    /* Backing disc spanning the whole gauge — it is what shows behind the
     * centre label AND in the slots between chips. Alpha 0 means no disc, and
     * therefore transparent slots. */
    UiColor discColour = UiColor::black().withAlpha(0.85f);

    /* Optional centre glyph or short label. Empty draws nothing. */
    std::string centreText;
    TextStyle   centreStyle;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. The
 * SEGMENT COUNT and the gap's relationship to it are not dimensions: eight
 * chips stay eight chips, and the gap between them grows with the ring so the
 * proportions hold. */
inline SegmentRingSpec scaled(const SegmentRingSpec& spec, float factor)
{
    SegmentRingSpec out = spec;
    out.radiusPx *= factor;
    out.thicknessPx *= factor;
    out.gapPx *= factor;
    out.glowRadiusPx *= factor;
    out.centreStyle = ui::scaled(out.centreStyle, factor);
    return out;
}

void drawSegmentRing(UiContext& context, Vec2 centre, const SegmentRingSpec& spec);

}  // namespace cromwell::ui
