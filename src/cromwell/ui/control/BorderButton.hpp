/* BorderButton.hpp — stroked-box button with an optional key prompt.
 *
 * SINGLE RESPONSIBILITY: draw "[ESC] CLOSE" — a thin outlined box around a
 * keycap chip and an action label — and say whether it was clicked.
 *
 * THE KEY PROMPT IS THE REASON THIS EXISTS as a separate control rather than a
 * text button with a border. A prompt that shows which key does the thing is
 * the single most useful affordance a game menu has, and it has to be part of
 * the button rather than a label beside it: the chip's plate has to tint with
 * the button's hover, the gap has to collapse when there is no prompt, and the
 * whole assembly has to centre as one. All three of those are wrong when the
 * prompt is a sibling.
 *
 * EMPTY KEY TEXT MEANS NO PROMPT, and the gap collapses with it, so the same
 * control is also a plain outlined text button. That is deliberately not a
 * separate mode flag — one fewer thing to set, and one fewer way to set it
 * inconsistently.
 *
 * ON ICONS. The Slate original could show a gamepad glyph from a texture
 * instead of the keycap. This draw list is untextured (see UiDrawList.hpp), so
 * the equivalent here is a glyph from an icon font in `keyText` — which is how
 * most prompt systems do it anyway, because a font scales and a texture atlas
 * does not.
 *
 * The two hover styles work as they do on the text button: Cross-Fade tints the
 * stroke, label and keycap toward the accent; the sweep styles flood the box's
 * interior with accent and flip the contents dark.
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
struct BorderButtonSpec {
    /* The action label ("Close"). */
    std::string text;
    bool uppercase = true;

    /* The keycap's text ("ESC", "F", "TAB"), or an icon-font glyph. Empty means
     * no prompt at all. */
    std::string keyText;

    TextStyle labelStyle{ 12.0f, FontWeight::SemiBold, 1.8f, UiColor::white() };
    TextStyle keyStyle{ 10.0f, FontWeight::Bold, 1.0f, UiColor::black() };

    UiPadding contentPadding = UiPadding::symmetric(10.0f, 5.0f);
    UiPadding keycapPadding = UiPadding::symmetric(6.0f, 2.0f);

    /* Gap between the keycap and the label. Collapses to nothing when there is
     * no prompt. */
    float keyGapPx = 8.0f;

    HorizontalAlign horizontalAlign = HorizontalAlign::Centre;
    VerticalAlign   verticalAlign = VerticalAlign::Middle;

    /* The box. Transparent background by default, so it reads as a pure
     * outline. */
    UiColor strokeColour = UiColor::white().withAlpha(0.7f);
    float   strokeThicknessPx = 1.0f;
    float   cornerRadiusPx = 2.0f;
    UiColor backgroundColour = UiColor::transparent();

    /* The keycap chip: a light plate with dark text on it. */
    UiColor keycapColour = UiColor{ 0.9f, 0.9f, 0.9f, 1.0f };
    UiColor keycapTextColour = UiColor::black();

    UiColor labelColour = UiColor::white();
    UiColor accentColour = theme::accent();

    /* Label and keycap colour under the cursor in the sweep styles — dark, so
     * they read on the accent flood. */
    UiColor hoveredContentColour = UiColor::black();

    float fadeInSeconds = theme::kFadeInSeconds;
    float fadeOutSeconds = theme::kFadeOutSeconds;
    float fadeEase = theme::kFadeEase;

    HighlightAnim hoverAnim = HighlightAnim::Fade;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. The
 * stroke scales with everything else: a hairline that stayed one device pixel
 * at 200% would read as a thinner, meaner box than the one that was designed. */
inline BorderButtonSpec scaled(const BorderButtonSpec& spec, float factor)
{
    BorderButtonSpec out = spec;
    out.labelStyle = ui::scaled(out.labelStyle, factor);
    out.keyStyle = ui::scaled(out.keyStyle, factor);
    out.contentPadding = ui::scaled(out.contentPadding, factor);
    out.keycapPadding = ui::scaled(out.keycapPadding, factor);
    out.keyGapPx *= factor;
    out.strokeThicknessPx *= factor;
    out.cornerRadiusPx *= factor;
    return out;
}

Vec2 measureBorderButton(const UiContext& context, const BorderButtonSpec& spec);

InteractionResult drawBorderButton(UiContext& context, UiId id, const UiRect& bounds,
                                   const BorderButtonSpec& spec);

}  // namespace cromwell::ui
