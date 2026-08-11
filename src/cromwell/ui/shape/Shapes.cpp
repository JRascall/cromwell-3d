#include "cromwell/ui/shape/Shapes.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell::ui::shapes {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/* Below this, an angle sweep or a radius is not worth emitting geometry for.
 * Matches Slate's KINDA_SMALL_NUMBER, which is what the originals tested
 * against. */
constexpr float kEpsilon = 1.0e-4f;

}  // namespace

int arcSamples(float sweepRadians, int minimum)
{
    const float fraction = std::abs(sweepRadians) / (2.0f * kPi);
    const int scaled = static_cast<int>(std::ceil(static_cast<float>(kCircleSamples) * fraction));
    return std::max(minimum, scaled);
}

void addAnnularBand(UiDrawList& drawList, Vec2 centre,
                    float innerRadius, float outerRadius,
                    float fromAngle, float toAngle, int samples,
                    const UiColor& colour, float falloff)
{
    samples = std::max(samples, 1);
    if (outerRadius <= innerRadius) {
        return;
    }

    const UiColor edge = colour.toEdge();
    const float softInner = std::max(innerRadius - falloff, 0.0f);
    const float softOuter = outerRadius + falloff;

    const std::uint32_t base = drawList.vertexCount();
    for (int sample = 0; sample <= samples; ++sample) {
        const float t = fromAngle + (toAngle - fromAngle) * static_cast<float>(sample) / static_cast<float>(samples);
        const Vec2 direction = Vec2::fromAngle(t);

        /* Four vertices per column, innermost first. The strip loop below
         * assumes exactly this order and this count. */
        drawList.addVertex(centre + direction * softInner, edge);
        drawList.addVertex(centre + direction * innerRadius, colour);
        drawList.addVertex(centre + direction * outerRadius, colour);
        drawList.addVertex(centre + direction * softOuter, edge);

        if (sample > 0) {
            const std::uint32_t previous = base + static_cast<std::uint32_t>(sample - 1) * 4;
            const std::uint32_t current = base + static_cast<std::uint32_t>(sample) * 4;
            for (std::uint32_t strip = 0; strip < 3; ++strip) {
                drawList.addQuad(previous + strip, current + strip,
                                 current + strip + 1, previous + strip + 1);
            }
        }
    }
}

void addRoundCap(UiDrawList& drawList, Vec2 capCentre, float capRadius,
                 float atAngle, float bulgeSign,
                 const UiColor& colour, float falloff)
{
    if (capRadius <= 0.0f) {
        return;
    }

    constexpr int kCapSegments = 8;
    const UiColor edge = colour.toEdge();
    const std::uint32_t base = drawList.vertexCount();

    /* The semicircle's normals, kept so the feather ring can reuse them rather
     * than recompute the same eight cosines. */
    Vec2 normals[kCapSegments + 1];
    for (int segment = 0; segment <= kCapSegments; ++segment) {
        const float t = atAngle + bulgeSign * kPi * static_cast<float>(segment) / static_cast<float>(kCapSegments);
        normals[segment] = Vec2::fromAngle(t);
        drawList.addVertex(capCentre + normals[segment] * capRadius, colour);
    }
    for (int segment = 0; segment <= kCapSegments; ++segment) {
        drawList.addVertex(capCentre + normals[segment] * (capRadius + falloff), edge);
    }

    /* Fan the solid half-disc from the first point on the rim, then quad the
     * feather ring outward from each rim edge. */
    for (std::uint32_t segment = 1; segment < kCapSegments; ++segment) {
        drawList.addTriangle(base, base + segment, base + segment + 1);
    }
    for (std::uint32_t segment = 0; segment < kCapSegments; ++segment) {
        const std::uint32_t outer = base + kCapSegments + 1;
        drawList.addQuad(base + segment, base + segment + 1,
                         outer + segment + 1, outer + segment);
    }
}

void addDisc(UiDrawList& drawList, Vec2 centre, float radius,
             const UiColor& colour, float falloff)
{
    if (radius <= 0.5f) {
        return;
    }

    const UiColor edge = colour.toEdge();
    const std::uint32_t base = drawList.vertexCount();

    drawList.addVertex(centre, colour);
    for (int sample = 0; sample <= kCircleSamples; ++sample) {
        const float t = 2.0f * kPi * static_cast<float>(sample) / static_cast<float>(kCircleSamples);
        const Vec2 direction = Vec2::fromAngle(t);
        drawList.addVertex(centre + direction * radius, colour);
        drawList.addVertex(centre + direction * (radius + falloff), edge);
    }

    for (std::uint32_t sample = 1; sample <= kCircleSamples; ++sample) {
        const std::uint32_t previous = base + 1 + (sample - 1) * 2;
        const std::uint32_t current = base + 1 + sample * 2;
        drawList.addTriangle(base, previous, current);
        drawList.addQuad(previous, current, current + 1, previous + 1);
    }
}

void addConvexFill(UiDrawList& drawList, const Outline& outline,
                   const UiColor& colour, float falloff)
{
    const std::size_t count = outline.size();
    if (count < 3) {
        return;
    }

    const UiColor edge = colour.toEdge();
    const std::uint32_t base = drawList.vertexCount();

    for (std::size_t point = 0; point < count; ++point) {
        drawList.addVertex(outline.position(point), colour);
    }
    for (std::size_t point = 0; point < count; ++point) {
        drawList.addVertex(outline.offsetPosition(point, falloff), edge);
    }

    /* Fan the interior from point 0 — valid because the outline is convex (see
     * Outline.hpp), and cheaper than a triangulation that would have to prove
     * it is not. */
    const auto pointCount = static_cast<std::uint32_t>(count);
    for (std::uint32_t point = 1; point + 1 < pointCount; ++point) {
        drawList.addTriangle(base, base + point, base + point + 1);
    }
    for (std::uint32_t point = 0; point < pointCount; ++point) {
        const std::uint32_t next = (point + 1) % pointCount;
        drawList.addQuad(base + point, base + next, base + pointCount + next, base + pointCount + point);
    }
}

void addOutlineStroke(UiDrawList& drawList, const Outline& outline, float thickness,
                      const UiColor& colour, float falloff)
{
    const std::size_t count = outline.size();
    if (count < 3 || thickness <= 0.0f) {
        return;
    }

    const UiColor edge = colour.toEdge();

    /* Four rings, outermost first: the outer feather, the outer edge of the
     * stroke, its inner edge, and the inner feather. Only the middle band is
     * solid. */
    const float offsets[4] = { falloff, 0.0f, -thickness, -thickness - falloff };
    const UiColor colours[4] = { edge, colour, colour, edge };

    const std::uint32_t base = drawList.vertexCount();
    for (int ring = 0; ring < 4; ++ring) {
        for (std::size_t point = 0; point < count; ++point) {
            drawList.addVertex(outline.offsetPosition(point, offsets[ring]), colours[ring]);
        }
    }

    const auto pointCount = static_cast<std::uint32_t>(count);
    for (std::uint32_t ring = 0; ring < 3; ++ring) {
        const std::uint32_t outer = base + ring * pointCount;
        const std::uint32_t inner = outer + pointCount;
        for (std::uint32_t point = 0; point < pointCount; ++point) {
            const std::uint32_t next = (point + 1) % pointCount;
            drawList.addQuad(outer + point, outer + next, inner + next, inner + point);
        }
    }
}

void addRect(UiDrawList& drawList, const UiRect& rect, const UiColor& colour)
{
    if (rect.empty() || colour.a <= kEpsilon) {
        return;
    }

    /* SNAPPED TO WHOLE DEVICE PIXELS, and this is the entire reason this
     * builder exists separately from the feathered ones.
     *
     * A hard edge at a fractional coordinate is not a hard edge: a 1px divider
     * at y = 100.5 rasterises as two rows at half intensity, which reads as
     * grey mush where a crisp line was intended. It is easy to miss at a
     * display scale of 1 — plenty of layouts happen to land on integers — and
     * it is guaranteed at 1.25 or 1.5, where almost nothing does.
     *
     * The feathered builders do NOT do this and must not: their soft band is
     * what antialiases a shape at any subpixel position, and snapping them
     * would make curves and slants jitter as they move. This is only ever
     * right for something axis-aligned that wants to be sharp — which is
     * exactly what this function is for. */
    const float left = std::round(rect.left());
    const float top = std::round(rect.top());
    float right = std::round(rect.right());
    float bottom = std::round(rect.bottom());

    /* A sub-pixel rect must not round away to nothing — a hairline asked for
     * at 0.4px is still a request for a line. Give it the one pixel it takes
     * to be visible rather than dropping it. */
    if (right <= left) { right = left + 1.0f; }
    if (bottom <= top) { bottom = top + 1.0f; }

    const std::uint32_t topLeft = drawList.addVertex({ left, top }, colour);
    const std::uint32_t topRight = drawList.addVertex({ right, top }, colour);
    const std::uint32_t bottomRight = drawList.addVertex({ right, bottom }, colour);
    const std::uint32_t bottomLeft = drawList.addVertex({ left, bottom }, colour);
    drawList.addQuad(topLeft, topRight, bottomRight, bottomLeft);
}

void addRoundedRect(UiDrawList& drawList, const UiRect& rect, float cornerRadius,
                    const UiColor& colour, float falloff)
{
    const float radii[4] = { cornerRadius, cornerRadius, cornerRadius, cornerRadius };
    addRoundedRect(drawList, rect, radii, colour, falloff);
}

void addRoundedRect(UiDrawList& drawList, const UiRect& rect, const float cornerRadii[4],
                    const UiColor& colour, float falloff)
{
    if (rect.empty() || colour.a <= kEpsilon) {
        return;
    }

    const bool square = cornerRadii[0] <= 0.0f && cornerRadii[1] <= 0.0f
                     && cornerRadii[2] <= 0.0f && cornerRadii[3] <= 0.0f;
    if (square) {
        addRect(drawList, rect, colour);
        return;
    }

    /* Eight segments per corner is smooth to well past the radii UI uses, and
     * the whole shape is one convex outline, so the fill and its feather come
     * straight from the shared builder. */
    Outline outline;
    outline.buildRect(rect, cornerRadii, 8);
    addConvexFill(drawList, outline, colour, falloff);
}

}  // namespace cromwell::ui::shapes
