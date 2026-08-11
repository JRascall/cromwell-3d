/* Label.hpp — tag/badge label with an accent plate.
 *
 * SINGLE RESPONSIBILITY: draw a short piece of text in one of two states —
 * plain, or on a highlight plate with the text flipped dark — and animate
 * between them.
 *
 * "STANDARD", "NEW", "SELECTED". The two states are what make it a tag rather
 * than a text block: unhighlighted it is quiet copy; highlighted it is a filled
 * chip that reads as chosen. Everything else — the letter spacing, the shouted
 * capitals, the halo — follows from wanting those two states to be
 * unmistakable at a glance in a row of them.
 *
 * PURELY PRESENTATIONAL. It takes no input and returns nothing. Whether it is
 * highlighted is the caller's decision, made from selection state the caller
 * owns; a label that decided for itself would need to know what it was a label
 * FOR, which is the thing that makes widgets stop being reusable.
 *
 * STATEFUL only for the cross-fade between the two states — see the note on ids
 * in UiContext.hpp.
 */
#pragma once

#include "cromwell/ui/control/WipeFill.hpp"
#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiText.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

#include <string>

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct LabelSpec {
    std::string text;

    /* Render in capitals however the copy was authored. On by default because
     * every label in this style is shouted, and switching it off per label is
     * rarer than remembering to switch it on. */
    bool uppercase = true;

    /* Which of the two states to show. */
    bool highlighted = false;

    /* Size, weight and letter spacing. The COLOUR in here is ignored — the two
     * state colours below are what the label uses — so that a caller cannot set
     * one and be surprised when the highlight overrides it. */
    TextStyle textStyle{ 16.0f, FontWeight::Bold, 2.4f, UiColor::white() };

    UiPadding padding = UiPadding::symmetric(10.0f, 4.0f);
    HorizontalAlign horizontalAlign = HorizontalAlign::Centre;
    VerticalAlign   verticalAlign = VerticalAlign::Middle;

    /* The plate, when highlighted. */
    UiColor accentColour = theme::accent();

    UiColor normalTextColour = UiColor::white().withAlpha(0.7f);

    /* Dark, so it reads against the accent plate. */
    UiColor highlightedTextColour = UiColor::black();

    /* Seconds to cross-fade between the states, both directions. */
    float highlightFadeSeconds = 0.15f;
    float fadeEase = theme::kFadeEase;

    /* Halo around the plate, fading in and out with the highlight. */
    float glowStrength = theme::kGlowStrength;
    float glowRadiusPx = theme::kGlowRadiusPx;

    HighlightAnim highlightAnim = HighlightAnim::Fade;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. */
inline LabelSpec scaled(const LabelSpec& spec, float factor)
{
    LabelSpec out = spec;
    out.textStyle = ui::scaled(out.textStyle, factor);
    out.padding = ui::scaled(out.padding, factor);
    out.glowRadiusPx *= factor;
    return out;
}

/* The label's natural size: text plus padding. Callers lay out with this; the
 * kit has no layout engine on purpose (see UiContext.hpp). */
Vec2 measureLabel(const UiContext& context, const LabelSpec& spec);

void drawLabel(UiContext& context, UiId id, const UiRect& bounds, const LabelSpec& spec);

}  // namespace cromwell::ui
