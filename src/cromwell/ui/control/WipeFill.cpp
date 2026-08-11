#include "cromwell/ui/control/WipeFill.hpp"

#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>

namespace cromwell::ui {

void drawWipeFill(UiDrawList& drawList, const UiRect& bounds, const UiColor& colour,
                  float progress, HighlightAnim style)
{
    const float alpha = std::clamp(progress, 0.0f, 1.0f);
    if (alpha <= 0.0f || colour.a <= 0.0f || bounds.empty()) {
        return;
    }

    UiRect plate = bounds;
    UiColor tint = colour;

    switch (style) {
    case HighlightAnim::Fade:
        tint = colour.scaledAlpha(alpha);
        break;
    case HighlightAnim::SweepUp:
        plate.height = bounds.height * alpha;
        plate.y = bounds.bottom() - plate.height;
        break;
    case HighlightAnim::SweepDown:
        plate.height = bounds.height * alpha;
        break;
    case HighlightAnim::SweepRight:
        plate.width = bounds.width * alpha;
        break;
    case HighlightAnim::SweepLeft:
        plate.width = bounds.width * alpha;
        plate.x = bounds.right() - plate.width;
        break;
    }

    /* Hard-edged on purpose — see the note on addRect. The moving edge of a
     * sweep is the one place a feather would help, and it would cost a
     * permanent hairline seam on all four sides to buy it. */
    shapes::addRect(drawList, plate, tint);
}

}  // namespace cromwell::ui
