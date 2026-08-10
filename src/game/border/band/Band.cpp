#include "game/border/band/Band.hpp"

#include <algorithm>

namespace game {


int Band::count() const
{
    return static_cast<int>(std::count(members_.begin(), members_.end(), 1));
}

}  // namespace game
