/* PathPoint.hpp — one vertex of an articulated route.
 *
 * SINGLE RESPONSIBILITY: carry a world position plus the cell that produced
 * it. The articulation (risers, hops, teleport arcs) is added by whoever
 * builds the list; this only says where a point is.
 */
#pragma once

namespace xcom {

struct PathPoint {
    float x = 0.0f;
    float y = 0.0f;
    float height = 0.0f;
    int   cell = 0;
};

}  // namespace xcom
