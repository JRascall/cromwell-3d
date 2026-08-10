#include "core/los/VisibilityField.hpp"

#include <algorithm>

namespace xcom {

int VisibilityField::countVisible() const
{
    return static_cast<int>(std::count_if(
        grades_.begin(), grades_.end(),
        [](Visibility g) { return g != Visibility::None; }));
}

int VisibilityField::countPeekOnly() const
{
    return static_cast<int>(std::count(grades_.begin(), grades_.end(), Visibility::PeekOnly));
}

}  // namespace xcom
