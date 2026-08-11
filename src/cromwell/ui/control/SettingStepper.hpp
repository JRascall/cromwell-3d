/* SettingStepper.hpp — the settings-menu option stepper.
 *
 * SINGLE RESPONSIBILITY: draw "< VALUE >" over a list of options, step it from
 * the pointer, and hand back the selected index.
 *
 * For anything enumerated — OFF/ON, LOW/MEDIUM/HIGH/ULTRA, a resolution list.
 * A dropdown would need a popup layer, focus capture and a scroll view for
 * three options; a stepper needs none of that and is faster to use with a
 * gamepad, which is why console settings screens are built from them.
 *
 * THE VALUE COLUMN HAS A RESERVED WIDTH so the chevrons do not shuffle
 * sideways as the option text changes length. Watching the arrows twitch every
 * time you step is exactly the sort of thing nobody reports and everybody
 * notices.
 *
 * A CHEVRON AT THE END OF THE LIST IS HIDDEN, NOT REMOVED — it keeps its space,
 * for the same reason. With `wrap` on, neither is ever hidden and stepping past
 * the end comes round.
 *
 * CLICKING ANYWHERE STEPS, by default: left of the value's centre goes back,
 * right goes forward. It makes the whole row a target instead of two small
 * glyphs, which matters more than it sounds at 4K. The chevrons still highlight
 * individually so the affordance stays visible.
 */
#pragma once

#include "cromwell/ui/core/UiColor.hpp"
#include "cromwell/ui/core/UiContext.hpp"
#include "cromwell/ui/core/UiText.hpp"
#include "cromwell/ui/core/UiTheme.hpp"

#include <span>
#include <string>

namespace cromwell::ui {

/* ONE-SHOT DATA CARRIER — see the note in UiColor.hpp. */
struct SettingStepperSpec {
    /* The options, shown verbatim. The span is borrowed for the duration of the
     * call and never stored. */
    std::span<const std::string> options;
    int selectedIndex = 0;

    /* Cycle past the ends instead of stopping. */
    bool wrap = false;

    /* Clicks anywhere on the row step the value — see the header. */
    bool clickAreaSteps = true;

    TextStyle valueStyle{ 14.0f, FontWeight::SemiBold, 0.0f, UiColor::white() };

    /* Reserved width for the value column. */
    float valueMinWidthPx = 140.0f;

    /* Padding around each chevron's hit area. */
    UiPadding chevronPadding = UiPadding::symmetric(10.0f, 2.0f);

    /* The chevron glyphs. Default to the single angle quotation marks the
     * original used; override them for a font that lacks those code points, or
     * to use an icon font's arrows. */
    std::string previousGlyph = "‹";
    std::string nextGlyph = "›";

    UiColor normalColour = UiColor::white();
    UiColor accentColour = theme::accent();

    float fadeInSeconds = theme::kFadeInSeconds;
    float fadeOutSeconds = theme::kFadeOutSeconds;
    float fadeEase = theme::kFadeEase;
};

/* What the stepper did this frame. One-shot carrier. */
struct SettingStepperResult {
    bool hovered = false;
    bool changed = false;
    int  selectedIndex = 0;
};

/* Pixel dimensions only — see the note beside LoadingRingSpec's `scaled`. The
 * options and the selected index are data; the span is copied by reference and
 * still points at the caller's strings. */
inline SettingStepperSpec scaled(const SettingStepperSpec& spec, float factor)
{
    SettingStepperSpec out = spec;
    out.valueStyle = ui::scaled(out.valueStyle, factor);
    out.valueMinWidthPx *= factor;
    out.chevronPadding = ui::scaled(out.chevronPadding, factor);
    return out;
}

SettingStepperResult drawSettingStepper(UiContext& context, UiId id, const UiRect& bounds,
                                        const SettingStepperSpec& spec);

}  // namespace cromwell::ui
