#include "cromwell/ui/control/SettingStepper.hpp"

#include <algorithm>

namespace cromwell::ui {
namespace {

/* Steps `index` by `direction` over `count` options, wrapping or clamping.
 * Returns the same index when there is nowhere to go, which is what makes
 * "changed" honest. */
int stepIndex(int index, int direction, int count, bool wrap)
{
    if (count < 2) {
        return index;
    }
    const int moved = index + direction;
    return wrap ? ((moved % count) + count) % count : std::clamp(moved, 0, count - 1);
}

}  // namespace

SettingStepperResult drawSettingStepper(UiContext& context, UiId id, const UiRect& bounds,
                                        const SettingStepperSpec& spec)
{
    SettingStepperResult result;

    const int count = static_cast<int>(spec.options.size());
    result.selectedIndex = count > 0 ? std::clamp(spec.selectedIndex, 0, count - 1) : 0;

    /* A view, not a copy: the option strings belong to the caller and outlive
     * the call. Empty when there are no options, which draws nothing rather
     * than drawing an index nobody supplied. */
    const std::string_view valueText = count > 0
        ? std::string_view(spec.options[static_cast<std::size_t>(result.selectedIndex)])
        : std::string_view();

    /* ---- layout -------------------------------------------------------- */

    const Vec2 previousSize = context.metrics().measure(spec.previousGlyph, spec.valueStyle);
    const Vec2 nextSize = context.metrics().measure(spec.nextGlyph, spec.valueStyle);

    const UiRect previousRect{ bounds.x, bounds.y,
                               previousSize.x + spec.chevronPadding.horizontal(), bounds.height };
    const UiRect nextRect{ bounds.right() - (nextSize.x + spec.chevronPadding.horizontal()), bounds.y,
                           nextSize.x + spec.chevronPadding.horizontal(), bounds.height };

    const UiRect valueRect{ previousRect.right(), bounds.y,
                            std::max(nextRect.x - previousRect.right(), spec.valueMinWidthPx),
                            bounds.height };

    /* ---- interaction ---------------------------------------------------- */

    /* The chevrons are tested FIRST and take the press, so a click on one steps
     * once rather than stepping once for the chevron and again for the row
     * underneath it. */
    const InteractionResult previous = context.interact(UiContext::childId(id, "prev"), previousRect);
    const InteractionResult next = context.interact(UiContext::childId(id, "next"), nextRect);
    const InteractionResult row = context.interact(id, bounds);

    const bool previousEnabled = spec.wrap || result.selectedIndex > 0;
    const bool nextEnabled = spec.wrap || result.selectedIndex < count - 1;

    int direction = 0;
    if (previous.clicked && previousEnabled) {
        direction = -1;
    } else if (next.clicked && nextEnabled) {
        direction = +1;
    } else if (row.clicked && spec.clickAreaSteps) {
        /* Left of the value column's centre steps back, right steps forward. */
        direction = context.mousePosition().x < valueRect.centre().x ? -1 : +1;
    }

    if (direction != 0) {
        const int moved = stepIndex(result.selectedIndex, direction, count, spec.wrap);
        result.changed = moved != result.selectedIndex;
        result.selectedIndex = moved;
    }

    result.hovered = row.hovered || previous.hovered || next.hovered;

    /* ---- colour --------------------------------------------------------- */

    WidgetState& state = context.state(id);
    const float controlAlpha = state.fade().advance(result.hovered, spec.fadeInSeconds,
                                                    spec.fadeOutSeconds, context.time());

    /* A chevron under the cursor goes brighter than the row's own highlight,
     * never dimmer: the two fades are combined by taking the stronger, so
     * pointing at a chevron highlights it without the row's blend dragging it
     * back down. */
    const auto chevronAlpha = [&](int slot, bool hovered) {
        const float own = state.auxFade(slot).advance(hovered, spec.fadeInSeconds,
                                                      spec.fadeOutSeconds, context.time());
        return std::max(own, controlAlpha);
    };

    const UiColor valueColour = theme::blendHover(spec.normalColour, spec.accentColour,
                                                  controlAlpha, spec.fadeEase);
    const UiColor previousColour = theme::blendHover(spec.normalColour, spec.accentColour,
                                                     chevronAlpha(0, previous.hovered), spec.fadeEase);
    const UiColor nextColour = theme::blendHover(spec.normalColour, spec.accentColour,
                                                 chevronAlpha(1, next.hovered), spec.fadeEase);

    /* ---- draw ----------------------------------------------------------- */

    UiDrawList& drawList = context.drawList();

    const auto drawGlyph = [&](const std::string& glyph, const UiRect& rect,
                               const UiColor& colour, bool visible) {
        /* Hidden rather than removed: the space stays reserved so the value
         * never shifts when a chevron goes away. */
        if (glyph.empty() || !visible) {
            return;
        }
        TextStyle style = spec.valueStyle;
        style.colour = colour;
        const Vec2 size = context.metrics().measure(glyph, style);
        const UiRect placed = alignIn(rect, size, HorizontalAlign::Centre, VerticalAlign::Middle);

        TextRun run;
        run.text = glyph;
        run.position = placed.topLeft();
        run.style = style;
        drawList.addText(std::move(run));
    };

    drawGlyph(spec.previousGlyph, previousRect, previousColour, previousEnabled);
    drawGlyph(spec.nextGlyph, nextRect, nextColour, nextEnabled);

    if (!valueText.empty()) {
        TextStyle style = spec.valueStyle;
        style.colour = valueColour;
        const Vec2 size = context.metrics().measure(valueText, style);
        const UiRect placed = alignIn(valueRect, size, HorizontalAlign::Centre, VerticalAlign::Middle);

        TextRun run;
        run.text = std::string(valueText);
        run.position = placed.topLeft();
        run.style = style;
        drawList.addText(std::move(run));
    }

    return result;
}

}  // namespace cromwell::ui
