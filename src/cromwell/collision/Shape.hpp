/* Shape.hpp — the volumes a trace can be, and the box everything reduces to.
 *
 * SINGLE RESPONSIBILITY: describe the swept volume of a query. It holds no
 * position — the position and direction are the trace's arguments, and a shape
 * that carried its own would be an object rather than a query parameter.
 *
 * FOUR SHAPES.
 *
 *   RAY     — zero extent. The line trace everything starts with.
 *   BOX     — an axis-aligned box, given as half-extents.
 *   SPHERE  — a radius.
 *   CAPSULE — a radius and a half-height, standing UPRIGHT. The character
 *             controller's shape, and the one Unreal reaches for most.
 *
 * ============ WHY THE CAPSULE IS UPRIGHT, AND WHY THAT IS ENOUGH ===========
 *
 * BOTH ENGINES' CHARACTER CONTROLLERS USE AN UPRIGHT CAPSULE AND NOTHING ELSE.
 * Unreal's UCapsuleComponent is defined by a half-height and a radius about the
 * vertical axis; Unity's CharacterController is the same. That is not a
 * simplification they regretted — a character that tilts its collision volume
 * as it looks up is a character that clips its own head into the ceiling.
 *
 * The restriction also buys the entire implementation. A general oriented
 * capsule swept against a box needs segment tests against a Minkowski sum with
 * cylindrical edges and spherical corners on two unrelated axes, and the cases
 * that go wrong are precisely the ones that matter — a foot catching a step
 * edge. An UPRIGHT one has an exact closed form, given below, and it reuses the
 * sphere path rather than being a fourth algorithm to keep in step.
 *
 * NO ORIENTED BOX. A rotated box sweep is a different algorithm — the grid
 * acceleration stops applying, because a rotated box has no integer cell range
 * — and nothing in a tile game, an RTS or a hitscan FPS needs one. Rotated
 * volume queries are a case for a real physics broadphase, which is Jolt's job
 * when Jolt arrives.
 *
 * ============== WHY A SPHERE IS A BOX UNTIL IT IS NOT ==================
 *
 * THE KEY FACT THE WHOLE TRACE LAYER RESTS ON: sweeping a box of half-extents h
 * against a static box B is exactly a RAY against B expanded by h. That is the
 * Minkowski sum, and it means a box sweep costs the same as a ray — no
 * per-shape traversal, no separate code path, one algorithm.
 *
 * A sphere of radius r is bounded by a box of half-extent r, so the same
 * machinery finds every cell a sphere could possibly touch. Where they differ
 * is the corners: the expanded box has square corners and the true swept volume
 * has round ones, so a box sweep reports a hit slightly early on a diagonal
 * approach to an edge. That difference is refined away by an exact test against
 * the rounded region, and ONLY for the hits that actually land in a corner —
 * which is CLAUDE.md's cull-cheaply-then-test-expensively rule: the cheap box
 * bound rejects almost everything, and the expensive rounded test runs on the
 * handful that survive. See Intersect.hpp.
 *
 * ================= AND WHY A CAPSULE IS A SPHERE, EXACTLY =================
 *
 * THE FACT THE WHOLE CAPSULE IMPLEMENTATION RESTS ON:
 *
 *     capsule = segment (+) ball
 *
 * so, for any box B,
 *
 *     capsule (+) B = ball (+) (segment (+) B)
 *
 * and because the segment is axis-aligned, `segment (+) B` is just B STRETCHED
 * along that axis by the segment's half-length. Which makes an upright capsule
 * sweep exactly a SPHERE sweep against the target box extended vertically by
 * (halfHeight - radius). No new traversal, no new contact solver, no second set
 * of corner cases — the one already-tested rounded-corner path answers it.
 *
 * That identity is why `segmentHalf` is a named accessor here rather than
 * arithmetic at the call site: it is the quantity the box gets stretched by, and
 * getting it wrong (using halfHeight, forgetting to subtract the radius)
 * produces a capsule that is too tall by exactly one radius — which reads as a
 * character that hovers, and is invisible in any single screenshot.
 */
#pragma once

#include "cromwell/math/Vec3.hpp"

#include <algorithm>

namespace cromwell {

/* An axis-aligned box, as two corners.
 *
 * PUBLIC MEMBERS, the same exception Vec3 documents: it is a mathematical value
 * and there is no pair of corners that is an invalid one — an inverted box is a
 * meaningful empty box, and `overlaps` says so rather than asserting. */
struct Aabb {
    Vec3 min;
    Vec3 max;

    static Aabb fromCentre(Vec3 centre, Vec3 halfExtents)
    {
        return { centre - halfExtents, centre + halfExtents };
    }

    /* The cell at integer coordinates, in a lattice of unit cells. The shape the
     * grid trace works in. */
    static Aabb unitCell(int x, int y, int z)
    {
        const Vec3 corner{ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
        return { corner, corner + Vec3::one() };
    }

    Vec3 centre() const { return (min + max) * 0.5f; }
    Vec3 size() const { return max - min; }
    Vec3 halfExtents() const { return (max - min) * 0.5f; }

    bool empty() const { return max.x < min.x || max.y < min.y || max.z < min.z; }

    bool contains(Vec3 point) const
    {
        return point.x >= min.x && point.x <= max.x
            && point.y >= min.y && point.y <= max.y
            && point.z >= min.z && point.z <= max.z;
    }

    bool overlaps(const Aabb& other) const
    {
        return min.x <= other.max.x && max.x >= other.min.x
            && min.y <= other.max.y && max.y >= other.min.y
            && min.z <= other.max.z && max.z >= other.min.z;
    }

    /* THE MINKOWSKI SUM, and the reason a box sweep is not its own algorithm.
     * See the header. */
    Aabb expandedBy(Vec3 halfExtents) const
    {
        return { min - halfExtents, max + halfExtents };
    }

    Aabb merged(const Aabb& other) const
    {
        return { Vec3{ std::min(min.x, other.min.x), std::min(min.y, other.min.y),
                       std::min(min.z, other.min.z) },
                 Vec3{ std::max(max.x, other.max.x), std::max(max.y, other.max.y),
                       std::max(max.z, other.max.z) } };
    }
};

class TraceShape {
public:
    enum class Kind { Ray, Box, Sphere, Capsule };

    static TraceShape ray() { return TraceShape{ Kind::Ray, Vec3::zero(), 0.0f, 0.0f }; }

    static TraceShape box(Vec3 halfExtents)
    {
        /* A negative half-extent would expand cells inward and make the trace
         * miss geometry it is inside of. Clamped rather than asserted, because a
         * half-extent is frequently computed (a body's width times a scale) and
         * a zero is a ray, which is a sane thing to degenerate to. */
        return TraceShape{ Kind::Box,
                           Vec3{ std::max(halfExtents.x, 0.0f), std::max(halfExtents.y, 0.0f),
                                 std::max(halfExtents.z, 0.0f) },
                           0.0f, 0.0f };
    }

    static TraceShape sphere(float radius)
    {
        const float r = std::max(radius, 0.0f);
        return TraceShape{ Kind::Sphere, Vec3{ r, r, r }, r, r };
    }

    /* An UPRIGHT capsule: `halfHeight` is measured to the outside of the caps,
     * the way Unreal's UCapsuleComponent and Unity's CharacterController both
     * measure it — so a 1.8 m tall character is halfHeight 0.9, not 0.9 plus a
     * radius. Getting that convention backwards makes every character two radii
     * too tall, which is the sort of thing that survives review.
     *
     * A halfHeight below the radius is not a capsule; it is clamped up, which
     * makes it a sphere. That is what Unreal does with the same input, and it
     * beats a shape whose caps overlap and whose segment has negative length. */
    static TraceShape capsule(float radius, float halfHeight)
    {
        const float r = std::max(radius, 0.0f);
        const float half = std::max(halfHeight, r);
        return TraceShape{ Kind::Capsule, Vec3{ r, half, r }, r, half };
    }

    Kind kind() const { return kind_; }

    /* The bounding half-extents, whatever the shape. A ray's are zero, a
     * sphere's are its radius on each axis, a capsule's are its radius across
     * and its half-height up — which is what lets the traversal treat all four
     * identically and only the contact test differ. */
    Vec3 halfExtents() const { return halfExtents_; }
    float radius() const { return radius_; }
    float halfHeight() const { return halfHeight_; }

    /* Half the length of the capsule's INNER SEGMENT — the distance between the
     * two cap centres, halved. The amount a target box is stretched vertically
     * to turn a capsule sweep into a sphere sweep; see the header. Zero for
     * every other shape, which makes that stretch a no-op and lets the capsule
     * path be written once for all of them. */
    float segmentHalf() const
    {
        return kind_ == Kind::Capsule ? halfHeight_ - radius_ : 0.0f;
    }

    bool isRay() const { return kind_ == Kind::Ray; }

private:
    TraceShape(Kind kind, Vec3 halfExtents, float radius, float halfHeight)
        : halfExtents_(halfExtents), radius_(radius), halfHeight_(halfHeight), kind_(kind) {}

    Vec3  halfExtents_;
    float radius_ = 0.0f;
    float halfHeight_ = 0.0f;
    Kind  kind_ = Kind::Ray;
};

}  // namespace cromwell
