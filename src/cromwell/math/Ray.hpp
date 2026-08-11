/* Ray.hpp — a point and a direction, going somewhere.
 *
 * SINGLE RESPONSIBILITY: be the two vectors every trace, pick and projection
 * query passes around.
 *
 * WHY THE ENGINE HAS ITS OWN, when raylib ships a `Ray` with the same two
 * fields. The same reason Vec3 exists rather than Vector3, stated at length in
 * Vec3.hpp and worth repeating here because a ray is the type that crosses the
 * most boundaries:
 *
 *   THE HEADLESS HALF CANNOT INCLUDE raylib.h. Every trace in
 *   cromwell/collision, every picker, every line-of-sight test is arithmetic
 *   over a ray, and all of it must build and run without linking a window
 *   library. A ray typed as raylib's drags a GL context's worth of expectations
 *   into the simulation and its tests.
 *
 *   AND A RAY IS NOT A RENDERING CONCEPT. It is the shape of a question —
 *   "what is along this line" — asked by movement, by cover scoring, by AI
 *   visibility, by the cursor. Borrowing the renderer's spelling for it puts a
 *   rendering library in the interface of code that does not draw.
 *
 * THE CONVERSION IS FREE. Both layouts are two three-float vectors in the same
 * order; see math/RaylibInterop.hpp, which asserts it rather than assuming.
 *
 * DIRECTION IS EXPECTED TO BE UNIT LENGTH and is not enforced, exactly as Quat
 * does not enforce normalisation. Distances along a ray are metres only if it
 * is, and every trace in this engine documents that it measures in metres —
 * `normalised()` below is there so a caller can say so cheaply. Enforcing it in
 * the constructor would mean a square root on every ray built inside a loop,
 * which is precisely where they are built.
 *
 * PUBLIC MEMBERS, the same deliberate exception Vec2 and Vec3 document: there is
 * no pair of vectors that is an invalid ray.
 */
#pragma once

#include "cromwell/math/Vec3.hpp"

namespace cromwell {

struct Ray {
    Vec3 origin;
    Vec3 direction{ 0.0f, 0.0f, 1.0f };

    static Ray between(Vec3 from, Vec3 to)
    {
        return Ray{ from, (to - from).normalised() };
    }

    /* The point `distance` along it. Metres, if the direction is unit length. */
    Vec3 at(float distance) const { return origin + direction * distance; }

    Ray normalised() const { return Ray{ origin, direction.normalised() }; }
};

}  // namespace cromwell
