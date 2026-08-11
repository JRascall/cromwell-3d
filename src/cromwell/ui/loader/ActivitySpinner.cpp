#include "cromwell/ui/loader/ActivitySpinner.hpp"

#include "cromwell/ui/shape/Glow.hpp"
#include "cromwell/ui/shape/Outline.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/* Apple's proportions, off the outer radius. */
constexpr float kInnerRadiusFraction = 0.42f;
constexpr float kSpokeHalfWidthFraction = 0.10f;

/* The dimmest a trailing spoke gets. Not zero: a spoke that vanishes entirely
 * makes the ring look like it has a hole in it as the highlight passes. */
constexpr float kTailFloor = 0.12f;

constexpr int kCapSegments = 8;

}  // namespace

void drawActivitySpinner(UiContext& context, Vec2 centre, const ActivitySpinnerSpec& spec)
{
    const float outerRadius = std::max(spec.radiusPx, 2.0f);
    const int spokeCount = std::clamp(spec.spokeCount, 3, 32);
    const float period = std::max(spec.periodSeconds, 0.05f);

    const float innerRadius = outerRadius * kInnerRadiusFraction;
    const float halfWidth = std::max(0.5f, outerRadius * kSpokeHalfWidthFraction);

    /* The stepped clock — see the header. Integer division of the cycle into
     * spokeCount positions is what produces the tick. */
    const double cycles = context.time() / static_cast<double>(period);
    const int step = static_cast<int>(cycles * spokeCount) % spokeCount;

    UiDrawList& drawList = context.drawList();
    Outline outline;

    for (int index = 0; index < spokeCount; ++index) {
        /* How far behind the bright spoke this one sits, 0 = brightest, then a
         * linear ramp down the tail. The modulo is written the long way because
         * C++ gives a negative remainder for a negative left operand and the
         * tail would invert for half the ring. */
        const int tailPosition = ((step - index) % spokeCount + spokeCount) % spokeCount;
        const float fade = 1.0f + (kTailFloor - 1.0f)
            * static_cast<float>(tailPosition) / static_cast<float>(std::max(spokeCount - 1, 1));

        const UiColor spokeColour = spec.colour.scaledAlpha(fade);

        /* Spoke axis: up at index 0, advancing clockwise. The cap centres are
         * inset by the half-width at each end so the capsule's rounded ends
         * land exactly on the inner and outer radii. */
        const float psi = -0.5f * kPi + 2.0f * kPi * static_cast<float>(index) / static_cast<float>(spokeCount);
        const Vec2 direction = Vec2::fromAngle(psi);
        const Vec2 capInner = centre + direction * (innerRadius + halfWidth);
        const Vec2 capOuter = centre + direction * (outerRadius - halfWidth);

        outline.buildCapsule(capInner, capOuter, halfWidth, kCapSegments);

        /* Halo first so it sits under the spoke — see Glow.hpp. */
        glow::addClosedOutline(drawList, outline, spec.glowRadiusPx, spokeColour, spec.glowStrength);
        shapes::addConvexFill(drawList, outline, spokeColour, shapes::kFeatherPx);
    }
}

}  // namespace cromwell::ui
