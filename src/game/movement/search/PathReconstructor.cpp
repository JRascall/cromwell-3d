#include "game/movement/search/PathReconstructor.hpp"

#include <algorithm>

namespace game {


std::vector<int> PathReconstructor::reconstruct(const ReachField& reach,
                                                int destIndex,
                                                int startIndex)
{
    std::vector<int> path;
    if (destIndex < 0 || destIndex >= reach.size()) return path;
    if (!reach.isReachable(destIndex)) return path;

    int cell = destIndex;
    while (cell != startIndex) {
        /* a chain longer than the grid means the field is malformed */
        if (static_cast<int>(path.size()) >= reach.size()) return {};
        path.push_back(cell);
        cell = reach.previous(cell);
        if (cell < 0) return {};
    }
    path.push_back(startIndex);

    std::reverse(path.begin(), path.end());   /* start first */
    return path;
}

}  // namespace game
