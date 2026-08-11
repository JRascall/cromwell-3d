#include "cromwell/ui/shape/Glow.hpp"

namespace cromwell::ui::glow {

void addClosedOutline(UiDrawList& drawList, const Outline& outline,
                      float glowRadius, const UiColor& shapeColour, float strength)
{
    const float peakAlpha = edgeAlpha(strength, shapeColour.a);
    if (peakAlpha <= 0.0f || glowRadius <= 0.0f || outline.size() < 3) {
        return;
    }

    /* The two-step falloff: full peak hugging the edge, a little over a third of
     * it at 40% out, nothing at the rim. */
    const UiColor ringColours[3] = {
        shapeColour.withAlpha(peakAlpha),
        shapeColour.withAlpha(peakAlpha * 0.35f),
        shapeColour.withAlpha(0.0f),
    };
    const float ringOffsets[3] = { 0.0f, glowRadius * 0.4f, glowRadius };

    const std::size_t count = outline.size();
    const std::uint32_t base = drawList.vertexCount();

    for (int ring = 0; ring < 3; ++ring) {
        for (std::size_t point = 0; point < count; ++point) {
            drawList.addVertex(outline.offsetPosition(point, ringOffsets[ring]), ringColours[ring]);
        }
    }

    for (std::uint32_t ring = 0; ring < 2; ++ring) {
        const std::uint32_t inner = base + ring * static_cast<std::uint32_t>(count);
        const std::uint32_t outer = inner + static_cast<std::uint32_t>(count);
        for (std::uint32_t point = 0; point < static_cast<std::uint32_t>(count); ++point) {
            const std::uint32_t next = (point + 1) % static_cast<std::uint32_t>(count);
            drawList.addQuad(inner + point, inner + next, outer + next, outer + point);
        }
    }
}

}  // namespace cromwell::ui::glow
