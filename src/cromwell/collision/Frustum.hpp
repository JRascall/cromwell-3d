/* Frustum.hpp — the six planes of a view, and whether a box is inside them.
 *
 * SINGLE RESPONSIBILITY: turn a view-projection matrix into six planes and
 * answer "could this box be visible". Nothing else; it holds no scene, knows
 * no renderable, and is as true of a shadow cascade or an audio audibility
 * test as it is of a camera.
 *
 * ==================== WHY HERE AND NOT IN math/ OR render/ ================
 *
 * It is an intersection test between two shapes, and Aabb — the other half of
 * the test — already lives in this folder. Putting it in math/ would make a
 * maths header depend on a collision one, which is the arrow pointing the wrong
 * way; putting it in render/ would make it unavailable to everything else that
 * asks the same question. A frustum is not a rendering concept. It is the shape
 * a projection matrix describes, and the renderer is only its loudest caller.
 *
 * ==================== THE PLANES COME OUT OF THE MATRIX ===================
 *
 * Gribb and Hartmann's extraction: clip space is defined by six inequalities on
 * the transformed vector, and each inequality is a row-sum of the matrix. Left
 * is `w + x >= 0`, which is row 3 plus row 0 of the view-projection, and so on
 * round the six.
 *
 * WHY THIS RATHER THAN UNPROJECTING EIGHT CORNERS. The corner method needs the
 * matrix inverted, needs to know the depth convention to pick the near corners,
 * and then has to build planes from triples of points — three chances to get a
 * winding backwards, each of which produces a frustum that culls everything or
 * nothing. The row sums need no inverse, no convention and no winding: they are
 * the inequalities themselves, and they are correct for perspective, for
 * orthographic and for the sun's box without a special case. That matters here
 * because this engine has all three, and an orthographic frustum whose near
 * plane came out backwards would hide the entire shadow map.
 *
 * IT IS INDEPENDENT OF THE DEPTH RANGE, which is worth saying because this
 * project has been bitten by exactly that. See the `glClipControl` trap in
 * rhi/MIGRATION.md §5: the engine renders with a 0..1 clip depth and raylib
 * with -1..1, and the two conventions differ only in the NEAR plane's
 * inequality. This class takes the near plane as `w + z >= 0` — the -1..1
 * form — and that is deliberately the CONSERVATIVE choice: under a 0..1
 * convention it describes a volume extending behind the eye, so it can admit a
 * box the hardware will clip and can never reject one the hardware would draw.
 * A culler that errs must err this way; the other direction deletes geometry.
 *
 * ==================== THE TEST IS CONSERVATIVE ON PURPOSE =================
 *
 * `intersects` uses the positive-vertex test: for each plane, take the box
 * corner furthest along the plane's normal and reject only if even THAT corner
 * is outside. It is six dot products, it never rejects a visible box, and it
 * accepts a small number of boxes that are outside — a box straddling two
 * planes' outside half-spaces near a corner passes all six tests individually.
 *
 * That false positive is the right trade and is not worth fixing. The cost of
 * accepting a box that is not visible is one draw the depth test throws away;
 * the cost of rejecting one that is visible is a hole in the world. Every
 * shipped renderer makes this trade, and the exact test — the "n-vertex /
 * p-vertex" formulation — is what makes it six comparisons rather than eight
 * corner transforms.
 */
#pragma once

#include "cromwell/collision/Shape.hpp"
#include "cromwell/math/Mat4.hpp"

namespace cromwell {

class Frustum {
public:
    /* An UNBOUNDED frustum: every box is inside it. The default, so a view that
     * has not been given a matrix yet draws the world rather than nothing —
     * the same "the safe value is the one you get for free" rule CutawayView
     * states, and for the same reason: a culler that defaults to rejecting
     * everything fails as an empty screen with no error anywhere. */
    Frustum() = default;

    /* WHAT ONE PLANE IS: a normal and a distance, in the space the matrix maps
     * FROM. Hand it a projection * view and the planes are in world space;
     * hand it a projection alone and they are in view space. Nothing here
     * decides which, and the caller usually wants the former.
     *
     * NOT NORMALISED, and that is not an omission. The test below only ever
     * compares a signed distance against zero, and scaling a plane equation by
     * a positive constant does not move the plane or change any sign. Real
     * distances would need a normalise per plane at construction — six square
     * roots to serve a comparison that does not read the magnitude. Anything
     * that DOES want a true distance (a near-plane fade, a sphere-radius test)
     * must normalise first, and should say so at its own call site. */
    static Frustum fromViewProjection(const Mat4& viewProjection)
    {
        const float* m = viewProjection.m;

        /* COLUMN-MAJOR, matching Mat4's own layout — so `m[i + 4*j]` is row i
         * of column j, and a "row" of the matrix is a stride-4 walk. Reading
         * this as row-major transposes every plane, which produces a frustum
         * pointing in a plausible but wrong direction: the scene culls hardest
         * when the camera looks along an axis and not at all when it does not,
         * which reads as a level-streaming bug rather than as a transpose. */
        auto row = [m](int i) {
            return Vec4{ m[i], m[i + 4], m[i + 8], m[i + 12] };
        };

        const Vec4 x = row(0);
        const Vec4 y = row(1);
        const Vec4 z = row(2);
        const Vec4 w = row(3);

        Frustum result;
        result.planes_[0] = w + x;   /* left   */
        result.planes_[1] = w - x;   /* right  */
        result.planes_[2] = w + y;   /* bottom */
        result.planes_[3] = w - y;   /* top    */
        result.planes_[4] = w + z;   /* near — see the header on the convention */
        result.planes_[5] = w - z;   /* far    */
        result.bounded_ = true;
        return result;
    }

    bool bounded() const { return bounded_; }

    /* COULD THIS BOX BE VISIBLE. Conservative: false means definitely not, true
     * means possibly. See the header on why that asymmetry is the correct one.
     *
     * An EMPTY box — max below min on any axis, which is what a renderable with
     * no geometry has — is reported as not visible. That is the honest answer
     * and it is also the useful one: it means an unfilled bounds field culls
     * its renderable away rather than making it visible from everywhere, so the
     * mistake shows up as "my thing does not draw" with one obvious cause
     * rather than as a mysterious cost in every view. */
    bool intersects(const Aabb& box) const
    {
        if (!bounded_) return !box.empty();
        if (box.empty()) return false;

        for (const Vec4& plane : planes_) {
            /* THE CORNER FURTHEST ALONG THE PLANE'S NORMAL, picked per axis by
             * the sign of that component. If even this corner is behind the
             * plane, every corner is, and the box is wholly outside — which is
             * the only case a conservative culler may reject on. */
            const float px = plane.x >= 0.0f ? box.max.x : box.min.x;
            const float py = plane.y >= 0.0f ? box.max.y : box.min.y;
            const float pz = plane.z >= 0.0f ? box.max.z : box.min.z;

            if (plane.x * px + plane.y * py + plane.z * pz + plane.w < 0.0f) return false;
        }
        return true;
    }

private:
    /* SIX PLANES AS Vec4s — xyz is the normal, w the constant, which is the
     * shape every graphics text writes a plane in and the shape the dot product
     * above wants. */
    Vec4 planes_[6]{};

    /* Whether fromViewProjection has been called. A separate flag rather than a
     * sentinel in the planes, because "no planes" and "six degenerate planes"
     * are different things and only one of them should draw the world. */
    bool bounded_ = false;
};

}  // namespace cromwell
