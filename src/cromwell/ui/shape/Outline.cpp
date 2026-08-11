#include "cromwell/ui/shape/Outline.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

void Outline::clear()
{
    positions_.clear();
    normals_.clear();
}

void Outline::reset(std::size_t expectedPoints)
{
    clear();
    positions_.reserve(expectedPoints);
    normals_.reserve(expectedPoints);
}

void Outline::add(Vec2 position, Vec2 normal)
{
    positions_.push_back(position);
    normals_.push_back(normal);
}

void Outline::buildCapsule(Vec2 capA, Vec2 capB, float radius, int capSegments)
{
    capSegments = std::max(capSegments, 1);
    reset(static_cast<std::size_t>(2 * (capSegments + 1)));

    /* Axis angle. A zero-length axis gives atan2(0,0) = 0, which walks the two
     * half-circles as one full circle around the shared centre — exactly the
     * right degenerate answer. */
    const Vec2 axis = capB - capA;
    const float psi = std::atan2(axis.y, axis.x);

    /* Half the ring around capB, from one side of the axis to the other, then
     * the mirrored half around capA. Walking them in this order keeps the whole
     * ring clockwise. */
    for (int segment = 0; segment <= capSegments; ++segment) {
        const float t = psi - 0.5f * kPi + kPi * static_cast<float>(segment) / static_cast<float>(capSegments);
        const Vec2 normal = Vec2::fromAngle(t);
        add(capB + normal * radius, normal);
    }
    for (int segment = 0; segment <= capSegments; ++segment) {
        const float t = psi + 0.5f * kPi + kPi * static_cast<float>(segment) / static_cast<float>(capSegments);
        const Vec2 normal = Vec2::fromAngle(t);
        add(capA + normal * radius, normal);
    }
}

void Outline::buildRect(const UiRect& rect, float cornerRadius, int cornerSegments)
{
    const float radii[4] = { cornerRadius, cornerRadius, cornerRadius, cornerRadius };
    buildRect(rect, radii, cornerSegments);
}

void Outline::buildRect(const UiRect& rect, const float cornerRadii[4], int cornerSegments)
{
    cornerSegments = std::max(cornerSegments, 1);

    /* Each radius is capped at half the shorter side, so an over-large radius
     * degrades to a stadium rather than folding the outline inside out. */
    const float limit = std::min(rect.width, rect.height) * 0.5f;
    const float topLeft = std::clamp(cornerRadii[0], 0.0f, limit);
    const float topRight = std::clamp(cornerRadii[1], 0.0f, limit);
    const float bottomRight = std::clamp(cornerRadii[2], 0.0f, limit);
    const float bottomLeft = std::clamp(cornerRadii[3], 0.0f, limit);

    reset(static_cast<std::size_t>(4 * (cornerSegments + 1)));

    /* Corner arc centres and their start angles, walking TR -> BR -> BL -> TL,
     * which is clockwise in y-down screen space. */
    const float walkRadii[4] = { topRight, bottomRight, bottomLeft, topLeft };
    const Vec2 centres[4] = {
        { rect.x + rect.width - topRight, rect.y + topRight },
        { rect.x + rect.width - bottomRight, rect.y + rect.height - bottomRight },
        { rect.x + bottomLeft, rect.y + rect.height - bottomLeft },
        { rect.x + topLeft, rect.y + topLeft },
    };
    const float startAngles[4] = { -0.5f * kPi, 0.0f, 0.5f * kPi, kPi };

    for (int corner = 0; corner < 4; ++corner) {
        for (int segment = 0; segment <= cornerSegments; ++segment) {
            const float t = startAngles[corner]
                          + 0.5f * kPi * static_cast<float>(segment) / static_cast<float>(cornerSegments);
            const Vec2 normal = Vec2::fromAngle(t);
            add(centres[corner] + normal * walkRadii[corner], normal);
        }
    }
}

void Outline::buildConvexPolygon(const Vec2* points, int count)
{
    if (points == nullptr || count < 3) {
        clear();
        return;
    }

    reset(static_cast<std::size_t>(3 * count));

    /* Outward normal of each edge, from the edge direction rotated a quarter
     * turn — correct for clockwise winding in y-down space. */
    std::vector<Vec2> edgeNormals(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const Vec2 direction = points[(index + 1) % count] - points[index];
        edgeNormals[static_cast<std::size_t>(index)] = direction.perpendicular().normalised();
    }

    for (int index = 0; index < count; ++index) {
        const Vec2 incoming = edgeNormals[static_cast<std::size_t>((index + count - 1) % count)];
        const Vec2 outgoing = edgeNormals[static_cast<std::size_t>(index)];
        const Vec2 mitre = (incoming + outgoing).normalised();

        /* Three points at the same position with rotating normals. Offsetting
         * them fans the ring around the corner; without the mitre in the middle
         * a sharp corner's halo would cut a chord across it. */
        add(points[index], incoming);
        add(points[index], mitre);
        add(points[index], outgoing);
    }
}

}  // namespace cromwell::ui
