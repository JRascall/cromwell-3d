/* WipeFill.hpp — how a highlight plate arrives.
 *
 * SINGLE RESPONSIBILITY: draw a coloured plate at a given 0..1 progress, either
 * fading in place or wiping in from one edge.
 *
 * WHY THIS IS ITS OWN THING RATHER THAN THREE LINES INSIDE EACH CONTROL. The
 * label, the text button and the border button all highlight by putting an
 * accent plate behind their content, and all three want the same choice of how
 * it arrives. Written per control it would be the same switch three times, and
 * the day a fourth style is added two of them would get it. It is also the one
 * piece of the highlight that is purely visual — no state, no input — so having
 * it separate keeps the controls' own files about their own layout.
 *
 * THE SWEEP STYLES ARE NOT DECORATION. A cross-fade says "this is highlighted";
 * a sweep says "this is highlighted BECAUSE something moved here", which is
 * what a keyboard- or gamepad-driven menu wants — the direction carries the
 * navigation. Cross-fade is the default because it is the one that never looks
 * wrong.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"

namespace cromwell::ui {

enum class HighlightAnim {
    /* The plate fades in place. */
    Fade,

    /* The plate wipes in from one edge, filling toward the other. */
    SweepUp,
    SweepDown,
    SweepRight,
    SweepLeft,
};

/* Draws the plate for `progress` (0 = absent, 1 = fully arrived). No-ops at
 * zero progress or zero alpha, so callers can hand their fade straight in. */
void drawWipeFill(UiDrawList& drawList, const UiRect& bounds, const UiColor& colour,
                  float progress, HighlightAnim style);

}  // namespace cromwell::ui
