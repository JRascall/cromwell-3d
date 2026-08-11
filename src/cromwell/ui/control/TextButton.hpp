/* TextButton.hpp — the text-only button.
 *
 * SINGLE RESPONSIBILITY: draw a piece of letter-spaced text that reacts to the
 * cursor, and say whether it was clicked.
 *
 * The main-menu item as a reusable control: no box, no border, no plate — just
 * type that lights up. Which is why the hover treatment carries the whole
 * interaction, and why there are two of them. Cross-Fade tints the text toward
 * the accent, which is the quiet version. The sweep styles wipe an accent plate
 * in behind it and flip the text dark, which is the loud one, and reads as
 * navigation rather than as hover — worth the extra option in a menu driven by
 * a gamepad, where "the highlight moved down" is the entire feedback.
 *
 * PLATE PADDING APPLIES TO THE SWEEP STYLES ONLY. Under Cross-Fade the button
 * is exactly its text, because padding around nothing visible is just a bigger
 * hit area than the thing you can see; under a sweep the padding is the plate's
 * breathing room and the button is measured with it.
 *
 * CLICKS FIRE ON PRESS. See the note in UiContext.hpp — a menu is markedly more
 * responsive for it, and nothing here needs press-drag-off-to-cancel.
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
struct TextButtonSpec {
    std::string text;
    bool uppercase = true;

    /* The colour in here is ignored; the state colours below decide it. */
    TextStyle textStyle{ 16.0f, FontWeight::SemiBold, 2.4f, UiColor::white() };

    HorizontalAlign horizontalAlign = HorizontalAlign::Centre;
    VerticalAlign   verticalAlign = VerticalAlign::Middle;

    UiColor normalColour = UiColor::white();
    UiColor accentColour = theme::accent();

    /* Text colour under the cursor in the sweep styles — dark, so it reads on
     * the accent plate. Cross-Fade ignores it and tints toward the accent
     * itself. */
    UiColor hoveredTextColour = UiColor::black();

    float fadeInSeconds = theme::kFadeInSeconds;
    float fadeOutSeconds = theme::kFadeOutSeconds;
    float fadeEase = theme::kFadeEase;

    HighlightAnim hoverAnim = HighlightAnim::Fade;

    /* Breathing room between the text and the plate's edges. Sweep styles
     * only — see the header. */
    UiPadding platePadding = UiPadding::symmetric(10.0f, 4.0f);
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. */
inline TextButtonSpec scaled(const TextButtonSpec& spec, float factor)
{
    TextButtonSpec out = spec;
    out.textStyle = ui::scaled(out.textStyle, factor);
    out.platePadding = ui::scaled(out.platePadding, factor);
    return out;
}

Vec2 measureTextButton(const UiContext& context, const TextButtonSpec& spec);

/* Draws it and returns what the pointer did. */
InteractionResult drawTextButton(UiContext& context, UiId id, const UiRect& bounds,
                                 const TextButtonSpec& spec);

}  // namespace cromwell::ui
