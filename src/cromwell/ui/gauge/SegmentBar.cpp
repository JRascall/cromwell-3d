#include "cromwell/ui/gauge/SegmentBar.hpp"

#include "cromwell/ui/shape/Glow.hpp"
#include "cromwell/ui/shape/Outline.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>

namespace cromwell::ui {
namespace {

/* One chip's four corners, wound clockwise from the top-left: the top edge is
 * sheared right by `slant`, so the chip leans like an italic. */
void chipCorners(Vec2 topLeft, float width, float height, float slant, Vec2 outPoints[4])
{
    outPoints[0] = { topLeft.x + slant, topLeft.y };
    outPoints[1] = { topLeft.x + slant + width, topLeft.y };
    outPoints[2] = { topLeft.x + width, topLeft.y + height };
    outPoints[3] = { topLeft.x, topLeft.y + height };
}

}  // namespace

SegmentBarResult drawSegmentBar(UiContext& context, UiId id, const UiRect& bounds,
                                const SegmentBarSpec& spec)
{
    SegmentBarResult result;

    const int segmentCount = std::clamp(spec.segmentCount, 1, 64);
    const float width = std::max(spec.segmentSize.x, 2.0f);
    const float height = std::max(spec.segmentSize.y, 2.0f);
    const float slant = std::max(spec.slantPx, 0.0f);
    const float spacing = std::max(spec.spacingPx, 0.0f);
    const float stroke = std::max(spec.outlineThicknessPx, 0.5f);

    /* The row's natural footprint, and where it sits inside whatever it was
     * given. Alignment only matters when the caller handed over more room than
     * the chips need. */
    const float contentWidth = (width + spacing) * static_cast<float>(segmentCount) - spacing + slant;
    float offsetX = bounds.x;
    if (spec.horizontalAlign == HorizontalAlign::Centre) {
        offsetX += (bounds.width - contentWidth) * 0.5f;
    } else if (spec.horizontalAlign == HorizontalAlign::Right) {
        offsetX += bounds.width - contentWidth;
    }
    float offsetY = bounds.y;
    if (spec.verticalAlign == VerticalAlign::Middle) {
        offsetY += (bounds.height - height) * 0.5f;
    } else if (spec.verticalAlign == VerticalAlign::Bottom) {
        offsetY += bounds.height - height;
    }

    const UiRect contentRect{ offsetX, offsetY, contentWidth, height };

    const int filled = std::clamp(spec.value, 0, segmentCount);
    int displayFilled = filled;

    /* Geometric cursor test against the row's own box, not the bounds it was
     * given — a bar centred in a wide row should not highlight when the cursor
     * is in the empty margin beside it. */
    if (spec.hoverPreview && context.isHovered(contentRect)) {
        result.hovered = true;
        const float local = context.mousePosition().x - contentRect.x;
        displayFilled = std::clamp(static_cast<int>(local / (width + spacing)) + 1, 1, segmentCount);
    }
    result.previewValue = result.hovered ? displayFilled : filled;

    WidgetState& state = context.state(id);
    const float highlightAlpha = state.fade().advance(result.hovered, spec.fadeInSeconds,
                                                      spec.fadeOutSeconds, context.time());

    const UiColor fillColour = theme::blendHover(spec.fillColour, spec.highlightColour,
                                                 highlightAlpha, spec.fadeEase);
    const UiColor emptyColour = theme::blendHover(spec.emptyColour, spec.highlightColour,
                                                  highlightAlpha, spec.fadeEase);

    UiDrawList& drawList = context.drawList();
    Outline outline;
    Vec2 corners[4];

    /* The halo pass goes under EVERYTHING, and covers the solid chips only:
     * haloing the hollow ones too would fill their interiors with a glow and
     * cost them the contrast that makes "empty" read as empty. */
    if (spec.glowStrength > 0.0f && spec.glowRadiusPx > 0.0f) {
        for (int segment = 0; segment < std::min(displayFilled, segmentCount); ++segment) {
            const Vec2 topLeft{ offsetX + static_cast<float>(segment) * (width + spacing), offsetY };
            chipCorners(topLeft, width, height, slant, corners);
            outline.buildConvexPolygon(corners, 4);
            glow::addClosedOutline(drawList, outline, spec.glowRadiusPx, fillColour, spec.glowStrength);
        }
    }

    for (int segment = 0; segment < segmentCount; ++segment) {
        const Vec2 topLeft{ offsetX + static_cast<float>(segment) * (width + spacing), offsetY };
        chipCorners(topLeft, width, height, slant, corners);
        outline.buildConvexPolygon(corners, 4);

        if (segment < displayFilled) {
            shapes::addConvexFill(drawList, outline, fillColour, shapes::kFeatherPx);
        } else {
            shapes::addOutlineStroke(drawList, outline, stroke, emptyColour, shapes::kFeatherPx);
        }
    }

    return result;
}

}  // namespace cromwell::ui
