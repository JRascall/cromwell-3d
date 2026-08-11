#include "cromwell/ui/panel/BlurPanel.hpp"

#include "cromwell/ui/shape/Shapes.hpp"

namespace cromwell::ui {

UiRect drawBlurPanel(UiContext& context, const UiRect& bounds, const BlurPanelSpec& spec)
{
    UiDrawList& drawList = context.drawList();

    if (spec.blurStrengthPx > 0.0f && !bounds.empty()) {
        UiBackdropBlur blur;
        blur.rect = bounds;
        blur.strengthPx = spec.blurStrengthPx;
        for (int corner = 0; corner < 4; ++corner) {
            blur.cornerRadii[corner] = spec.cornerRadii[corner];
        }
        drawList.addBackdropBlur(blur);
    }

    shapes::addRoundedRect(drawList, bounds, spec.cornerRadii, spec.fillColour, shapes::kFeatherPx);

    return bounds.inset(spec.contentPadding.left, spec.contentPadding.top,
                        spec.contentPadding.right, spec.contentPadding.bottom);
}

}  // namespace cromwell::ui
