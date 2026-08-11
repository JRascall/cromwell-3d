/* Outline.hpp — a closed shape as a ring of points with outward normals.
 *
 * SINGLE RESPONSIBILITY: describe a convex silhouette in the one form the glow
 * and stroke builders can both consume — where the edge is, and which way is
 * out.
 *
 * WHY POSITIONS AND NORMALS RATHER THAN JUST POSITIONS. Everything the kit does
 * to a silhouette is "offset it outward by n pixels": the one-pixel feather
 * that antialiases it, the concentric rings that make its halo, the inward
 * offset that hollows it into a stroke. Recomputing an outward direction from
 * neighbouring points at each of those sites gets the corners wrong in the same
 * way three times — a mitre is not the average of two edge normals unless the
 * corner is shallow, and it is undefined where two points coincide. Building
 * the normals once, where the shape's geometry is actually known, is both
 * cheaper and the only version that is right.
 *
 * CORNERS EMIT SEVERAL POINTS AT THE SAME POSITION. That is deliberate and is
 * what makes a halo wrap a sharp corner smoothly: three coincident points
 * carrying the incoming edge normal, the mitre, and the outgoing edge normal
 * fan the offset ring around the corner instead of shooting it off to a spike.
 * A degenerate zero-length edge in the ring is therefore normal input, not a
 * bug — every consumer here tolerates it.
 *
 * CONVEX ONLY. Every shape in the kit is a capsule, a rounded rectangle or a
 * parallelogram. A concave outline offset outward self-intersects, and handling
 * that properly is a polygon-offsetting library, not a UI helper. If something
 * concave ever needs a halo, decompose it into convex pieces and glow each.
 *
 * WINDING IS CLOCKWISE IN SCREEN SPACE (y down), which is what makes
 * `edge.perpendicular()` point outward. Everything here relies on it.
 *
 * REUSE THE BUFFER. Each build() clears and refills, keeping capacity, so a
 * caller that keeps one Outline across a loop of chips stops allocating after
 * the first.
 */
#pragma once

#include "cromwell/math/Vec2.hpp"
#include "cromwell/ui/core/UiDrawList.hpp"

#include <cstddef>
#include <vector>

namespace cromwell::ui {

class Outline {
public:
    /* Stadium shape: the convex hull of two circles of `radius` centred at
     * `capA` and `capB`, as 2*(capSegments+1) points. Coincident centres
     * degrade cleanly to a circle, which is what a spoke of zero length is. */
    void buildCapsule(Vec2 capA, Vec2 capB, float radius, int capSegments);

    /* Rectangle, optionally with rounded corners. At radius 0 each corner emits
     * `cornerSegments + 1` COINCIDENT points with rotating normals — see the
     * header note — so a square plate's halo still turns the corner. */
    void buildRect(const UiRect& rect, float cornerRadius, int cornerSegments);

    /* The same with a radius PER CORNER, in the order top-left, top-right,
     * bottom-right, bottom-left.
     *
     * Per-corner exists for stacked cards: a panel built from a title strip, a
     * body and a footer is three rectangles, and only the top one should round
     * its top corners and only the bottom one its bottom. Rounding each section
     * uniformly leaves visible notches where they meet. */
    void buildRect(const UiRect& rect, const float cornerRadii[4], int cornerSegments);

    /* Arbitrary convex polygon from its corners, wound clockwise. Each corner
     * emits three coincident points (previous edge normal, mitre, next edge
     * normal), which is what lets a halo round off a sharp corner. Used by the
     * slanted segment-bar chips, whose corners are neither square nor
     * rounded. */
    void buildConvexPolygon(const Vec2* points, int count);

    void clear();

    std::size_t size() const { return positions_.size(); }
    bool empty() const { return positions_.empty(); }

    Vec2 position(std::size_t index) const { return positions_[index]; }
    Vec2 normal(std::size_t index) const { return normals_[index]; }

    /* The point offset outward by `distance` — the operation every consumer
     * performs, named once. */
    Vec2 offsetPosition(std::size_t index, float distance) const
    {
        return positions_[index] + normals_[index] * distance;
    }

private:
    void reset(std::size_t expectedPoints);
    void add(Vec2 position, Vec2 normal);

    std::vector<Vec2> positions_;
    std::vector<Vec2> normals_;
};

}  // namespace cromwell::ui
