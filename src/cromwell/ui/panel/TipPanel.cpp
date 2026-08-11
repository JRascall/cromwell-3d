#include "cromwell/ui/panel/TipPanel.hpp"

#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>
#include <vector>

namespace cromwell::ui {
namespace {

std::string titleText(const TipPanelSpec& spec)
{
    return spec.uppercaseTitle ? toUpperAscii(spec.title) : spec.title;
}

std::string footerText(const TipPanelSpec& spec)
{
    return spec.uppercaseFooter ? toUpperAscii(spec.footerText) : spec.footerText;
}

/* Which sections a spec actually produces, and how tall each is. Worked out
 * once and used by both the measure and the draw, because a card that measured
 * differently from how it drew would be a scrolling list's worst bug. */
struct Sections {
    bool  hasTitle = false;
    bool  hasMedia = false;
    bool  hasBody = false;
    bool  hasFooter = false;

    float titleHeight = 0.0f;
    float mediaHeight = 0.0f;

    /* Body block: the paragraph, plus the divider and footer row when present,
     * plus the body padding around the lot. */
    float bodyBlockHeight = 0.0f;

    /* Wrapped body lines, kept so the draw does not wrap twice. */
    std::vector<std::string> bodyLines;
    float bodyLineHeight = 0.0f;
    float footerRowHeight = 0.0f;

    float totalHeight = 0.0f;
};

Sections layOut(const UiContext& context, float width, const TipPanelSpec& spec)
{
    Sections sections;
    const TextMetrics& metrics = context.metrics();

    sections.hasTitle = !spec.title.empty();
    sections.hasMedia = spec.mediaHeightPx > 0.0f;
    sections.hasBody = !spec.bodyText.empty();
    sections.hasFooter = !spec.footerText.empty();

    if (sections.hasTitle) {
        const Vec2 size = metrics.measure(titleText(spec), spec.titleStyle);
        sections.titleHeight = size.y + spec.titlePadding.vertical();
    }

    if (sections.hasMedia) {
        sections.mediaHeight = spec.mediaHeightPx + spec.mediaPadding.vertical();
    }

    if (sections.hasBody || sections.hasFooter) {
        const float contentWidth = std::max(width - spec.bodyPadding.horizontal(), 1.0f);
        float height = 0.0f;

        if (sections.hasBody) {
            metrics.wrap(spec.bodyText, contentWidth, spec.bodyStyle, sections.bodyLines);
            sections.bodyLineHeight = metrics.lineHeight(spec.bodyStyle);
            height += sections.bodyLineHeight * static_cast<float>(sections.bodyLines.size());
        }

        if (sections.hasFooter) {
            const Vec2 action = metrics.measure(footerText(spec), spec.footerStyle);
            float rowHeight = action.y;
            if (!spec.footerKeyText.empty()) {
                const Vec2 key = metrics.measure(spec.footerKeyText, spec.footerKeyStyle);
                rowHeight = std::max(rowHeight, key.y + spec.keycapPadding.vertical());
            }
            sections.footerRowHeight = rowHeight;

            /* The divider only exists between something and the footer. A
             * footer-only card gets no rule above it, because there is nothing
             * for it to divide. */
            const float dividerBlock = sections.hasBody
                ? spec.dividerGapPx * 2.0f + spec.dividerThicknessPx
                : 0.0f;
            height += dividerBlock + rowHeight;
        }

        sections.bodyBlockHeight = height + spec.bodyPadding.vertical();
    }

    sections.totalHeight = sections.titleHeight + sections.mediaHeight + sections.bodyBlockHeight;
    return sections;
}

/* Top corners to the topmost visible section, bottom corners to the bottommost;
 * everything in between is square so the sections butt together cleanly. */
void sectionRadii(const TipPanelSpec& spec, bool isTop, bool isBottom, float out[4])
{
    out[0] = isTop ? spec.cornerRadii[0] : 0.0f;
    out[1] = isTop ? spec.cornerRadii[1] : 0.0f;
    out[2] = isBottom ? spec.cornerRadii[2] : 0.0f;
    out[3] = isBottom ? spec.cornerRadii[3] : 0.0f;
}

}  // namespace

float measureTipPanel(const UiContext& context, float width, const TipPanelSpec& spec)
{
    return layOut(context, width, spec).totalHeight;
}

TipPanelResult drawTipPanel(UiContext& context, const UiRect& bounds, const TipPanelSpec& spec)
{
    TipPanelResult result;

    const Sections sections = layOut(context, bounds.width, spec);
    result.height = sections.totalHeight;

    UiDrawList& drawList = context.drawList();
    const TextMetrics& metrics = context.metrics();

    /* Which section is first and which is last, so the rounding lands on the
     * card's actual outer edges rather than on a section that is not there. */
    const bool titleIsTop = sections.hasTitle;
    const bool mediaIsTop = !titleIsTop && sections.hasMedia;
    const bool bodyIsTop = !titleIsTop && !sections.hasMedia;

    const bool bodyIsBottom = sections.bodyBlockHeight > 0.0f;
    const bool mediaIsBottom = !bodyIsBottom && sections.hasMedia;
    const bool titleIsBottom = !bodyIsBottom && !sections.hasMedia;

    float cursorY = bounds.y;
    float radii[4];

    /* ---- title strip ---------------------------------------------------- */
    if (sections.hasTitle) {
        const UiRect strip{ bounds.x, cursorY, bounds.width, sections.titleHeight };
        sectionRadii(spec, titleIsTop, titleIsBottom, radii);
        shapes::addRoundedRect(drawList, strip, radii, spec.titleBarColour, shapes::kFeatherPx);

        TextStyle style = spec.titleStyle;
        style.colour = spec.titleTextColour;

        const std::string text = titleText(spec);
        const UiRect content = strip.inset(spec.titlePadding.left, spec.titlePadding.top,
                                           spec.titlePadding.right, spec.titlePadding.bottom);
        const UiRect placed = alignIn(content, metrics.measure(text, style),
                                      spec.titleAlign, VerticalAlign::Middle);

        TextRun run;
        run.text = text;
        run.position = placed.topLeft();
        run.style = style;
        drawList.addText(std::move(run));

        cursorY += sections.titleHeight;
    }

    /* ---- media area ----------------------------------------------------- */
    if (sections.hasMedia) {
        const UiRect area{ bounds.x, cursorY, bounds.width, sections.mediaHeight };
        sectionRadii(spec, mediaIsTop, mediaIsBottom, radii);

        if (spec.blurMediaBackground && spec.mediaBlurStrengthPx > 0.0f) {
            UiBackdropBlur blur;
            blur.rect = area;
            blur.strengthPx = spec.mediaBlurStrengthPx;
            for (int corner = 0; corner < 4; ++corner) {
                blur.cornerRadii[corner] = radii[corner];
            }
            drawList.addBackdropBlur(blur);
        }

        shapes::addRoundedRect(drawList, area, radii, spec.mediaBackgroundColour, shapes::kFeatherPx);

        result.mediaRect = area.inset(spec.mediaPadding.left, spec.mediaPadding.top,
                                      spec.mediaPadding.right, spec.mediaPadding.bottom);
        cursorY += sections.mediaHeight;
    }

    /* ---- body and footer ------------------------------------------------ */
    if (sections.bodyBlockHeight > 0.0f) {
        const UiRect block{ bounds.x, cursorY, bounds.width, sections.bodyBlockHeight };
        sectionRadii(spec, bodyIsTop, bodyIsBottom, radii);
        shapes::addRoundedRect(drawList, block, radii, spec.bodyBackgroundColour, shapes::kFeatherPx);

        const UiRect content = block.inset(spec.bodyPadding.left, spec.bodyPadding.top,
                                           spec.bodyPadding.right, spec.bodyPadding.bottom);
        float lineY = content.y;

        if (sections.hasBody) {
            TextStyle style = spec.bodyStyle;
            style.colour = spec.bodyTextColour;

            for (const std::string& line : sections.bodyLines) {
                const UiRect lineBox{ content.x, lineY, content.width, sections.bodyLineHeight };
                const UiRect placed = alignIn(lineBox, metrics.measure(line, style),
                                              spec.bodyAlign, VerticalAlign::Top);
                TextRun run;
                run.text = line;
                run.position = placed.topLeft();
                run.style = style;
                drawList.addText(std::move(run));
                lineY += sections.bodyLineHeight;
            }
        }

        if (sections.hasFooter) {
            if (sections.hasBody) {
                lineY += spec.dividerGapPx;
                shapes::addRect(drawList, { content.x, lineY, content.width, spec.dividerThicknessPx },
                                spec.dividerColour);
                lineY += spec.dividerThicknessPx + spec.dividerGapPx;
            }

            /* The footer row is measured as one assembly and then aligned, so
             * the keycap and its action text move together. */
            TextStyle actionStyle = spec.footerStyle;
            const std::string action = footerText(spec);
            const Vec2 actionSize = metrics.measure(action, actionStyle);

            Vec2 keycapSize;
            float gap = 0.0f;
            if (!spec.footerKeyText.empty()) {
                const Vec2 keySize = metrics.measure(spec.footerKeyText, spec.footerKeyStyle);
                keycapSize = { keySize.x + spec.keycapPadding.horizontal(),
                               keySize.y + spec.keycapPadding.vertical() };
                gap = spec.footerKeyGapPx;
            }

            const Vec2 rowSize{ keycapSize.x + gap + actionSize.x, sections.footerRowHeight };
            const UiRect rowBox{ content.x, lineY, content.width, sections.footerRowHeight };
            const UiRect row = alignIn(rowBox, rowSize, spec.footerAlign, VerticalAlign::Top);

            float cursorX = row.x;
            if (keycapSize.x > 0.0f) {
                const UiRect keycap{ cursorX, row.y + (row.height - keycapSize.y) * 0.5f,
                                     keycapSize.x, keycapSize.y };
                shapes::addRoundedRect(drawList, keycap, 2.0f, spec.keycapColour, shapes::kFeatherPx);

                TextStyle keyStyle = spec.footerKeyStyle;
                keyStyle.colour = spec.keycapTextColour;
                const UiRect placed = alignIn(keycap, metrics.measure(spec.footerKeyText, keyStyle),
                                              HorizontalAlign::Centre, VerticalAlign::Middle);
                TextRun run;
                run.text = spec.footerKeyText;
                run.position = placed.topLeft();
                run.style = keyStyle;
                drawList.addText(std::move(run));

                cursorX += keycapSize.x + gap;
            }

            TextRun run;
            run.text = action;
            run.position = { cursorX, row.y + (row.height - actionSize.y) * 0.5f };
            run.style = actionStyle;
            drawList.addText(std::move(run));
        }
    }

    return result;
}

}  // namespace cromwell::ui
