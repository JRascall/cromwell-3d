#include "cromwell/ui/gauge/SegmentRing.hpp"

#include "cromwell/ui/shape/Glow.hpp"
#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cromwell::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-4f;

/* Everything one chip band needs to know about the gauge it belongs to.
 * Assembled once per draw and handed to the band builder, which would otherwise
 * take eleven arguments. */
struct ChipGeometry {
    Vec2  centre;
    float innerRadius = 0.0f;
    float midRadius = 0.0f;
    float outerRadius = 0.0f;
    float gapHalfWidth = 0.0f;
};

/* Where a chip's straight cut edge sits at radius `radius`: the line parallel
 * to the radial line at `boundaryAngle`, offset INTO the chip by `halfGap`.
 * `sign` is +1 where the chip begins and -1 where it ends. A half-gap of zero
 * degrades to a plain radial cut, which is what a single-segment ring wants.
 *
 * This asin is the whole trick — see the header note on why a radial wedge cut
 * is not good enough. */
float cutAngle(float boundaryAngle, float halfGap, float sign, float radius)
{
    const float ratio = std::min(halfGap / std::max(radius, kEpsilon), 1.0f);
    return boundaryAngle + sign * std::asin(ratio);
}

/* One chip's band between boundary lines `phi0` and `phi1`, optionally stopped
 * early at `endLimit` — which is how a part-filled chip's fill arc ends
 * mid-chip.
 *
 * The radial cross-section per column is the same four vertices the plain
 * annular band uses (soft-inner, inner, outer, soft-outer). What differs is
 * that each column's ANGLE is clamped per-vertex to the cut edges at that
 * vertex's radius, and that is what makes the chip ends straight lines rather
 * than arc-normal radial edges.
 *
 * `tangentialFeather` of fade is added past each end, subdivided into columns
 * so that a wide fade — the glow passes reuse this builder — follows the arc
 * instead of cutting a chord across it. */
void addChipBand(UiDrawList& drawList, const ChipGeometry& geometry,
                 float phi0, float phi1, float endLimit,
                 const UiColor& colour, float radialFalloff, float tangentialFeather)
{
    const float radii[4] = {
        std::max(geometry.innerRadius - radialFalloff, 0.0f),
        geometry.innerRadius,
        geometry.outerRadius,
        geometry.outerRadius + radialFalloff,
    };

    const auto startEdge = [&](float radius) {
        return cutAngle(phi0, geometry.gapHalfWidth, 1.0f, radius);
    };
    const auto endEdge = [&](float radius) {
        return std::min(endLimit, cutAngle(phi1, geometry.gapHalfWidth, -1.0f, radius));
    };

    const float sweepStart = startEdge(geometry.midRadius);
    const float sweepEnd = endEdge(geometry.midRadius);
    const float sweep = sweepEnd - sweepStart;
    if (sweep <= kEpsilon) {
        return;
    }
    const bool closed = sweep >= 2.0f * kPi - kEpsilon;

    const int samples = shapes::arcSamples(sweep, 4);

    /* How many columns the tangential fade gets. Proportional to how far it
     * spreads in ANGLE, so a one-pixel feather on a large ring is one column
     * and an eight-pixel glow on a small one is several. */
    const int featherColumns = (!closed && tangentialFeather > 0.0f)
        ? std::clamp(static_cast<int>(std::ceil(
              static_cast<float>(shapes::kCircleSamples)
              * (tangentialFeather / std::max(geometry.midRadius, 1.0f)) / (2.0f * kPi))), 1, 8)
        : 0;

    const UiColor edge = colour.toEdge();
    const std::uint32_t base = drawList.vertexCount();
    int columnCount = 0;

    /* `edgeMode` -1 or +1 places the column `offsetPx` past the start or end
     * edge; 0 places it at `angle`, clamped into the chip. `weight` scales the
     * core alpha, which is what ramps the fade out. */
    const auto addColumn = [&](float angle, float weight, int edgeMode, float offsetPx) {
        const UiColor core = colour.scaledAlpha(std::clamp(weight, 0.0f, 1.0f));
        for (int vertex = 0; vertex < 4; ++vertex) {
            const float radius = radii[vertex];
            float t = 0.0f;
            if (edgeMode < 0) {
                t = startEdge(radius) - offsetPx / std::max(radius, 1.0f);
            } else if (edgeMode > 0) {
                t = endEdge(radius) + offsetPx / std::max(radius, 1.0f);
            } else {
                const float low = startEdge(radius);
                t = std::clamp(angle, low, std::max(low, endEdge(radius)));
            }
            const bool isSoftEdge = (vertex == 0 || vertex == 3);
            drawList.addVertex(geometry.centre + Vec2::fromAngle(t) * radius, isSoftEdge ? edge : core);
        }
        ++columnCount;
    };

    for (int column = 0; column < featherColumns; ++column) {
        const float fraction = static_cast<float>(column) / static_cast<float>(featherColumns);
        addColumn(0.0f, fraction, -1, tangentialFeather * (1.0f - fraction));
    }
    for (int sample = 0; sample <= samples; ++sample) {
        const float t = sweepStart + sweep * static_cast<float>(sample) / static_cast<float>(samples);
        addColumn(t, 1.0f, 0, 0.0f);
    }
    for (int column = 1; column <= featherColumns; ++column) {
        const float fraction = static_cast<float>(column) / static_cast<float>(featherColumns);
        addColumn(0.0f, 1.0f - fraction, 1, tangentialFeather * fraction);
    }

    for (int column = 1; column < columnCount; ++column) {
        const std::uint32_t previous = base + static_cast<std::uint32_t>(column - 1) * 4;
        const std::uint32_t current = base + static_cast<std::uint32_t>(column) * 4;
        for (std::uint32_t strip = 0; strip < 3; ++strip) {
            drawList.addQuad(previous + strip, current + strip,
                             current + strip + 1, previous + strip + 1);
        }
    }
}

}  // namespace

void drawSegmentRing(UiContext& context, Vec2 centre, const SegmentRingSpec& spec)
{
    ChipGeometry geometry;
    geometry.centre = centre;
    geometry.outerRadius = std::max(spec.radiusPx, 4.0f);

    const float halfThickness = std::clamp(spec.thicknessPx * 0.5f, 0.5f, geometry.outerRadius * 0.5f);
    geometry.midRadius = geometry.outerRadius - halfThickness;
    geometry.innerRadius = geometry.midRadius - halfThickness;

    const int segments = std::clamp(spec.segmentCount, 1, 64);
    const float stride = 2.0f * kPi / static_cast<float>(segments);
    const float startAngle = -0.5f * kPi + spec.startAngleDegrees * kPi / 180.0f;

    /* The gap is clamped to a quarter-stride's worth of chord so that a large
     * gap on a many-segment ring narrows the chips rather than inverting
     * them. */
    geometry.gapHalfWidth = segments > 1
        ? std::min(std::max(spec.gapPx, 0.0f) * 0.5f, geometry.midRadius * std::sin(stride * 0.25f))
        : 0.0f;

    const float fillSweep = 2.0f * kPi * std::clamp(spec.progress, 0.0f, 1.0f);
    const float fillEnd = startAngle + fillSweep;
    constexpr float kNoLimit = std::numeric_limits<float>::max();

    UiDrawList& drawList = context.drawList();

    /* Painter's order: the backing plate, every chip's track, the fill's halo,
     * then the crisp fill on top. */
    if (spec.discColour.a > kEpsilon) {
        shapes::addDisc(drawList, centre, geometry.outerRadius, spec.discColour, shapes::kFeatherPx);
    }

    for (int segment = 0; segment < segments; ++segment) {
        const float phi0 = startAngle + static_cast<float>(segment) * stride;
        addChipBand(drawList, geometry, phi0, phi0 + stride, kNoLimit,
                    spec.trackColour, shapes::kFeatherPx, shapes::kFeatherPx);
    }

    if (fillSweep > kEpsilon) {
        /* Two soft passes at the full and half glow radius, each at half the
         * peak alpha, so they sum to the peak at the fill's edge — the same
         * two-step falloff as the loading ring, for the same reason. */
        const float peakAlpha = glow::edgeAlpha(spec.glowStrength, spec.fillColour.a);
        if (peakAlpha > 0.0f && spec.glowRadiusPx > 0.0f) {
            const UiColor glowColour = spec.fillColour.withAlpha(peakAlpha * 0.5f);
            for (const float passRadius : { spec.glowRadiusPx, spec.glowRadiusPx * 0.5f }) {
                for (int segment = 0; segment < segments; ++segment) {
                    const float phi0 = startAngle + static_cast<float>(segment) * stride;
                    addChipBand(drawList, geometry, phi0, phi0 + stride, fillEnd,
                                glowColour, passRadius, passRadius);
                }
            }
        }

        for (int segment = 0; segment < segments; ++segment) {
            const float phi0 = startAngle + static_cast<float>(segment) * stride;
            addChipBand(drawList, geometry, phi0, phi0 + stride, fillEnd,
                        spec.fillColour, shapes::kFeatherPx, shapes::kFeatherPx);
        }
    }

    if (!spec.centreText.empty()) {
        const Vec2 size = context.metrics().measure(spec.centreText, spec.centreStyle);
        TextRun run;
        run.text = spec.centreText;
        run.position = { centre.x - size.x * 0.5f, centre.y - size.y * 0.5f };
        run.style = spec.centreStyle;
        drawList.addText(std::move(run));
    }
}

}  // namespace cromwell::ui
