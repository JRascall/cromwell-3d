#include "cromwell/ui/control/TextButton.hpp"

namespace cromwell::ui {
namespace {

std::string displayText(const TextButtonSpec& spec)
{
    return spec.uppercase ? toUpperAscii(spec.text) : spec.text;
}

/* Zero under Cross-Fade, the authored padding under a sweep — see the header. */
UiPadding effectivePadding(const TextButtonSpec& spec)
{
    return spec.hoverAnim == HighlightAnim::Fade ? UiPadding{} : spec.platePadding;
}

}  // namespace

Vec2 measureTextButton(const UiContext& context, const TextButtonSpec& spec)
{
    const UiPadding padding = effectivePadding(spec);
    const Vec2 text = context.metrics().measure(displayText(spec), spec.textStyle);
    return { text.x + padding.horizontal(), text.y + padding.vertical() };
}

InteractionResult drawTextButton(UiContext& context, UiId id, const UiRect& bounds,
                                 const TextButtonSpec& spec)
{
    const InteractionResult interaction = context.interact(id, bounds);

    WidgetState& state = context.state(id);
    const float rawAlpha = state.fade().advance(interaction.hovered, spec.fadeInSeconds,
                                                spec.fadeOutSeconds, context.time());
    const float alpha = theme::easeInOut(rawAlpha, spec.fadeEase);

    UiDrawList& drawList = context.drawList();

    /* The plate exists only in the sweep styles; under Cross-Fade the button is
     * bare text and the fill is skipped entirely rather than drawn
     * transparent. */
    if (spec.hoverAnim != HighlightAnim::Fade) {
        drawWipeFill(drawList, bounds, spec.accentColour, alpha, spec.hoverAnim);
    }

    TextStyle style = spec.textStyle;
    const UiColor target = spec.hoverAnim == HighlightAnim::Fade
        ? spec.accentColour
        : spec.hoveredTextColour;
    style.colour = lerp(spec.normalColour, target, alpha);

    const std::string text = displayText(spec);
    const UiPadding padding = effectivePadding(spec);
    const UiRect content = bounds.inset(padding.left, padding.top, padding.right, padding.bottom);
    const Vec2 size = context.metrics().measure(text, style);
    const UiRect placed = alignIn(content, size, spec.horizontalAlign, spec.verticalAlign);

    TextRun run;
    run.text = text;
    run.position = placed.topLeft();
    run.style = style;
    drawList.addText(std::move(run));

    return interaction;
}

}  // namespace cromwell::ui
