#include "cromwell/collision/Intersect.hpp"

#include <algorithm>
#include <cmath>

namespace cromwell {

namespace {

/* Small enough not to matter at world scale, large enough to absorb the error
 * in a normalised direction. Everything parallel-to-a-slab is decided against
 * this. */
constexpr float kEpsilon = 1.0e-6f;

float axisOf(Vec3 v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

void setAxis(Vec3& v, int axis, float value)
{
    if (axis == 0) v.x = value;
    else if (axis == 1) v.y = value;
    else v.z = value;
}

Vec3 axisNormal(int axis, float sign)
{
    Vec3 normal = Vec3::zero();
    setAxis(normal, axis, sign);
    return normal;
}

Vec3 clampToBox(Vec3 point, const Aabb& box)
{
    return Vec3{ std::clamp(point.x, box.min.x, box.max.x),
                 std::clamp(point.y, box.min.y, box.max.y),
                 std::clamp(point.z, box.min.z, box.max.z) };
}

/* The shallowest way out of a box the point is already inside. Six candidate
 * pushes — one per face — and the smallest wins.
 *
 * SHALLOWEST, NOT NEAREST-FACE-BY-CENTRE. They differ for a flat box, and it is
 * the shallowest that a depenetration step wants: pushing a character out
 * through the thin axis of a floor it has sunk into moves it centimetres, while
 * pushing it toward the "nearest" face of a wide slab can move it metres
 * sideways and through the wall behind. */
void shallowestSeparation(Vec3 point, const Aabb& box, Vec3& normal, float& depth)
{
    normal = Vec3{ 0.0f, 1.0f, 0.0f };
    depth = 0.0f;

    bool first = true;
    for (int axis = 0; axis < 3; ++axis) {
        const float value = axisOf(point, axis);
        const float toMin = value - axisOf(box.min, axis);  /* push toward -axis */
        const float toMax = axisOf(box.max, axis) - value;  /* push toward +axis */

        const bool towardMax = toMax < toMin;
        const float candidate = towardMax ? toMax : toMin;

        if (first || candidate < depth) {
            depth = candidate;
            normal = axisNormal(axis, towardMax ? 1.0f : -1.0f);
            first = false;
        }
    }
}

/* The slab test, over a box already expanded by the moving shape's extents.
 *
 * Returns the entry distance and the axis and sign of the face entered through,
 * or false for a miss. The entry axis is a by-product of the test rather than
 * something recovered afterwards from the contact point — recovering it costs a
 * comparison against every face and gets the wrong answer at exactly the corners
 * where it matters. */
bool slabEntry(Vec3 start, Vec3 direction, float maxDistance, const Aabb& box,
               float& entryDistance, int& entryAxis, float& entrySign)
{
    float enter = 0.0f;
    float exit = maxDistance;

    entryAxis = 1;
    entrySign = 1.0f;

    for (int axis = 0; axis < 3; ++axis) {
        const float d = axisOf(direction, axis);
        const float o = axisOf(start, axis);
        const float low = axisOf(box.min, axis);
        const float high = axisOf(box.max, axis);

        if (std::abs(d) < kEpsilon) {
            /* Parallel to this pair of slabs: either always between them or
             * never. */
            if (o < low || o > high) return false;
            continue;
        }

        const float inverse = 1.0f / d;
        float near = (low - o) * inverse;
        float far = (high - o) * inverse;
        float sign = -1.0f;  /* entering the min face means the normal points -axis */
        if (near > far) {
            std::swap(near, far);
            sign = 1.0f;
        }

        if (near > enter) {
            enter = near;
            entryAxis = axis;
            entrySign = sign;
        }
        exit = std::min(exit, far);

        /* Strict, so a grazing pass along a face is not a contact — see the
         * header. */
        if (enter > exit) return false;
    }

    entryDistance = enter;
    return true;
}

}  // namespace

SweepContact sweepBox(Vec3 start, Vec3 halfExtents, Vec3 direction, float maxDistance,
                      const Aabb& target)
{
    SweepContact contact;

    /* THE WHOLE TRICK, in one line — see the header. */
    const Aabb expanded = target.expandedBy(halfExtents);

    if (expanded.contains(start)) {
        /* Already overlapping. Report it as such rather than as a hit at zero:
         * the direction the caller should be pushed is a property of how deep it
         * is, not of where it was heading. */
        contact.hit = true;
        contact.startPenetrating = true;
        contact.distance = 0.0f;
        contact.end = start;

        float depth = 0.0f;
        shallowestSeparation(start, expanded, contact.normal, depth);
        contact.point = clampToBox(start, target);
        return contact;
    }

    float distance = 0.0f;
    int axis = 0;
    float sign = 1.0f;
    if (!slabEntry(start, direction, maxDistance, expanded, distance, axis, sign)) {
        return contact;
    }

    contact.hit = true;
    contact.distance = distance;
    contact.end = start + direction * distance;
    contact.normal = axisNormal(axis, sign);

    /* The contact point is the shape's centre pulled back onto the static box.
     * For a face contact that is exactly where they touch; for a ray it is the
     * ray's own hit point, since the centre IS on the surface. */
    contact.point = clampToBox(contact.end, target);
    return contact;
}

float raySphereDistance(Vec3 origin, Vec3 direction, Vec3 centre, float radius,
                        float maxDistance)
{
    const Vec3 toCentre = centre - origin;
    const float projection = dot(toCentre, direction);
    const float centreDistanceSquared = toCentre.lengthSquared();
    const float radiusSquared = radius * radius;

    if (centreDistanceSquared <= radiusSquared) return 0.0f;  /* started inside */

    /* Heading away, and outside: nothing to hit. Checked before the discriminant
     * because it rejects half the candidates without a square root. */
    if (projection < 0.0f) return -1.0f;

    const float closestSquared = centreDistanceSquared - projection * projection;
    if (closestSquared > radiusSquared) return -1.0f;

    const float halfChord = std::sqrt(radiusSquared - closestSquared);
    const float distance = projection - halfChord;
    return (distance >= 0.0f && distance <= maxDistance) ? distance : -1.0f;
}

float rayAxisCylinderDistance(Vec3 origin, Vec3 direction, int axis, float centreU,
                              float centreV, float axisMin, float axisMax, float radius,
                              float maxDistance)
{
    /* The two axes perpendicular to the cylinder, in increasing order — the
     * plane the circle lives in. */
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;

    const float ou = axisOf(origin, u) - centreU;
    const float ov = axisOf(origin, v) - centreV;
    const float du = axisOf(direction, u);
    const float dv = axisOf(direction, v);

    const float a = du * du + dv * dv;
    if (a < kEpsilon) {
        /* Travelling parallel to the cylinder's axis: it either never meets the
         * surface or is already inside it, and the caller's corner test covers
         * the second case. */
        return -1.0f;
    }

    const float b = 2.0f * (ou * du + ov * dv);
    const float c = ou * ou + ov * ov - radius * radius;

    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return -1.0f;

    const float root = std::sqrt(discriminant);
    const float first = (-b - root) / (2.0f * a);
    const float second = (-b + root) / (2.0f * a);

    for (const float candidate : { first, second }) {
        if (candidate < 0.0f || candidate > maxDistance) continue;

        /* Inside the finite length? Past either end the true surface is the
         * corner sphere, not this cylinder, and returning a hit here would place
         * the contact off the end of the edge. */
        const float along = axisOf(origin, axis) + axisOf(direction, axis) * candidate;
        if (along < axisMin || along > axisMax) continue;

        return candidate;
    }
    return -1.0f;
}

SweepContact sweepSphere(Vec3 start, float radius, Vec3 direction, float maxDistance,
                         const Aabb& target)
{
    /* THE CHEAP BOUND FIRST. The expanded box contains the true swept volume, so
     * a miss here is a miss for certain and costs one slab test. */
    SweepContact contact = sweepBox(start, Vec3{ radius, radius, radius }, direction,
                                    maxDistance, target);
    if (!contact.hit || contact.startPenetrating) {
        if (contact.hit) {
            /* Penetrating the expanded BOX is not necessarily penetrating the
             * sphere's true volume — the corners stick out. Verify against the
             * real distance before reporting an overlap the caller will try to
             * resolve. */
            const Vec3 closest = clampToBox(start, target);
            if ((start - closest).lengthSquared() > radius * radius) {
                contact = SweepContact{};
            }
        }
        return contact;
    }

    /* Which region of the box the box-sweep contact landed in. An axis is
     * "outside" when the shape's centre is beyond that face; one axis outside is
     * a face contact, two an edge, three a corner. */
    const Vec3 centre = contact.end;
    int outsideAxes = 0;
    int insideAxis = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const float value = axisOf(centre, axis);
        if (value < axisOf(target.min, axis) || value > axisOf(target.max, axis)) {
            ++outsideAxes;
        } else {
            insideAxis = axis;
        }
    }

    if (outsideAxes <= 1) {
        /* FACE CONTACT — the expanded box and the true swept volume share their
         * flat faces exactly, so the box answer is already the sphere answer.
         * This is the overwhelming majority of contacts. */
        return contact;
    }

    float refined = -1.0f;
    if (outsideAxes == 2) {
        /* EDGE. The true surface here is a quarter-cylinder along the one axis
         * that is still inside the box's span. */
        const int u = (insideAxis + 1) % 3;
        const int v = (insideAxis + 2) % 3;

        /* The edge sits at whichever corner of the cross-section the centre is
         * beyond. */
        const float edgeU = axisOf(centre, u) < axisOf(target.min, u)
                                ? axisOf(target.min, u) : axisOf(target.max, u);
        const float edgeV = axisOf(centre, v) < axisOf(target.min, v)
                                ? axisOf(target.min, v) : axisOf(target.max, v);

        refined = rayAxisCylinderDistance(start, direction, insideAxis, edgeU, edgeV,
                                          axisOf(target.min, insideAxis),
                                          axisOf(target.max, insideAxis), radius,
                                          maxDistance);
    }

    if (refined < 0.0f) {
        /* CORNER — either all three axes are outside, or the edge test ran off
         * the end of its cylinder, which means the contact is on the sphere cap
         * at that end. Both are a ray against a sphere of `radius` sitting on
         * the box's corner. */
        Vec3 corner;
        for (int axis = 0; axis < 3; ++axis) {
            const float value = axisOf(centre, axis);
            const float low = axisOf(target.min, axis);
            const float high = axisOf(target.max, axis);
            setAxis(corner, axis, value < low ? low : (value > high ? high : std::clamp(value, low, high)));
        }
        refined = raySphereDistance(start, direction, corner, radius, maxDistance);

        if (refined < 0.0f) {
            /* The rounded volume is smaller than the box that bounded it, so a
             * box hit with no rounded contact is a genuine miss — the sphere
             * passed the corner diagonally. This is exactly the case the
             * refinement exists to catch. */
            return SweepContact{};
        }

        contact.distance = refined;
        contact.end = start + direction * refined;
        contact.point = corner;
        contact.normal = (contact.end - corner).normalised();
        return contact;
    }

    contact.distance = refined;
    contact.end = start + direction * refined;

    /* The contact on an edge is the centre projected onto the edge line: the
     * axis coordinate is kept, the other two snap to the edge. */
    contact.point = clampToBox(contact.end, target);
    contact.normal = (contact.end - contact.point).normalised();
    return contact;
}

SweepContact sweepCapsule(Vec3 start, float radius, float halfHeight, Vec3 direction,
                          float maxDistance, const Aabb& target)
{
    const float r = std::max(radius, 0.0f);

    /* THE IDENTITY THE WHOLE FUNCTION IS: capsule = segment (+) ball, and an
     * axis-aligned segment (+) a box is that box stretched along the axis. So
     * sweeping a capsule's centre against `target` is sweeping a SPHERE against
     * `target` grown vertically by the segment's half-length. See Shape.hpp.
     *
     * Which means the rounded corners, the edge cylinders and the penetration
     * case are all the sphere path's — already written, already tested, and
     * unable to drift out of step with this because there is nothing here to
     * drift. */
    const float segmentHalf = std::max(halfHeight - r, 0.0f);
    const Aabb stretched = target.expandedBy(Vec3{ 0.0f, segmentHalf, 0.0f });

    SweepContact contact = sweepSphere(start, r, direction, maxDistance, stretched);
    if (!contact.hit) return contact;

    /* The one thing that does need mapping back. `distance`, `end` and `normal`
     * are already the capsule's — the stretch does not move the contact plane,
     * only which part of the capsule reaches it — but `point` came back on the
     * STRETCHED box, which is taller than anything really there. Clamping its
     * height onto the real box turns a contact against the phantom extension
     * into the top or bottom edge it actually corresponds to.
     *
     * Only y needs it: the stretch is vertical, so x and z faces are shared
     * between the two boxes. */
    contact.point.y = std::clamp(contact.point.y, target.min.y, target.max.y);
    return contact;
}

SweepContact sweepShape(const TraceShape& shape, Vec3 start, Vec3 direction,
                        float maxDistance, const Aabb& target)
{
    switch (shape.kind()) {
        case TraceShape::Kind::Sphere:
            return sweepSphere(start, shape.radius(), direction, maxDistance, target);
        case TraceShape::Kind::Capsule:
            return sweepCapsule(start, shape.radius(), shape.halfHeight(), direction,
                                maxDistance, target);
        default:
            /* Ray and Box are the same call — a ray is a box with zero extents,
             * which is the whole reason there is no separate ray path to keep in
             * step. */
            return sweepBox(start, shape.halfExtents(), direction, maxDistance, target);
    }
}

}  // namespace cromwell
