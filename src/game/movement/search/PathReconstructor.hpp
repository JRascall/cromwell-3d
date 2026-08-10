/* PathReconstructor.hpp — turn a ReachField into a route.
 *
 * SINGLE RESPONSIBILITY: walk the predecessor chain back from a destination.
 * Separate from Pathfinder because a single search answers many "path to
 * here?" questions as the cursor moves, and none of them re-run the search.
 */
#pragma once

#include "game/movement/search/ReachField.hpp"

#include <vector>

namespace game {


class PathReconstructor {
public:
    /* Cells from start to destination inclusive, or empty when the
     * destination is unreachable or the chain is broken. */
    static std::vector<int> reconstruct(const ReachField& reach, int destIndex, int startIndex);
};

}  // namespace game
