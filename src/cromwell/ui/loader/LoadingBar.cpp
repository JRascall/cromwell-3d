#include "cromwell/ui/loader/LoadingBar.hpp"

#include "cromwell/ui/shape/Glow.hpp"
#include "cromwell/ui/shape/Outline.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>

namespace cromwell::ui {
namespace {

constexpr float kEpsilon = 1.0e-4f;

}  // namespace

void drawLoadingBar(UiContext& context, UiId id, const UiRect& bounds, const LoadingBarSpec& spec)
{
    if (bounds.empty()) {
        return;
    }

    const float height = std::min(std::max(spec.barHeightPx, 1.0f), bounds.height);
    const UiRect barRect{ bounds.x, bounds.y + (bounds.height - height) * 0.5f, bounds.width, height };

    const float target = std::clamp(spec.progress, 0.0f, 1.0f);

    WidgetState& state = context.state(id);
    if (!state.seen()) {
        /* First frame shows the bound value rather than animating up from zero
         * — a bar that appears mid-load should appear at the load's position. */
        state.setDisplayedValue(target);
        state.markSeen();
    } else {
        const float animationTime = std::max(spec.fillAnimationSeconds, 0.0f);
        state.setDisplayedValue(animationTime <= 0.0f
            ? target
            : theme::interpConstantTo(state.displayedValue(), target,
                                      context.deltaSeconds(), 1.0f / animationTime));
    }
    const float displayed = state.displayedValue();

    UiDrawList& drawList = context.drawList();
    const float cornerRadius = spec.roundedEnds ? height * 0.5f : 0.0f;

    /* Painter's order: the fill's halo, then the track, then the fill. The halo
     * goes under the TRACK as well as under the fill, which is what stops a
     * bright fill's spill from washing out the groove immediately ahead of
     * it. */
    const float fillWidth = spec.roundedEnds
        ? std::max(height, barRect.width * displayed)
        : barRect.width * displayed;
    const UiRect fillRect{ barRect.x, barRect.y, fillWidth, height };

    if (displayed > kEpsilon
        && glow::edgeAlpha(spec.glowStrength, spec.fillColour.a) > 0.0f
        && spec.glowRadiusPx > 0.0f) {
        Outline outline;
        if (spec.roundedEnds) {
            /* A capsule rather than a rounded rect: the two are the same shape
             * at this radius, and the capsule builder needs no corner
             * clamping. */
            const float halfHeight = height * 0.5f;
            const float centreY = fillRect.y + halfHeight;
            outline.buildCapsule({ fillRect.x + halfHeight, centreY },
                                 { fillRect.right() - halfHeight, centreY },
                                 halfHeight, 8);
        } else {
            outline.buildRect(fillRect, 0.0f, 8);
        }
        glow::addClosedOutline(drawList, outline, spec.glowRadiusPx, spec.fillColour, spec.glowStrength);
    }

    shapes::addRoundedRect(drawList, barRect, cornerRadius, spec.trackColour, shapes::kFeatherPx);

    if (displayed > kEpsilon) {
        shapes::addRoundedRect(drawList, fillRect, cornerRadius, spec.fillColour, shapes::kFeatherPx);
    }
}

}  // namespace cromwell::ui
