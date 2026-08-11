#include "cromwell/ui/loader/LoadingRing.hpp"

#include "cromwell/ui/shape/Glow.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-4f;

}  // namespace

void drawLoadingRing(UiContext& context, Vec2 centre, const LoadingRingSpec& spec)
{
    const float outerRadius = std::max(spec.radiusPx, 4.0f);
    const float halfThickness = std::clamp(spec.thicknessPx * 0.5f, 0.5f, outerRadius * 0.5f);
    const float midRadius = outerRadius - halfThickness;
    const float innerRadius = midRadius - halfThickness;

    /* One smooth cycle per period. What the clock DRIVES depends on the style:
     * Spin rotates a fixed arc, Fill grows one, Progress ignores it entirely. */
    const float period = std::max(spec.periodSeconds, 0.05f);
    const float turn = static_cast<float>(std::fmod(context.time() / static_cast<double>(period), 1.0));

    float startAngle = -0.5f * kPi;
    float sweep = 0.0f;
    switch (spec.style) {
    case LoadingRingStyle::Spin:
        startAngle += 2.0f * kPi * turn;
        sweep = std::clamp(spec.sweepDegrees, 10.0f, 360.0f) * kPi / 180.0f;
        break;
    case LoadingRingStyle::Fill:
        sweep = 2.0f * kPi * turn;
        break;
    case LoadingRingStyle::Progress:
        sweep = 2.0f * kPi * std::clamp(spec.progress, 0.0f, 1.0f);
        break;
    }

    UiDrawList& drawList = context.drawList();

    /* Painter's order inside the one draw list: track, then the arc's halo,
     * then the crisp arc on top. */
    shapes::addAnnularBand(drawList, centre, innerRadius, outerRadius,
                           0.0f, 2.0f * kPi, shapes::kCircleSamples,
                           spec.trackColour, shapes::kFeatherPx);

    if (sweep <= kEpsilon) {
        return;
    }

    const int samples = shapes::arcSamples(sweep);
    const bool hasCaps = sweep < 2.0f * kPi - kEpsilon;

    /* The halo, as two soft passes of the same band at the full and half glow
     * radius. Their alphas are half the peak each, so where they overlap at the
     * arc's edge they sum to it — the same two-step falloff Glow.hpp builds
     * from rings, expressed here as passes because the shape is an arc rather
     * than a closed outline. */
    const float peakAlpha = glow::edgeAlpha(spec.glowStrength, spec.arcColour.a);
    if (peakAlpha > 0.0f && spec.glowRadiusPx > 0.0f) {
        const UiColor glowColour = spec.arcColour.withAlpha(peakAlpha * 0.5f);
        for (const float passRadius : { spec.glowRadiusPx, spec.glowRadiusPx * 0.5f }) {
            shapes::addAnnularBand(drawList, centre, innerRadius, outerRadius,
                                   startAngle, startAngle + sweep, samples,
                                   glowColour, passRadius);
            if (hasCaps) {
                const Vec2 endCentre = centre + Vec2::fromAngle(startAngle + sweep) * midRadius;
                const Vec2 startCentre = centre + Vec2::fromAngle(startAngle) * midRadius;
                shapes::addRoundCap(drawList, endCentre, halfThickness,
                                    startAngle + sweep, 1.0f, glowColour, passRadius);
                shapes::addRoundCap(drawList, startCentre, halfThickness,
                                    startAngle, -1.0f, glowColour, passRadius);
            }
        }
    }

    shapes::addAnnularBand(drawList, centre, innerRadius, outerRadius,
                           startAngle, startAngle + sweep, samples,
                           spec.arcColour, shapes::kFeatherPx);
    if (hasCaps) {
        const Vec2 endCentre = centre + Vec2::fromAngle(startAngle + sweep) * midRadius;
        const Vec2 startCentre = centre + Vec2::fromAngle(startAngle) * midRadius;
        shapes::addRoundCap(drawList, endCentre, halfThickness,
                            startAngle + sweep, 1.0f, spec.arcColour, shapes::kFeatherPx);
        shapes::addRoundCap(drawList, startCentre, halfThickness,
                            startAngle, -1.0f, spec.arcColour, shapes::kFeatherPx);
    }
}

}  // namespace cromwell::ui
