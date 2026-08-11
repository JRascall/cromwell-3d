/* Intersect.hpp — the closed-form contact tests everything else is built from.
 *
 * SINGLE RESPONSIBILITY: given one moving shape and one static box, say when
 * and where they first touch. No traversal, no filtering, no world — those are
 * GridTrace's and the caller's.
 *
 * FREE FUNCTIONS, NOT A CLASS, because there is no state: each of these is a
 * formula. Kept together in one unit because they share the slab test and the
 * conventions around it, and splitting them would mean either duplicating that
 * or exporting it.
 *
 * ===================== THE ONE IDEA: MINKOWSKI EXPANSION ===================
 *
 * A BOX OF HALF-EXTENTS h SWEEPING AGAINST A STATIC BOX B TOUCHES IT EXACTLY
 * WHEN ITS CENTRE ENTERS B EXPANDED BY h. So a box sweep is a ray test against a
 * bigger box — the same slab arithmetic, no extra cost, and no second algorithm
 * to keep in step with the first. Everything below follows from that one line.
 *
 * A sphere gets the same treatment for finding CANDIDATES and then a correction,
 * because the expanded box has square corners where the true swept volume has
 * round ones. The correction is exact and it runs only when the contact actually
 * lands in a corner or edge region — cheap bound first, expensive test on the
 * survivors, which is the ranking rule from CLAUDE.md applied to geometry.
 *
 * ============================== CONVENTIONS ================================
 *
 * DIRECTIONS ARE UNIT LENGTH and distances are in metres, not in a 0..1
 * fraction of some segment. A fraction is the more common convention in physics
 * engines and it is the wrong one here: a trace's length is decided by the
 * caller, several of these tests are chained along one trace, and re-normalising
 * a fraction against a changing length at every step is how an off-by-a-cell
 * creeps in. The caller divides once at the end if it wants a fraction.
 *
 * NORMALS POINT OUT OF THE STATIC SURFACE, toward where the moving shape came
 * from. Always unit length, including in the penetrating case.
 *
 * A GRAZING CONTACT IS NOT A HIT. A shape sliding exactly along a wall touches
 * it at every point, and reporting that as a collision makes a character catch
 * on flat surfaces. The slab test is therefore strict on the exit side, which
 * costs a contact only when the approach is exactly parallel — where there is
 * nothing to resolve anyway.
 */
#pragma once

#include "cromwell/collision/Shape.hpp"
#include "cromwell/math/Vec3.hpp"

namespace cromwell {

/* One contact between a moving shape and one static box.
 *
 * ONE-SHOT DATA CARRIER. Smaller than TraceHit on purpose: this layer knows
 * nothing about layers, ids or cells, so it reports only geometry and the trace
 * above fills in the rest. */
struct SweepContact {
    bool hit = false;

    /* Metres travelled before touching. Zero when already overlapping. */
    float distance = 0.0f;

    /* Out of the static surface. Unit length. When `startPenetrating`, this is
     * the shallowest direction that separates them instead — the push a
     * depenetration step should apply. */
    Vec3 normal;

    /* On the static box's surface, where the two touch. */
    Vec3 point;

    /* The moving shape's centre at the moment of contact. */
    Vec3 end;

    /* They were already overlapping at the start. See TraceHit.hpp on why this
     * is a separate state rather than a hit at distance zero. */
    bool startPenetrating = false;
};

/* Sweeps an axis-aligned box against a static one.
 *
 * `halfExtents` of zero makes this a pure ray test, and that is not a special
 * case in the implementation — it is the same arithmetic with a zero expansion,
 * which is what keeps the two paths from drifting apart. */
SweepContact sweepBox(Vec3 start, Vec3 halfExtents, Vec3 direction, float maxDistance,
                      const Aabb& target);

/* Sweeps a sphere against a static box. Exact, including the rounded edges and
 * corners — see the header on how, and on why it is not simply a box sweep. */
SweepContact sweepSphere(Vec3 start, float radius, Vec3 direction, float maxDistance,
                         const Aabb& target);

/* Sweeps an UPRIGHT capsule against a static box. Exact.
 *
 * `halfHeight` is measured to the outside of the caps — see TraceShape::capsule
 * for why that convention, and Shape.hpp's header for why this is a sphere sweep
 * against a stretched box rather than an algorithm of its own. */
SweepContact sweepCapsule(Vec3 start, float radius, float halfHeight, Vec3 direction,
                          float maxDistance, const Aabb& target);

/* Dispatches on the shape. The entry point the trace layer uses, so a caller
 * that switched shapes does not also have to switch functions. */
SweepContact sweepShape(const TraceShape& shape, Vec3 start, Vec3 direction,
                        float maxDistance, const Aabb& target);

/* ---- the pieces, exposed because they are independently useful ---------- */

/* Nearest intersection of a ray with a sphere, in metres, or a negative number
 * for a miss. A ray starting INSIDE returns 0 — which is the honest answer and
 * is why the callers above test for penetration separately rather than reading
 * a zero as "touching at the origin". */
float raySphereDistance(Vec3 origin, Vec3 direction, Vec3 centre, float radius,
                        float maxDistance);

/* Nearest intersection with an axis-aligned capsule-free cylinder: the round
 * edge of an expanded box. `axis` is 0, 1 or 2; the cylinder runs along it
 * between `axisMin` and `axisMax`, centred at `centreU`/`centreV` in the other
 * two axes taken in increasing order. Negative for a miss.
 *
 * Specialised to an axis rather than taking two endpoints, because every edge
 * this is used on is axis-aligned and the general form would solve a 3D
 * quadratic where this solves a 2D one. */
float rayAxisCylinderDistance(Vec3 origin, Vec3 direction, int axis, float centreU,
                              float centreV, float axisMin, float axisMax, float radius,
                              float maxDistance);

}  // namespace cromwell
