#include "cromwell/ui/control/Label.hpp"

#include "cromwell/ui/shape/Glow.hpp"
#include "cromwell/ui/shape/Outline.hpp"

namespace cromwell::ui {
namespace {

std::string displayText(const LabelSpec& spec)
{
    return spec.uppercase ? toUpperAscii(spec.text) : spec.text;
}

}  // namespace

Vec2 measureLabel(const UiContext& context, const LabelSpec& spec)
{
    const Vec2 text = context.metrics().measure(displayText(spec), spec.textStyle);
    return { text.x + spec.padding.horizontal(), text.y + spec.padding.vertical() };
}

void drawLabel(UiContext& context, UiId id, const UiRect& bounds, const LabelSpec& spec)
{
    WidgetState& state = context.state(id);

    /* One fade time in both directions: a tag is not reacting to a pointer, it
     * is reflecting a selection, and a selection that arrives faster than it
     * leaves reads as a glitch rather than as attention. */
    const float rawAlpha = state.fade().advance(spec.highlighted, spec.highlightFadeSeconds,
                                                spec.highlightFadeSeconds, context.time());
    const float alpha = theme::easeInOut(rawAlpha, spec.fadeEase);

    UiDrawList& drawList = context.drawList();

    /* Painter's order: the plate's halo, the plate, then the text. */
    if (alpha > 0.0f && spec.glowStrength > 0.0f && spec.glowRadiusPx > 0.0f) {
        Outline outline;
        outline.buildRect(bounds, 0.0f, 4);
        glow::addClosedOutline(drawList, outline, spec.glowRadiusPx,
                               spec.accentColour.scaledAlpha(alpha), spec.glowStrength);
    }

    drawWipeFill(drawList, bounds, spec.accentColour, alpha, spec.highlightAnim);

    /* The text colour cross-fades in step with the plate whichever way the
     * plate arrived — a sweep that left the text pale until it had finished
     * would be unreadable exactly while the eye is on it. */
    TextStyle style = spec.textStyle;
    style.colour = lerp(spec.normalTextColour, spec.highlightedTextColour, alpha);

    const std::string text = displayText(spec);
    const UiRect content = bounds.inset(spec.padding.left, spec.padding.top,
                                        spec.padding.right, spec.padding.bottom);
    const Vec2 size = context.metrics().measure(text, style);
    const UiRect placed = alignIn(content, size, spec.horizontalAlign, spec.verticalAlign);

    TextRun run;
    run.text = text;
    run.position = placed.topLeft();
    run.style = style;
    drawList.addText(std::move(run));
}

}  // namespace cromwell::ui
