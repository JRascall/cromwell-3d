#include "core/border/LoopSet.hpp"

#include <algorithm>

namespace xcom {

int LoopSet::closedLoopCount() const
{
    return static_cast<int>(std::count_if(
        loops_.begin(), loops_.end(), [](const Loop& l) { return l.closed; }));
}

}  // namespace xcom
