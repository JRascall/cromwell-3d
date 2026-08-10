#include "core/los/EyeSet.hpp"

#include "core/lattice/Constants.hpp"

namespace xcom {

std::vector<Eye> EyeSet::eyesFor(const Cell& cell) const
{
    std::vector<Eye> eyes;

    eyes.push_back({ static_cast<float>(cell.x) + 0.5f,
                     static_cast<float>(cell.y) + 0.5f,
                     terrain_.centerHeight(cell) + kEyeHeight,
                     false });

    for (const Cell& peek : peeks_.peekPositions(cell)) {
        eyes.push_back({ static_cast<float>(peek.x) + 0.5f,
                         static_cast<float>(peek.y) + 0.5f,
                         terrain_.centerHeight(peek) + kEyeHeight,
                         true });
    }
    return eyes;
}

}  // namespace xcom
