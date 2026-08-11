/* TipPanel.hpp — the tutorial/tip card.
 *
 * SINGLE RESPONSIBILITY: stack a title strip, an optional media area, a body
 * paragraph and a footer key prompt into one card, and report where the media
 * area landed so the caller can draw into it.
 *
 * The layout, top to bottom: a white TITLE strip with black text — an inverted
 * header is what makes the card read as a labelled callout rather than as
 * floating copy; an optional MEDIA area on translucent black, for an
 * illustration or a live demo of the thing being explained; a BODY paragraph on
 * near-black for reading contrast; and a FOOTER key prompt under a divider.
 *
 * EVERY SECTION HIDES ITSELF WHEN ITS CONTENT IS EMPTY. That is what lets one
 * widget serve a full tutorial card and a one-line hint, and it is why the
 * corner radii are assigned to whichever sections are currently the top and
 * bottom rather than to fixed ones — round the title's top corners on a card
 * that has no title and the rounding lands nowhere.
 *
 * PURELY PRESENTATIONAL. It takes no input. The media area is returned rather
 * than owned, for the reason BlurPanel.hpp gives: an immediate-mode panel
 * cannot own its content.
 *
 * MEASURE BEFORE YOU PLACE IT. The card's height depends on how the body wraps,
 * which depends on the width, so `measureTipPanel` takes a width and returns a
 * height. A caller that wants the card bottom-anchored needs that number before
 * it can pick a rect.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiText.hpp"

#include <string>

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct TipPanelSpec {
    /* Empty title = no title strip. */
    std::string title;

    /* Wraps to the card's width. Empty = no body row. */
    std::string bodyText;

    /* Footer keycap ("TAB") and action ("Open Manual"). An empty action hides
     * the whole footer AND its divider; the keycap alone is not a footer. */
    std::string footerKeyText;
    std::string footerText;

    bool uppercaseTitle = true;
    bool uppercaseFooter = true;

    /* Height reserved for the media area. 0 = no media section. */
    float mediaHeightPx = 0.0f;

    /* Frost whatever shows through the translucent media background. */
    bool  blurMediaBackground = false;
    float mediaBlurStrengthPx = 8.0f;

    TextStyle titleStyle{ 14.0f, FontWeight::Bold, 2.1f, UiColor::black() };
    TextStyle bodyStyle{ 14.0f, FontWeight::Regular, 0.0f, UiColor::white() };
    TextStyle footerStyle{ 12.0f, FontWeight::SemiBold, 1.8f, UiColor::white() };
    TextStyle footerKeyStyle{ 10.0f, FontWeight::Bold, 1.0f, UiColor::black() };

    UiColor titleBarColour = UiColor::white();
    UiColor titleTextColour = UiColor::black();
    UiColor mediaBackgroundColour = UiColor::black().withAlpha(0.55f);
    UiColor bodyBackgroundColour = UiColor::black().withAlpha(0.9f);
    UiColor bodyTextColour = UiColor::white().withAlpha(0.9f);
    UiColor dividerColour = UiColor::white().withAlpha(0.15f);
    UiColor keycapColour = UiColor{ 0.9f, 0.9f, 0.9f, 1.0f };
    UiColor keycapTextColour = UiColor::black();

    UiPadding titlePadding = UiPadding::symmetric(12.0f, 6.0f);
    UiPadding mediaPadding = UiPadding::all(12.0f);
    UiPadding bodyPadding = UiPadding::all(12.0f);
    UiPadding keycapPadding = UiPadding::symmetric(6.0f, 2.0f);

    float footerKeyGapPx = 8.0f;
    float dividerThicknessPx = 1.0f;
    float dividerGapPx = 8.0f;

    /* Whole-card rounding: top-left, top-right, bottom-right, bottom-left. The
     * radii land on whichever sections are currently topmost and bottommost. */
    float cornerRadii[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    HorizontalAlign titleAlign = HorizontalAlign::Left;
    HorizontalAlign bodyAlign = HorizontalAlign::Left;
    HorizontalAlign footerAlign = HorizontalAlign::Left;
};

/* Where the card's parts ended up. One-shot carrier. */
struct TipPanelResult {
    /* The media area's content rect, already inset by the media padding. Empty
     * when the card has no media section. */
    UiRect mediaRect;

    /* The height the card actually used, which is what `measureTipPanel`
     * returns for the same inputs. */
    float height = 0.0f;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. */
inline TipPanelSpec scaled(const TipPanelSpec& spec, float factor)
{
    TipPanelSpec out = spec;
    out.mediaHeightPx *= factor;
    out.mediaBlurStrengthPx *= factor;
    out.titleStyle = ui::scaled(out.titleStyle, factor);
    out.bodyStyle = ui::scaled(out.bodyStyle, factor);
    out.footerStyle = ui::scaled(out.footerStyle, factor);
    out.footerKeyStyle = ui::scaled(out.footerKeyStyle, factor);
    out.titlePadding = ui::scaled(out.titlePadding, factor);
    out.mediaPadding = ui::scaled(out.mediaPadding, factor);
    out.bodyPadding = ui::scaled(out.bodyPadding, factor);
    out.keycapPadding = ui::scaled(out.keycapPadding, factor);
    out.footerKeyGapPx *= factor;
    out.dividerThicknessPx *= factor;
    out.dividerGapPx *= factor;
    for (float& radius : out.cornerRadii) {
        radius *= factor;
    }
    return out;
}

/* The card's height at the given width. */
float measureTipPanel(const UiContext& context, float width, const TipPanelSpec& spec);

/* Draws from `bounds`'s top-left, using its width and as much height as the
 * content needs — `bounds.height` is ignored, since the card's height is a
 * function of its content. */
TipPanelResult drawTipPanel(UiContext& context, const UiRect& bounds, const TipPanelSpec& spec);

}  // namespace cromwell::ui
