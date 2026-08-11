#include "cromwell/ui/control/SettingSlider.hpp"

#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cromwell::ui {
namespace {

/* Rounds to the nearest multiple of `step`, which is what Unreal's GridSnap
 * does and what a settings value wants. A step of zero means no snapping. */
float snapToGrid(float value, float step)
{
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

std::string formatValue(const SettingSliderSpec& spec, float value)
{
    const int digits = std::clamp(spec.fractionDigits, 0, 3);
    std::string text = std::format("{:.{}f}", value, digits);
    text += spec.valueSuffix;
    return text;
}

}  // namespace

SettingSliderResult drawSettingSlider(UiContext& context, UiId id, const UiRect& bounds,
                                      const SettingSliderSpec& spec)
{
    SettingSliderResult result;
    result.value = std::clamp(spec.value, spec.minValue, spec.maxValue);

    /* Reserve the readout's column first: the track gets what is left, so the
     * track's length is stable as the digits change. Measured from the INCOMING
     * value — the drag below may change it, and a track that resized under the
     * cursor mid-drag would drag against itself. */
    const float readoutWidth = spec.showValue
        ? spec.valueGapPx + std::max(spec.valueMinWidthPx,
                                     context.metrics()
                                         .measure(formatValue(spec, result.value), spec.valueStyle).x)
        : 0.0f;
    const UiRect trackArea{ bounds.x, bounds.y, std::max(bounds.width - readoutWidth, 0.0f), bounds.height };

    /* The thumb's CENTRE travels between these, so the tick never hangs off
     * either end of the line. */
    const float halfThumb = std::max(spec.thumbSize.x, 1.0f) * 0.5f;
    const float travelLeft = trackArea.x + halfThumb;
    const float travelRight = std::max(travelLeft, trackArea.right() - halfThumb);

    /* Hover covers the WHOLE row, including the readout: the number is part of
     * the control even though it is not draggable, and a highlight that dropped
     * as the cursor crossed onto it would flicker on every approach. Pressing
     * anywhere on the row starts a drag, which is also what makes the row easy
     * to hit. */
    const InteractionResult interaction = context.interact(id, bounds);
    WidgetState& state = context.state(id);

    if (interaction.clicked) {
        state.setDragging(true);
    }
    if (!context.mouseDown()) {
        state.setDragging(false);
    }
    result.dragging = state.dragging() && context.activeId() == id;

    if (result.dragging && travelRight > travelLeft) {
        const float fraction = std::clamp((context.mousePosition().x - travelLeft)
                                          / (travelRight - travelLeft), 0.0f, 1.0f);
        float dragged = spec.minValue + (spec.maxValue - spec.minValue) * fraction;
        if (spec.snapToStep) {
            dragged = snapToGrid(dragged, spec.stepSize);
        }
        dragged = std::clamp(dragged, spec.minValue, spec.maxValue);
        result.changed = dragged != result.value;
        result.value = dragged;
    }

    result.hovered = interaction.hovered;

    /* The drag keeps the highlight even when the cursor has left — see the
     * header. */
    const float rawAlpha = state.fade().advance(result.hovered || result.dragging,
                                                spec.fadeInSeconds, spec.fadeOutSeconds,
                                                context.time());
    const UiColor colour = theme::blendHover(spec.normalColour, spec.accentColour,
                                             rawAlpha, spec.fadeEase);

    UiDrawList& drawList = context.drawList();

    const float trackThickness = std::max(spec.trackThicknessPx, 1.0f);
    const UiRect trackLine{ trackArea.x, trackArea.centre().y - trackThickness * 0.5f,
                            trackArea.width, trackThickness };
    shapes::addRect(drawList, trackLine, colour);

    const float span = spec.maxValue - spec.minValue;
    const float fraction = span > 0.0f
        ? std::clamp((result.value - spec.minValue) / span, 0.0f, 1.0f)
        : 0.0f;
    const float thumbCentreX = travelLeft + (travelRight - travelLeft) * fraction;
    const float thumbHeight = std::max(spec.thumbSize.y, 1.0f);
    shapes::addRect(drawList, { thumbCentreX - halfThumb,
                                trackArea.centre().y - thumbHeight * 0.5f,
                                halfThumb * 2.0f, thumbHeight }, colour);

    if (spec.showValue) {
        /* Formatted AFTER the drag, so the number the user is watching is the
         * one they are dragging to rather than last frame's. */
        const std::string valueText = formatValue(spec, result.value);

        TextStyle style = spec.valueStyle;
        style.colour = colour;

        /* Right-aligned in its reserved column, so the digits line up down a
         * settings screen however many of them there are. */
        const UiRect readout{ trackArea.right() + spec.valueGapPx, bounds.y,
                              std::max(readoutWidth - spec.valueGapPx, 0.0f), bounds.height };
        const Vec2 size = context.metrics().measure(valueText, style);
        const UiRect placed = alignIn(readout, size, HorizontalAlign::Right, VerticalAlign::Middle);

        TextRun run;
        run.text = valueText;
        run.position = placed.topLeft();
        run.style = style;
        drawList.addText(std::move(run));
    }

    return result;
}

}  // namespace cromwell::ui
