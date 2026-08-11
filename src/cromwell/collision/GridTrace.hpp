/* GridTrace.hpp — walking a swept shape through a lattice of unit cells,
 * visiting exactly the cells it touches, in order.
 *
 * SINGLE RESPONSIBILITY: enumerate candidate cells. It does not know what is IN
 * a cell, what a layer is, or what counts as a hit — the visitor decides all
 * three. That is what makes it engine code: a tile game, a voxel terrain and an
 * RTS heightfield all have a grid, and none of them agree about what fills it.
 *
 * ================== WHY THIS AND NOT A FIXED-STEP MARCH ====================
 *
 * SAMPLING A RAY EVERY FEW CENTIMETRES IS BOTH SLOWER AND WRONG, and it is what
 * this replaces. Wrong, because a step larger than the thinnest geometry can
 * jump clean over it — a grazing ray past a 9 cm wall at a 3 cm step lands on
 * the floor behind it, and the fix is always to shrink the step, which makes it
 * slower without ever making it correct. Slower, because a 140 m trace at 1 cm
 * is fourteen thousand samples where the cells actually crossed number in the
 * dozens.
 *
 * A DDA visits each cell ONCE, in order, with no step size to tune and nothing
 * to miss. Amanatides & Woo, 1987 — the standard, and it has not been improved
 * on for axis-aligned grids.
 *
 * ===================== HOW A BOX SWEEP IS THE SAME WALK ====================
 *
 * A RAY OCCUPIES ONE CELL AT A TIME; A SWEPT BOX OCCUPIES A CUBOID OF THEM. So
 * the traversal tracks a RANGE of cells per axis rather than a single index, and
 * a step on one axis enters a whole SLAB of new cells rather than one.
 *
 * The ranges are recomputed from the box's position each time it steps, which is
 * what keeps them exact and — the part that matters — is what makes each cell
 * come up exactly once. Because the box moves monotonically, a cell index once
 * left is never re-entered, and the index a step enters was by definition not in
 * range before, so no cell can appear in two slabs. No visited-set, no
 * deduplication, no allocation.
 *
 * A RAY IS THE ZERO-EXTENT CASE and runs the identical code: its ranges are one
 * cell wide, its slabs are single cells, and it degenerates exactly into
 * Amanatides & Woo. One implementation, so the ray path cannot rot while the box
 * path is maintained.
 *
 * ====================== THE ORDERING GUARANTEE, EXACTLY ====================
 *
 * SLABS ARRIVE IN INCREASING DISTANCE. CELLS WITHIN A SLAB DO NOT.
 *
 * This matters and is easy to get wrong. A visitor must NOT stop at the first
 * cell it finds something in — for a box, several cells enter together and a
 * nearer contact may still be coming in the same slab. What it may do is stop
 * once the SLAB's entry distance exceeds the best contact found so far, which is
 * why that distance is passed in: it is a lower bound on every contact in this
 * slab and in every slab after it.
 *
 * For a ray the two rules coincide, because a slab is one cell. Write the
 * visitor for the general case anyway; the day someone passes a box to a trace
 * written for a ray should not be the day the answer quietly becomes wrong.
 *
 * ========================================================================
 *
 * HEADER-ONLY TEMPLATE, so the visitor inlines. The visitor is called once per
 * candidate cell — a genuine hot loop, thousands of calls a frame across the
 * traces a scene runs — and an indirect call per cell would dominate the
 * arithmetic it is wrapped around. This is one of the few places in the engine
 * where that is worth a template rather than a std::function.
 *
 * NO ALLOCATION AND NO PROFILER ZONE. Per CLAUDE.md: a zone costs ~40 ns, which
 * is ruinous inside a per-cell loop. The SYSTEM that runs the traces gets the
 * zone; this gets a benchmark if it ever needs one.
 *
 * CELLS ARE UNIT CUBES AT INTEGER COORDINATES. A world with a different cell
 * size scales its inputs on the way in — one multiply at the call site, against
 * a division per axis per step if the scale were carried through here.
 */
#pragma once

#include "cromwell/collision/Shape.hpp"
#include "cromwell/math/Vec3.hpp"

#include <cmath>
#include <limits>

namespace cromwell {

/* What a visitor is told about a candidate cell.
 *
 * ONE-SHOT DATA CARRIER, and by value because it is three ints and a float. */
struct GridCell {
    int x = 0;
    int y = 0;
    int z = 0;

    /* Distance at which the shape first reached this cell's SLAB. A lower bound
     * on any contact here or beyond — see the ordering guarantee in the header.
     * Zero for the cells the shape starts inside. */
    float slabDistance = 0.0f;
};

namespace detail {

inline float componentOf(Vec3 v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

}  // namespace detail

/* Walks the grid, calling `visit(const GridCell&)` for each cell the swept shape
 * touches. The visitor returns true to stop the walk.
 *
 * `direction` must be unit length; `maxDistance` is in cells, which are metres
 * wherever a cell is a metre. `halfExtents` of zero gives a pure ray.
 *
 * `maxCells` is a runaway guard rather than a tuning knob. A direction that is
 * not quite normalised, or a maxDistance of infinity, would otherwise walk until
 * the integer coordinates wrapped; this turns that into a truncated answer,
 * which is recoverable, instead of a hang, which is not. The default is far
 * above any real trace — a 256-cell map's diagonal is under 500 cells. */
template <typename Visit>
void traceGrid(Vec3 start, Vec3 direction, float maxDistance, Vec3 halfExtents,
               Visit&& visit, int maxCells = 4096)
{
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    constexpr float kEpsilon = 1.0e-6f;

    int low[3];
    int high[3];
    int step[3];
    float tMax[3];
    float tDelta[3];

    for (int axis = 0; axis < 3; ++axis) {
        const float centre = detail::componentOf(start, axis);
        const float half = detail::componentOf(halfExtents, axis);
        const float d = detail::componentOf(direction, axis);

        low[axis] = static_cast<int>(std::floor(centre - half));
        high[axis] = static_cast<int>(std::floor(centre + half));

        if (std::abs(d) < kEpsilon) {
            step[axis] = 0;
            tMax[axis] = kInfinity;
            tDelta[axis] = kInfinity;
            continue;
        }

        step[axis] = d > 0.0f ? 1 : -1;
        tDelta[axis] = 1.0f / std::abs(d);

        /* Measured on the LEADING FACE, not on the centre. The face is what
         * crosses into a new cell; using the centre would enter each cell half an
         * extent late and miss the geometry the box's front had already reached
         * — a wall clipped through at exactly half the box's width, which reads
         * as "collision is inconsistent" rather than as an off-by-one. */
        const float leading = centre + static_cast<float>(step[axis]) * half;
        const float boundary = step[axis] > 0 ? std::floor(leading) + 1.0f
                                              : std::floor(leading);
        tMax[axis] = (boundary - leading) / d;
    }

    /* The cells the shape starts in — the whole cuboid for a box, one cell for a
     * ray. They are at distance zero and must be reported: a sweep that begins
     * already inside geometry is a real and common state, and skipping the
     * starting cells is how a character that has sunk into a floor never learns
     * it needs pushing out. */
    for (int x = low[0]; x <= high[0]; ++x) {
        for (int y = low[1]; y <= high[1]; ++y) {
            for (int z = low[2]; z <= high[2]; ++z) {
                if (visit(GridCell{ x, y, z, 0.0f })) return;
            }
        }
    }

    for (int visited = 0; visited < maxCells; ++visited) {
        /* The axis whose leading face crosses a boundary soonest. */
        int axis = 0;
        if (tMax[1] < tMax[axis]) axis = 1;
        if (tMax[2] < tMax[axis]) axis = 2;

        const float distance = tMax[axis];
        if (distance > maxDistance || distance == kInfinity) return;

        int entered = 0;
        if (step[axis] > 0) {
            high[axis] += 1;
            entered = high[axis];
        } else {
            low[axis] -= 1;
            entered = low[axis];
        }

        /* The perpendicular ranges, recomputed from where the shape actually is
         * now. Exact rather than incrementally maintained, and cheap — two floors
         * per axis against the alternative of tracking a trailing-face crossing
         * per axis as well. See the header on why this also makes duplicate
         * visits impossible. */
        const Vec3 centre = start + direction * distance;
        for (int other = 0; other < 3; ++other) {
            if (other == axis) continue;
            const float value = detail::componentOf(centre, other);
            const float half = detail::componentOf(halfExtents, other);
            low[other] = static_cast<int>(std::floor(value - half));
            high[other] = static_cast<int>(std::floor(value + half));
        }

        /* The newly entered slab: one cell thick on `axis`, the shape's current
         * footprint on the other two. */
        const int loA = axis == 0 ? entered : low[0];
        const int hiA = axis == 0 ? entered : high[0];
        const int loB = axis == 1 ? entered : low[1];
        const int hiB = axis == 1 ? entered : high[1];
        const int loC = axis == 2 ? entered : low[2];
        const int hiC = axis == 2 ? entered : high[2];

        for (int x = loA; x <= hiA; ++x) {
            for (int y = loB; y <= hiB; ++y) {
                for (int z = loC; z <= hiC; ++z) {
                    if (visit(GridCell{ x, y, z, distance })) return;
                }
            }
        }

        tMax[axis] += tDelta[axis];
    }
}

/* The same walk, taking a TraceShape. A sphere walks its bounding box — the
 * rounded corners only ever make the true volume SMALLER, so the box's cell set
 * is a superset and the per-cell contact test rejects the difference. See
 * Intersect.hpp. */
template <typename Visit>
void traceGrid(const TraceShape& shape, Vec3 start, Vec3 direction, float maxDistance,
               Visit&& visit, int maxCells = 4096)
{
    traceGrid(start, direction, maxDistance, shape.halfExtents(),
              static_cast<Visit&&>(visit), maxCells);
}

}  // namespace cromwell
