#include "game/border/loop/LoopSet.hpp"

#include <algorithm>

namespace game {


int LoopSet::closedLoopCount() const
{
    return static_cast<int>(std::count_if(
        loops_.begin(), loops_.end(), [](const Loop& l) { return l.closed; }));
}

}  // namespace game
