#include "cromwell/ui/control/BorderButton.hpp"

#include "cromwell/ui/shape/Outline.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>

namespace cromwell::ui {
namespace {

std::string displayText(const BorderButtonSpec& spec)
{
    return spec.uppercase ? toUpperAscii(spec.text) : spec.text;
}

/* The row of content inside the stroke: the keycap chip (or nothing), the gap
 * (or nothing), and the label. Measured together because the button's size, the
 * chip's rect and the label's rect all come from the same arithmetic and would
 * drift if it were written twice. */
struct ContentLayout {
    Vec2  keycapSize;      /* zero when there is no prompt */
    Vec2  labelSize;
    float gap = 0.0f;
    Vec2  totalSize;
};

ContentLayout layOutContent(const UiContext& context, const BorderButtonSpec& spec)
{
    ContentLayout layout;
    layout.labelSize = context.metrics().measure(displayText(spec), spec.labelStyle);

    if (!spec.keyText.empty()) {
        const Vec2 keyText = context.metrics().measure(spec.keyText, spec.keyStyle);
        layout.keycapSize = { keyText.x + spec.keycapPadding.horizontal(),
                              keyText.y + spec.keycapPadding.vertical() };
        layout.gap = spec.keyGapPx;
    }

    layout.totalSize = { layout.keycapSize.x + layout.gap + layout.labelSize.x,
                         std::max(layout.keycapSize.y, layout.labelSize.y) };
    return layout;
}

}  // namespace

Vec2 measureBorderButton(const UiContext& context, const BorderButtonSpec& spec)
{
    const ContentLayout layout = layOutContent(context, spec);
    return { layout.totalSize.x + spec.contentPadding.horizontal(),
             layout.totalSize.y + spec.contentPadding.vertical() };
}

InteractionResult drawBorderButton(UiContext& context, UiId id, const UiRect& bounds,
                                   const BorderButtonSpec& spec)
{
    const InteractionResult interaction = context.interact(id, bounds);

    WidgetState& state = context.state(id);
    const float rawAlpha = state.fade().advance(interaction.hovered, spec.fadeInSeconds,
                                                spec.fadeOutSeconds, context.time());
    const float alpha = theme::easeInOut(rawAlpha, spec.fadeEase);

    const bool sweeping = spec.hoverAnim != HighlightAnim::Fade;
    const UiColor contentTarget = sweeping ? spec.hoveredContentColour : spec.accentColour;

    UiDrawList& drawList = context.drawList();

    /* Painter's order: background, sweep flood, stroke, then the contents. The
     * flood is INSET by the stroke width so it fills the box rather than
     * painting over its own outline. */
    if (spec.backgroundColour.a > 0.0f) {
        shapes::addRoundedRect(drawList, bounds, spec.cornerRadiusPx,
                               spec.backgroundColour, shapes::kFeatherPx);
    }

    if (sweeping) {
        drawWipeFill(drawList, bounds.inset(spec.strokeThicknessPx), spec.accentColour,
                     alpha, spec.hoverAnim);
    }

    {
        Outline outline;
        outline.buildRect(bounds, spec.cornerRadiusPx, 8);
        const UiColor stroke = lerp(spec.strokeColour, spec.accentColour, alpha);
        shapes::addOutlineStroke(drawList, outline, spec.strokeThicknessPx,
                                 stroke, shapes::kFeatherPx);
    }

    const ContentLayout layout = layOutContent(context, spec);
    const UiRect content = bounds.inset(spec.contentPadding.left, spec.contentPadding.top,
                                        spec.contentPadding.right, spec.contentPadding.bottom);
    const UiRect row = alignIn(content, layout.totalSize, spec.horizontalAlign, spec.verticalAlign);

    float cursorX = row.x;

    if (layout.keycapSize.x > 0.0f) {
        const UiRect keycap{ cursorX, row.y + (row.height - layout.keycapSize.y) * 0.5f,
                             layout.keycapSize.x, layout.keycapSize.y };

        shapes::addRoundedRect(drawList, keycap, spec.cornerRadiusPx,
                               lerp(spec.keycapColour, contentTarget, alpha), shapes::kFeatherPx);

        /* On the sweep styles the chip's plate goes dark, so its text flips to
         * the accent to keep reading against it. Under Cross-Fade the plate
         * stays light and the authored text colour is left alone. */
        TextStyle keyStyle = spec.keyStyle;
        keyStyle.colour = sweeping
            ? lerp(spec.keycapTextColour, spec.accentColour, alpha)
            : spec.keycapTextColour;

        const Vec2 keySize = context.metrics().measure(spec.keyText, keyStyle);
        const UiRect placed = alignIn(keycap.inset(spec.keycapPadding.left, spec.keycapPadding.top,
                                                   spec.keycapPadding.right, spec.keycapPadding.bottom),
                                      keySize, HorizontalAlign::Centre, VerticalAlign::Middle);

        TextRun run;
        run.text = spec.keyText;
        run.position = placed.topLeft();
        run.style = keyStyle;
        drawList.addText(std::move(run));

        cursorX += layout.keycapSize.x + layout.gap;
    }

    TextStyle labelStyle = spec.labelStyle;
    labelStyle.colour = lerp(spec.labelColour, contentTarget, alpha);

    TextRun label;
    label.text = displayText(spec);
    label.position = { cursorX, row.y + (row.height - layout.labelSize.y) * 0.5f };
    label.style = labelStyle;
    drawList.addText(std::move(label));

    return interaction;
}

}  // namespace cromwell::ui
