#include "core/border/Band.hpp"

#include <algorithm>

namespace xcom {

int Band::count() const
{
    return static_cast<int>(std::count(members_.begin(), members_.end(), 1));
}

}  // namespace xcom
